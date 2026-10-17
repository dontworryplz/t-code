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
 * The messages travel over lspchannel.c's channels; lspjson.c finds and replaces the fields.
 */
#include "ipc/lsp.h"

#include "ipc/lspchannel.h"
#include "ipc/lspjson.h"

#include <gio/gunixsocketaddress.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef IDLE_SECONDS /* tests set a shorter one */
#define IDLE_SECONDS 60
#endif

/* Servers and the editors using them --------------------------------------------------------- */

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

/* A request an editor sent, waiting for the server's answer (in Server.requests). */
static void request_free(Request *r)
{
    g_free(r->id);
    g_free(r);
}

/* Answers an editor's request ourselves, with result (JSON). */
static void reply(Client *c, const char *id, const char *result)
{
    char *body = g_strdup_printf("{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":%s}", id, result);

    channel_send(c->ch, body);
    g_free(body);
}

/* Sends body with its "id" replaced: ours to the server, the editor's own back to it. */
static void send_with_id(Channel *ch, const char *body, const char *id)
{
    char *renumbered = json_replace(body, "id", id);

    channel_send(ch, renumbered);
    g_free(renumbered);
}

/* Stops a server: its editors' connections close, and EOF on its stdin makes it exit. */
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

/* The server exited (or its pipe broke). */
static void server_closed(gpointer owner)
{
    Server *s = owner;

    g_hash_table_remove(servers, s->key); /* frees it, closing its editors' connections */
}

/* IDLE_SECONDS without editors: the server stops. */
static gboolean server_idle(gpointer data)
{
    Server *s = data;

    s->idle = 0;
    g_hash_table_remove(servers, s->key);
    return G_SOURCE_REMOVE;
}

/* Passes an editor's request on to its server under a new id, remembering whose it was. */
static void forward_request(Client *c, const char *body, Span id, gboolean initialize)
{
    Server *s = c->server;
    Request *r = g_new0(Request, 1);
    guint n = s->next_id++;
    char *ours = g_strdup_printf("%u", n);

    r->client = c;
    r->id = json_text(id);
    r->initialize = initialize;
    g_hash_table_insert(s->requests, GUINT_TO_POINTER(n), r);
    send_with_id(s->ch, body, ours);
    g_free(ours);
}

/* An editor's "initialize": the first goes to the server; later ones get its cached answer, or
 * wait for it when it has not come yet. */
static void client_initialize(Client *c, const char *body, Span id)
{
    Server *s = c->server;

    if (s->init_result) {
        char *client_id = json_text(id);
        reply(c, client_id, s->init_result);
        g_free(client_id);
    } else if (s->init_sent) {
        Request *r = g_new0(Request, 1);
        r->client = c;
        r->id = json_text(id);
        g_ptr_array_add(s->init_waiting, r);
    } else {
        /* The server may watch the process that started it: that is t-code now, not this editor. */
        char *pid = g_strdup_printf("%d", getpid());
        char *ours = json_replace(body, "params.processId", pid);
        s->init_sent = TRUE;
        forward_request(c, ours, json_lookup(ours, "id"), TRUE);
        g_free(ours);
        g_free(pid);
    }
}

/* A message from an editor: requests are renumbered and forwarded, notifications forwarded once
 * (initialized) or counted (didOpen / didClose: only the last editor's didClose goes on). */
static void client_message(gpointer owner, const char *body)
{
    Client *c = owner;
    Server *s = c->server;
    char *method = json_lookup_string(body, "method");
    Span id = json_lookup(body, "id");

    if (!method) { /* an answer to a server request: those are answered here */
        return;
    }
    if (id.start) {
        if (strcmp(method, "initialize") == 0) {
            client_initialize(c, body, id);
        } else if (strcmp(method, "shutdown") == 0) {
            char *client_id = json_text(id);
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
        char *uri = json_lookup_string(body, "params.textDocument.uri");
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

/* Frees an editor's connection. */
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
            char *quoted = json_quote(uri);
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

/* The server answered "initialize": the answer is kept for later editors and sent to every
 * editor that waits for it. */
static void answer_initialize(Server *s, Request *r, const char *body)
{
    Span result = json_lookup(body, "result");

    if (result.start)
        s->init_result = json_text(result);
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

/* A message from the server: its own requests are answered here, notifications go to the editors
 * they concern, and answers go back to the editor that asked. */
static void server_message(gpointer owner, const char *body)
{
    Server *s = owner;
    char *method = json_lookup_string(body, "method");
    Span id = json_lookup(body, "id");

    if (method && id.start) { /* a request to the editor */
        char *server_id = json_text(id);
        char *answer = g_strdup_printf("{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":%s}", server_id,
                                       strcmp(method, "workspace/configuration") == 0 ? "[{\"enable\":true}]" : "null");
        channel_send(s->ch, answer);
        g_free(answer);
        g_free(server_id);
    } else if (method) {
        char *uri = strcmp(method, "textDocument/publishDiagnostics") == 0 ? json_lookup_string(body, "params.uri") : NULL;
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

/* glibc gives every thread that allocates its own malloc arena and rarely hands their freed
 * memory back: clangd, whose worker threads build ASTs and preambles, measured 1.5 GB resident
 * with 25 open files while it accounted for 130 MB itself. Two arenas kept it at 700 MB, also
 * after rebuilds, at the same speed. Other C libraries ignore the variable. */
static void set_malloc_arenas(GSubprocessLauncher *launcher)
{
    const char *tunables = g_getenv("GLIBC_TUNABLES");
    char *value;

    if (tunables && strstr(tunables, "glibc.malloc.arena_max"))
        return;
    value = tunables && *tunables ? g_strconcat(tunables, ":glibc.malloc.arena_max=2", NULL)
                                  : g_strdup("glibc.malloc.arena_max=2");
    g_subprocess_launcher_setenv(launcher, "GLIBC_TUNABLES", value, TRUE);
    g_free(value);
}

/* Starts the server an editor asked for: key is "<folder>\t<command>\t<arg>...". NULL when it
 * cannot start. */
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
#ifndef __GLIBC__
    /* malloc_trim() is glibc's: clangd built on musl (Void) has no --malloc-trim and exits on it. */
    for (char **from = fields + 1, **to = fields + 1;; from++) {
        if (*from && strcmp(*from, "--malloc-trim") == 0) {
            g_free(*from);
            continue;
        }
        if (!(*to++ = *from))
            break;
    }
#endif
    launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                         G_SUBPROCESS_FLAGS_STDERR_SILENCE);
    g_subprocess_launcher_set_cwd(launcher, fields[0]);
    set_malloc_arenas(launcher);
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
        channel_peer_closed(c->ch);
        return;
    }
    if (s->idle) {
        g_source_remove(s->idle);
        s->idle = 0;
    }
    c->server = s;
    g_ptr_array_add(s->clients, c);
}

/* An editor (its t-code-lsp) connected; its first line says which server it wants. */
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
