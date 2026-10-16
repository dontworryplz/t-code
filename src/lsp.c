/*
 * Language servers shared by all editors. Without this, every micro started its own clangd,
 * and each one parsed the same headers and indexed the same project again (gigabytes of RAM).
 *
 * micro's lsp plugin starts t-code-lsp (src/lsp/main.c) instead of the server when $TCODE_LSP_CONNECT
 * is set. It connects to the socket in $TCODE_LSP and sends "<cwd>\t<server>\t<arg>...\n"; here one
 * server runs per such line, and the editors' JSON-RPC messages are passed between them:
 *   - request ids are renumbered, so the editors' ids (each counts from 0) never clash, and the
 *     responses go back to the editor that asked, with its own id;
 *   - initialize goes to the server once; later editors get the same answer from here;
 *   - diagnostics go to the editors that opened the file; when an editor goes, the files it
 *     opened are closed (didClose) unless another editor has them open;
 *   - requests from the server (configuration, progress) are answered here.
 * A server without editors stops after IDLE_SECONDS, so closing and reopening a tab does not
 * start over. Servers read EOF on stdin when t-code goes, and exit.
 */
#include "app.h"

#include <gio/gunixsocketaddress.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define IDLE_SECONDS 60
#define MAX_HEADER 65536

/* Messages framed with Content-Length over a pair of streams ---------------------------------- */

typedef struct Channel Channel;

struct Channel {
    int refs;                /* the owner's, and one per read or write in flight */
    GInputStream *in;
    GOutputStream *out;
    GObject *keep;           /* the connection or process the streams belong to */
    GCancellable *cancel;
    GByteArray *buf;         /* read, not yet a whole message */
    char chunk[65536];
    GQueue writes;           /* GBytes waiting their turn */
    GBytes *writing;
    gboolean closed;
    gboolean header;         /* a line comes before the messages (clients) */
    void (*line)(gpointer owner, const char *line);
    void (*message)(gpointer owner, const char *body);
    void (*on_closed)(gpointer owner); /* the other side went: the owner calls channel_close() */
    gpointer owner;
};

static void channel_unref(Channel *ch)
{
    if (--ch->refs > 0)
        return;
    g_queue_clear_full(&ch->writes, (GDestroyNotify)g_bytes_unref);
    if (G_IS_IO_STREAM(ch->keep)) {
        g_io_stream_close(G_IO_STREAM(ch->keep), NULL, NULL);
    } else {
        g_output_stream_close(ch->out, NULL, NULL);
        g_input_stream_close(ch->in, NULL, NULL);
    }
    g_object_unref(ch->keep);
    g_object_unref(ch->cancel);
    g_byte_array_unref(ch->buf);
    g_free(ch);
}

/* By the owner: drops its reference; no on_closed. */
static void channel_close(Channel *ch)
{
    if (!ch->closed) {
        ch->closed = TRUE;
        g_cancellable_cancel(ch->cancel);
    }
    channel_unref(ch);
}

static void peer_closed(Channel *ch)
{
    if (ch->closed)
        return;
    ch->closed = TRUE;
    g_cancellable_cancel(ch->cancel);
    ch->on_closed(ch->owner);
}

/* The length from a message header ("Content-Length: 123\r\n..."); -1 without one. */
static long content_length(const char *header, gsize len)
{
    char *text = g_strndup(header, len);
    char **lines = g_strsplit(text, "\r\n", -1);
    long size = -1;

    for (int i = 0; lines[i]; i++)
        if (g_ascii_strncasecmp(lines[i], "Content-Length:", 15) == 0)
            size = strtol(lines[i] + 15, NULL, 10);
    g_strfreev(lines);
    g_free(text);
    return size;
}

static void parse(Channel *ch)
{
    while (!ch->closed) {
        const char *data = (const char *)ch->buf->data;
        gsize len = ch->buf->len;

        if (ch->header) {
            const char *nl = memchr(data, '\n', len);
            if (!nl) {
                if (len > MAX_HEADER)
                    peer_closed(ch);
                return;
            }
            char *line = g_strndup(data, nl - data);
            g_byte_array_remove_range(ch->buf, 0, nl - data + 1);
            ch->header = FALSE;
            ch->line(ch->owner, line);
            g_free(line);
            continue;
        }
        const char *end = NULL;
        for (gsize i = 0; i + 4 <= len && !end; i++)
            if (memcmp(data + i, "\r\n\r\n", 4) == 0)
                end = data + i;
        if (!end) {
            if (len > MAX_HEADER)
                peer_closed(ch);
            return;
        }
        gsize head = end - data + 4;
        long size = content_length(data, end - data);
        if (size < 0) {
            peer_closed(ch);
            return;
        }
        if (len < head + (gsize)size)
            return;
        char *body = g_strndup(data + head, size);
        g_byte_array_remove_range(ch->buf, 0, head + size);
        ch->message(ch->owner, body);
        g_free(body);
    }
}

static void channel_read(Channel *ch);

static void on_read(GObject *source, GAsyncResult *result, gpointer data)
{
    Channel *ch = data;
    gssize n = g_input_stream_read_finish(G_INPUT_STREAM(source), result, NULL);

    if (!ch->closed && n > 0) {
        g_byte_array_append(ch->buf, (const guint8 *)ch->chunk, n);
        parse(ch);
    } else {
        peer_closed(ch);
    }
    if (!ch->closed)
        channel_read(ch);
    channel_unref(ch);
}

static void channel_read(Channel *ch)
{
    ch->refs++;
    g_input_stream_read_async(ch->in, ch->chunk, sizeof ch->chunk, G_PRIORITY_DEFAULT, ch->cancel, on_read, ch);
}

static void write_next(Channel *ch);

static void on_written(GObject *source, GAsyncResult *result, gpointer data)
{
    Channel *ch = data;
    gboolean ok = g_output_stream_write_all_finish(G_OUTPUT_STREAM(source), result, NULL, NULL);

    g_clear_pointer(&ch->writing, g_bytes_unref);
    if (!ok)
        peer_closed(ch);
    else
        write_next(ch);
    channel_unref(ch);
}

static void write_next(Channel *ch)
{
    gsize size;
    const void *data;

    if (ch->writing || ch->closed || g_queue_is_empty(&ch->writes))
        return;
    ch->writing = g_queue_pop_head(&ch->writes);
    data = g_bytes_get_data(ch->writing, &size);
    ch->refs++;
    g_output_stream_write_all_async(ch->out, data, size, G_PRIORITY_DEFAULT, ch->cancel, on_written, ch);
}

static void channel_send(Channel *ch, const char *body)
{
    gsize len = strlen(body);
    GString *message;

    if (ch->closed)
        return;
    message = g_string_sized_new(len + 32);
    g_string_printf(message, "Content-Length: %" G_GSIZE_FORMAT "\r\n\r\n", len);
    g_string_append_len(message, body, len);
    g_queue_push_tail(&ch->writes, g_string_free_to_bytes(message));
    write_next(ch);
}

static Channel *channel_new(GInputStream *in, GOutputStream *out, GObject *keep, gboolean header, gpointer owner)
{
    Channel *ch = g_new0(Channel, 1);

    ch->refs = 1;
    ch->in = in;
    ch->out = out;
    ch->keep = g_object_ref(keep);
    ch->cancel = g_cancellable_new();
    ch->buf = g_byte_array_new();
    ch->header = header;
    ch->owner = owner;
    g_queue_init(&ch->writes);
    return ch;
}

/* Just enough JSON: finding fields by name and replacing their values ------------------------- */

typedef struct {
    const char *start, *end; /* start == NULL: not there */
} Span;

static const char *skip_space(const char *p, const char *end)
{
    while (p < end && g_ascii_isspace(*p))
        p++;
    return p;
}

static const char *skip_string(const char *p, const char *end) /* p at the opening quote */
{
    for (p++; p < end; p++) {
        if (*p == '\\')
            p++;
        else if (*p == '"')
            return p + 1;
    }
    return end;
}

/* Past one value; stops at the ',' or the closing bracket after a number or literal. */
static const char *skip_value(const char *p, const char *end)
{
    int depth = 0;

    while (p < end) {
        char c = *p;
        if (c == '"') {
            p = skip_string(p, end);
        } else if (c == '{' || c == '[') {
            depth++;
            p++;
        } else if (c == '}' || c == ']') {
            if (depth == 0)
                return p;
            depth--;
            p++;
        } else if (c == ',' && depth == 0) {
            return p;
        } else {
            p++;
        }
        if (depth == 0 && (c == '"' || c == '}' || c == ']'))
            return p;
    }
    return end;
}

static Span field(Span object, const char *key)
{
    Span none = { NULL, NULL };
    const char *p = skip_space(object.start, object.end), *end = object.end;
    gsize key_len = strlen(key);

    if (!p || p >= end || *p != '{')
        return none;
    for (p++;;) {
        p = skip_space(p, end);
        if (p >= end || *p != '"')
            return none;
        const char *name = p + 1;
        p = skip_string(p, end);
        gboolean match = (gsize)(p - 1 - name) == key_len && memcmp(name, key, key_len) == 0;
        p = skip_space(p, end);
        if (p >= end || *p != ':')
            return none;
        const char *value = skip_space(p + 1, end);
        p = skip_value(value, end);
        if (match) {
            const char *value_end = p;
            while (value_end > value && g_ascii_isspace(value_end[-1]))
                value_end--;
            return (Span){ value, value_end };
        }
        p = skip_space(p, end);
        if (p >= end || *p != ',')
            return none;
        p++;
    }
}

/* path: field names separated by dots, like "params.textDocument.uri". */
static Span lookup(const char *json, const char *path)
{
    Span span = { json, json + strlen(json) };
    char **keys = g_strsplit(path, ".", -1);

    for (int i = 0; keys[i] && span.start; i++)
        span = field(span, keys[i]);
    g_strfreev(keys);
    return span;
}

/* A string value without its quotes and with simple escapes undone (\uXXXX stays). */
static char *lookup_string(const char *json, const char *path)
{
    Span s = lookup(json, path);
    GString *out;

    if (!s.start || s.end - s.start < 2 || *s.start != '"')
        return NULL;
    out = g_string_new(NULL);
    for (const char *p = s.start + 1; p < s.end - 1; p++) {
        if (*p == '\\' && p + 1 < s.end - 1) {
            p++;
            switch (*p) {
            case 'n': g_string_append_c(out, '\n'); break;
            case 't': g_string_append_c(out, '\t'); break;
            case 'u': g_string_append(out, "\\u"); break;
            default: g_string_append_c(out, *p); break;
            }
        } else {
            g_string_append_c(out, *p);
        }
    }
    return g_string_free(out, FALSE);
}

static char *span_text(Span s)
{
    return g_strndup(s.start, s.end - s.start);
}

/* json with the value at path replaced by value; a copy of json when it has no such field. */
static char *replace(const char *json, const char *path, const char *value)
{
    Span s = lookup(json, path);

    if (!s.start)
        return g_strdup(json);
    return g_strdup_printf("%.*s%s%s", (int)(s.start - json), json, value, s.end);
}

static char *quote(const char *text)
{
    GString *out = g_string_new("\"");

    for (const char *p = text; *p; p++) {
        if (*p == '"' || *p == '\\')
            g_string_append_c(out, '\\');
        g_string_append_c(out, *p);
    }
    g_string_append_c(out, '"');
    return g_string_free(out, FALSE);
}

/* Servers and the editors using them ---------------------------------------------------------- */

typedef struct Server Server;

typedef struct {
    Server *server;          /* NULL until its first line came */
    Channel *ch;
    GHashTable *uris;        /* the documents it opened */
} Client;

typedef struct {
    Client *client;          /* NULL: it went, the answer goes nowhere */
    char *id;                /* its id, as JSON */
    gboolean initialize;
} Request;

struct Server {
    char *key;               /* the client's first line: folder and command */
    GSubprocess *process;
    Channel *ch;
    GPtrArray *clients;
    GHashTable *requests;    /* our id -> Request */
    guint next_id;
    char *init_result;       /* the answer to initialize, once it came */
    GPtrArray *init_waiting; /* Requests of editors that asked while it had not come yet */
    gboolean init_sent, initialized_sent;
    GHashTable *open;        /* uri -> how many editors opened it */
    guint idle;
};

static GHashTable *servers; /* key -> Server */
static GSocketService *service;
static char *socket_path;

static void request_free(Request *r)
{
    g_free(r->id);
    g_free(r);
}

static void reply(Client *c, const char *id, const char *result)
{
    char *body = g_strdup_printf("{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":%s}", id, result);

    channel_send(c->ch, body);
    g_free(body);
}

static void send_with_id(Channel *ch, const char *body, const char *id)
{
    char *renumbered = replace(body, "id", id);

    channel_send(ch, renumbered);
    g_free(renumbered);
}

static void server_free(Server *s)
{
    for (guint i = 0; i < s->clients->len; i++) {
        Client *c = g_ptr_array_index(s->clients, i);
        channel_close(c->ch);
        g_hash_table_unref(c->uris);
        g_free(c);
    }
    g_ptr_array_unref(s->clients);
    if (s->idle)
        g_source_remove(s->idle);
    channel_close(s->ch);  /* EOF on its stdin: it exits */
    g_object_unref(s->process);
    g_hash_table_unref(s->requests);
    g_ptr_array_unref(s->init_waiting);
    g_hash_table_unref(s->open);
    g_free(s->init_result);
    g_free(s->key);
    g_free(s);
}

static void server_closed(gpointer owner)
{
    Server *s = owner;

    g_hash_table_remove(servers, s->key); /* frees it, closing its editors' connections */
}

static gboolean server_idle(gpointer data)
{
    Server *s = data;

    s->idle = 0;
    g_hash_table_remove(servers, s->key);
    return G_SOURCE_REMOVE;
}

static void forward_request(Client *c, const char *body, Span id, gboolean initialize)
{
    Server *s = c->server;
    Request *r = g_new0(Request, 1);
    guint n = s->next_id++;
    char *ours = g_strdup_printf("%u", n);

    r->client = c;
    r->id = span_text(id);
    r->initialize = initialize;
    g_hash_table_insert(s->requests, GUINT_TO_POINTER(n), r);
    send_with_id(s->ch, body, ours);
    g_free(ours);
}

static void client_initialize(Client *c, const char *body, Span id)
{
    Server *s = c->server;

    if (s->init_result) {
        char *client_id = span_text(id);
        reply(c, client_id, s->init_result);
        g_free(client_id);
    } else if (s->init_sent) {
        Request *r = g_new0(Request, 1);
        r->client = c;
        r->id = span_text(id);
        g_ptr_array_add(s->init_waiting, r);
    } else {
        /* The server may watch the process that started it: that is t-code now, not this editor. */
        char *pid = g_strdup_printf("%d", getpid());
        char *ours = replace(body, "params.processId", pid);
        s->init_sent = TRUE;
        forward_request(c, ours, lookup(ours, "id"), TRUE);
        g_free(ours);
        g_free(pid);
    }
}

static void client_message(gpointer owner, const char *body)
{
    Client *c = owner;
    Server *s = c->server;
    char *method = lookup_string(body, "method");
    Span id = lookup(body, "id");

    if (!method) { /* an answer to a server request: those are answered here */
        return;
    }
    if (id.start) {
        if (strcmp(method, "initialize") == 0) {
            client_initialize(c, body, id);
        } else if (strcmp(method, "shutdown") == 0) {
            char *client_id = span_text(id);
            reply(c, client_id, "null");
            g_free(client_id);
        } else {
            forward_request(c, body, id, FALSE);
        }
    } else if (strcmp(method, "initialized") == 0) {
        if (!s->initialized_sent) {
            s->initialized_sent = TRUE;
            channel_send(s->ch, body);
        }
    } else if (strcmp(method, "exit") != 0 && strcmp(method, "$/cancelRequest") != 0) {
        char *uri = lookup_string(body, "params.textDocument.uri");
        gboolean forward = TRUE;
        if (uri && strcmp(method, "textDocument/didOpen") == 0 && !g_hash_table_contains(c->uris, uri)) {
            g_hash_table_add(c->uris, g_strdup(uri));
            int n = GPOINTER_TO_INT(g_hash_table_lookup(s->open, uri));
            g_hash_table_insert(s->open, g_strdup(uri), GINT_TO_POINTER(n + 1));
        } else if (uri && strcmp(method, "textDocument/didClose") == 0) {
            int n = GPOINTER_TO_INT(g_hash_table_lookup(s->open, uri));
            if (g_hash_table_remove(c->uris, uri))
                n--;
            if (n > 0)
                g_hash_table_insert(s->open, g_strdup(uri), GINT_TO_POINTER(n));
            else
                g_hash_table_remove(s->open, uri);
            forward = n <= 0;
        }
        if (forward)
            channel_send(s->ch, body);
        g_free(uri);
    }
    g_free(method);
}

static void client_free(Client *c)
{
    channel_close(c->ch);
    g_hash_table_unref(c->uris);
    g_free(c);
}

/* The editor went (or quit its plugin): forget its requests, close the documents only it had. */
static void client_closed(gpointer owner)
{
    Client *c = owner;
    Server *s = c->server;
    GHashTableIter iter;
    gpointer value, uri;

    if (!s) {
        client_free(c);
        return;
    }
    g_hash_table_iter_init(&iter, s->requests);
    while (g_hash_table_iter_next(&iter, NULL, &value))
        if (((Request *)value)->client == c)
            ((Request *)value)->client = NULL;
    for (guint i = s->init_waiting->len; i-- > 0;)
        if (((Request *)g_ptr_array_index(s->init_waiting, i))->client == c)
            g_ptr_array_remove_index(s->init_waiting, i);
    g_hash_table_iter_init(&iter, c->uris);
    while (g_hash_table_iter_next(&iter, &uri, NULL)) {
        int n = GPOINTER_TO_INT(g_hash_table_lookup(s->open, uri)) - 1;
        if (n > 0) {
            g_hash_table_insert(s->open, g_strdup(uri), GINT_TO_POINTER(n));
        } else {
            char *quoted = quote(uri);
            char *body = g_strdup_printf("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didClose\","
                                         "\"params\":{\"textDocument\":{\"uri\":%s}}}", quoted);
            g_hash_table_remove(s->open, uri);
            channel_send(s->ch, body);
            g_free(body);
            g_free(quoted);
        }
    }
    g_ptr_array_remove(s->clients, c);
    client_free(c);
    if (s->clients->len == 0 && !s->idle)
        s->idle = g_timeout_add_seconds(IDLE_SECONDS, server_idle, s);
}

static void answer_initialize(Server *s, Request *r, const char *body)
{
    Span result = lookup(body, "result");

    if (result.start)
        s->init_result = span_text(result);
    else
        s->init_sent = FALSE; /* failed: the next editor tries again */
    if (r->client)
        send_with_id(r->client->ch, body, r->id);
    for (guint i = 0; i < s->init_waiting->len; i++) {
        Request *w = g_ptr_array_index(s->init_waiting, i);
        if (s->init_result)
            reply(w->client, w->id, s->init_result);
        else
            send_with_id(w->client->ch, body, w->id);
    }
    g_ptr_array_set_size(s->init_waiting, 0);
}

static void server_message(gpointer owner, const char *body)
{
    Server *s = owner;
    char *method = lookup_string(body, "method");
    Span id = lookup(body, "id");

    if (method && id.start) { /* a request to the editor */
        char *server_id = span_text(id);
        char *answer = g_strdup_printf("{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":%s}", server_id,
                                       strcmp(method, "workspace/configuration") == 0 ? "[{\"enable\":true}]" : "null");
        channel_send(s->ch, answer);
        g_free(answer);
        g_free(server_id);
    } else if (method) {
        char *uri = strcmp(method, "textDocument/publishDiagnostics") == 0 ? lookup_string(body, "params.uri") : NULL;
        for (guint i = 0; i < s->clients->len; i++) {
            Client *c = g_ptr_array_index(s->clients, i);
            if (uri ? g_hash_table_contains(c->uris, uri) : strcmp(method, "window/showMessage") == 0)
                channel_send(c->ch, body);
        }
        g_free(uri);
    } else if (id.start) { /* an answer */
        guint n = (guint)g_ascii_strtoull(id.start, NULL, 10);
        Request *r = g_hash_table_lookup(s->requests, GUINT_TO_POINTER(n));
        if (r) {
            g_hash_table_steal(s->requests, GUINT_TO_POINTER(n));
            if (r->initialize)
                answer_initialize(s, r, body);
            else if (r->client)
                send_with_id(r->client->ch, body, r->id);
            request_free(r);
        }
    }
    g_free(method);
}

static Server *server_start(const char *key)
{
    char **fields = g_strsplit(key, "\t", -1);
    GSubprocessLauncher *launcher;
    GSubprocess *process;
    GError *error = NULL;
    Server *s;

    if (g_strv_length(fields) < 2 || !g_path_is_absolute(fields[0])) {
        g_strfreev(fields);
        return NULL;
    }
    launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                         G_SUBPROCESS_FLAGS_STDERR_SILENCE);
    g_subprocess_launcher_set_cwd(launcher, fields[0]);
    process = g_subprocess_launcher_spawnv(launcher, (const char *const *)fields + 1, &error);
    g_object_unref(launcher);
    if (!process) {
        g_warning("t-code: cannot start %s: %s", fields[1], error->message);
        g_clear_error(&error);
        g_strfreev(fields);
        return NULL;
    }
    g_strfreev(fields);

    s = g_new0(Server, 1);
    s->key = g_strdup(key);
    s->process = process;
    s->clients = g_ptr_array_new();
    s->requests = g_hash_table_new_full(NULL, NULL, NULL, (GDestroyNotify)request_free);
    s->init_waiting = g_ptr_array_new_with_free_func((GDestroyNotify)request_free);
    s->open = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    s->next_id = 1;
    s->ch = channel_new(g_subprocess_get_stdout_pipe(process), g_subprocess_get_stdin_pipe(process),
                        G_OBJECT(process), FALSE, s);
    s->ch->message = server_message;
    s->ch->on_closed = server_closed;
    channel_read(s->ch);
    g_hash_table_insert(servers, s->key, s);
    return s;
}

/* The client's first line: which server, from which folder. */
static void client_line(gpointer owner, const char *line)
{
    Client *c = owner;
    Server *s = g_hash_table_lookup(servers, line);

    if (!s)
        s = server_start(line);
    if (!s) { /* the connection closes: the plugin goes on without a server */
        peer_closed(c->ch);
        return;
    }
    if (s->idle) {
        g_source_remove(s->idle);
        s->idle = 0;
    }
    c->server = s;
    g_ptr_array_add(s->clients, c);
}

static gboolean on_incoming(GSocketService *listener G_GNUC_UNUSED, GSocketConnection *conn,
                            GObject *source G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED)
{
    Client *c = g_new0(Client, 1);
    GIOStream *stream = G_IO_STREAM(conn);

    c->uris = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    c->ch = channel_new(g_io_stream_get_input_stream(stream), g_io_stream_get_output_stream(stream),
                        G_OBJECT(conn), TRUE, c);
    c->ch->line = client_line;
    c->ch->message = client_message;
    c->ch->on_closed = client_closed;
    channel_read(c->ch);
    return TRUE;
}

/* The environment variables that make an editor's lsp plugin use the shared servers. */
void lsp_editor_env(App *t, GPtrArray *env)
{
    if (!socket_path || !t->lsp_program || !g_file_test(t->lsp_program, G_FILE_TEST_IS_EXECUTABLE))
        return;
    g_ptr_array_add(env, g_strconcat("TCODE_LSP=", socket_path, NULL));
    g_ptr_array_add(env, g_strconcat("TCODE_LSP_CONNECT=", t->lsp_program, NULL));
}

void lsp_start(App *t G_GNUC_UNUSED)
{
    GSocketAddress *address;
    GError *error = NULL;

    servers = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, (GDestroyNotify)server_free);
    socket_path = g_strdup_printf("%s/t-code-%d-lsp.sock", g_get_user_runtime_dir(), getpid());
    unlink(socket_path);
    address = g_unix_socket_address_new(socket_path);
    service = g_socket_service_new();
    if (!g_socket_listener_add_address(G_SOCKET_LISTENER(service), address, G_SOCKET_TYPE_STREAM,
                                       G_SOCKET_PROTOCOL_DEFAULT, NULL, NULL, &error)) {
        g_warning("t-code: cannot listen on %s: %s", socket_path, error->message);
        g_clear_error(&error);
        g_clear_pointer(&socket_path, g_free); /* editors start their own servers */
    }
    g_object_unref(address);
    g_signal_connect(service, "incoming", G_CALLBACK(on_incoming), NULL);
    g_socket_service_start(service);
}

void lsp_stop(void)
{
    if (service) {
        g_socket_service_stop(service);
        g_clear_object(&service);
    }
    if (socket_path) {
        unlink(socket_path);
        g_clear_pointer(&socket_path, g_free);
    }
    g_clear_pointer(&servers, g_hash_table_unref);
}
