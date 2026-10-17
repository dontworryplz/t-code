/* Tests for the shared language servers in src/ipc/lsp.c (included with its channels and JSON helpers,
 * to reach their static state): editors
 * connect to its socket like t-code-lsp does, and this program itself, started with --fake-server,
 * is the language server. It logs every message it gets to a file, so the tests can see what reached
 * the server. Besides the routing, they check that nothing piles up over many editors coming and
 * going: no requests, documents or clients left behind, and (with glibc, without ASan) no growth of
 * the heap. make test-asan runs this under AddressSanitizer, whose leak check covers the rest. */
#define IDLE_SECONDS 1
#include "ipc/lsp.c"
#include "ipc/lspchannel.c"
#include "ipc/lspjson.c"

#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/un.h>
#if defined(__GLIBC__) && !defined(__SANITIZE_ADDRESS__)
#include <malloc.h>
#define HEAP_CHECK 1
#endif

static char *tmp, *self;

/* The fake server ---------------------------------------------------------------------------- */

static void fake_send(const char *format, ...)
{
    va_list args;
    char *body;

    va_start(args, format);
    body = g_strdup_vprintf(format, args);
    va_end(args);
    printf("Content-Length: %zu\r\n\r\n%s", strlen(body), body);
    fflush(stdout);
    g_free(body);
}

/* Answers every request with its method name ("initialize": empty capabilities), publishes
 * diagnostics for every opened document, asks for its configuration once initialized, and logs
 * "<method or answer> <uri or ->" per message. */
static int fake_server(const char *log_path)
{
    FILE *log = fopen(log_path, "a");
    char header[256];
    long size = -1;

    if (!log)
        return 1;
    setvbuf(log, NULL, _IOLBF, 0);
    while (fgets(header, sizeof header, stdin)) {
        if (g_ascii_strncasecmp(header, "Content-Length:", 15) == 0) {
            size = strtol(header + 15, NULL, 10);
            continue;
        }
        if (strcmp(header, "\r\n") != 0 || size < 0)
            continue;
        char *body = g_malloc(size + 1);
        if (fread(body, 1, size, stdin) != (size_t)size) {
            g_free(body);
            break;
        }
        body[size] = '\0';
        size = -1;
        char *method = json_lookup_string(body, "method");
        char *uri = json_lookup_string(body, "params.textDocument.uri");
        Span id = json_lookup(body, "id");
        fprintf(log, "%s %s\n", method ? method : "answer", uri ? uri : "-");
        if (method && id.start) {
            char *server_id = json_text(id);
            if (strcmp(method, "initialize") == 0)
                fake_send("{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{\"capabilities\":{}}}", server_id);
            else
                fake_send("{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":\"%s\"}", server_id, method);
            g_free(server_id);
        } else if (method && strcmp(method, "initialized") == 0) {
            fake_send("{\"jsonrpc\":\"2.0\",\"id\":\"config\",\"method\":\"workspace/configuration\","
                      "\"params\":{\"items\":[{}]}}");
        } else if (method && uri && strcmp(method, "textDocument/didOpen") == 0) {
            char *quoted = json_quote(uri);
            fake_send("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\","
                      "\"params\":{\"uri\":%s,\"diagnostics\":[]}}", quoted);
            g_free(quoted);
        }
        g_free(uri);
        g_free(method);
        g_free(body);
    }
    fprintf(log, "eof -\n");
    fclose(log);
    return 0;
}

/* Editors ------------------------------------------------------------------------------------ */

typedef struct {
    int fd;
    GByteArray *in;
} Editor;

/* Runs the main loop (ipc/lsp.c's side) until done() or ms have passed. */
static gboolean spin(gboolean (*done)(gpointer), gpointer data, int ms)
{
    gint64 end = g_get_monotonic_time() + ms * 1000;

    while (!(done && done(data))) {
        if (g_get_monotonic_time() > end)
            return FALSE;
        if (!g_main_context_iteration(NULL, FALSE))
            g_usleep(1000);
    }
    return TRUE;
}

/* The key of the server for log: "<folder>\t<server command>". */
static char *server_key(const char *log)
{
    return g_strdup_printf("%s\t%s\t--fake-server\t%s", tmp, self, log);
}

static Server *server_for(const char *log)
{
    char *key = server_key(log);
    Server *s = g_hash_table_lookup(servers, key);

    g_free(key);
    return s;
}

static Editor *editor_connect(const char *log)
{
    Editor *e = g_new0(Editor, 1);
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    char *line = server_key(log);

    e->fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    g_strlcpy(address.sun_path, socket_path, sizeof address.sun_path);
    g_assert_cmpint(connect(e->fd, (struct sockaddr *)&address, sizeof address), ==, 0);
    g_assert_true(write(e->fd, line, strlen(line)) == (ssize_t)strlen(line) && write(e->fd, "\n", 1) == 1);
    fcntl(e->fd, F_SETFL, O_NONBLOCK);
    e->in = g_byte_array_new();
    g_free(line);
    return e;
}

static void editor_send(Editor *e, const char *format, ...)
{
    va_list args;
    char *body, *message;

    va_start(args, format);
    body = g_strdup_vprintf(format, args);
    va_end(args);
    message = g_strdup_printf("Content-Length: %zu\r\n\r\n%s", strlen(body), body);
    for (gsize done = 0, len = strlen(message); done < len;) {
        ssize_t n = write(e->fd, message + done, len - done);
        if (n > 0)
            done += n;
        else if (n < 0 && errno == EAGAIN)
            spin(NULL, NULL, 1);
        else
            g_error("write: %s", g_strerror(errno));
    }
    g_free(message);
    g_free(body);
}

/* The next whole message from the server's side, or NULL when none comes within ms. */
static gboolean editor_has_message(gpointer data)
{
    Editor *e = data;
    char chunk[4096];
    ssize_t n;

    while ((n = read(e->fd, chunk, sizeof chunk)) > 0)
        g_byte_array_append(e->in, (const guint8 *)chunk, n);
    if (e->in->len == 0)
        return FALSE;
    const char *data_start = (const char *)e->in->data, *end = g_strstr_len(data_start, e->in->len, "\r\n\r\n");
    if (!end)
        return FALSE;
    long size = content_length(data_start, end - data_start);
    return e->in->len >= (gsize)(end - data_start + 4 + size);
}

static char *editor_receive(Editor *e, int ms)
{
    if (!spin(editor_has_message, e, ms))
        return NULL;
    const char *data = (const char *)e->in->data, *end = g_strstr_len(data, e->in->len, "\r\n\r\n");
    gsize head = end - data + 4;
    long size = content_length(data, end - data);
    char *body = g_strndup(data + head, size);
    g_byte_array_remove_range(e->in, 0, head + size);
    return body;
}

/* The next message must answer id with result. */
static void expect_answer(Editor *e, const char *id, const char *result)
{
    char *body = editor_receive(e, 3000);

    g_assert_nonnull(body);
    Span got_id = json_lookup(body, "id"), got = json_lookup(body, "result");
    g_assert_nonnull(got_id.start);
    g_assert_nonnull(got.start);
    char *id_text = json_text(got_id), *result_text = json_text(got);
    g_assert_cmpstr(id_text, ==, id);
    g_assert_cmpstr(result_text, ==, result);
    g_free(result_text);
    g_free(id_text);
    g_free(body);
}

static void expect_diagnostics(Editor *e, const char *uri)
{
    char *body = editor_receive(e, 3000), *got;

    g_assert_nonnull(body);
    got = json_lookup_string(body, "params.uri");
    g_assert_cmpstr(got, ==, uri);
    g_free(got);
    g_free(body);
}

static void expect_nothing(Editor *e)
{
    char *body = editor_receive(e, 150);

    if (body)
        g_error("unexpected message: %s", body);
}

static void editor_open(Editor *e, const char *uri)
{
    editor_send(e, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{\"textDocument\":"
                   "{\"uri\":\"%s\",\"languageId\":\"c\",\"version\":1,\"text\":\"\"}}}", uri);
}

static void editor_initialize(Editor *e, const char *id)
{
    editor_send(e, "{\"jsonrpc\":\"2.0\",\"id\":%s,\"method\":\"initialize\",\"params\":{\"processId\":1}}", id);
    expect_answer(e, id, "{\"capabilities\":{}}");
    editor_send(e, "{\"jsonrpc\":\"2.0\",\"method\":\"initialized\",\"params\":{}}");
}

typedef struct {
    Server *server;
    guint clients;
} ClientCount;

static gboolean has_clients(gpointer data)
{
    ClientCount *c = data;

    return c->server->clients->len == c->clients;
}

/* Disconnects, and waits until lsp.c has let the editor go. */
static void editor_close(Editor *e, const char *log)
{
    ClientCount count = { server_for(log), 0 };

    g_assert_nonnull(count.server);
    count.clients = count.server->clients->len - 1;
    close(e->fd);
    g_byte_array_unref(e->in);
    g_free(e);
    g_assert_true(spin(has_clients, &count, 3000));
}

/* The server's log ---------------------------------------------------------------------------- */

static int log_count(const char *log, const char *line)
{
    char *text = NULL, **lines;
    int n = 0;

    if (!g_file_get_contents(log, &text, NULL, NULL))
        return 0;
    lines = g_strsplit(text, "\n", -1);
    for (int i = 0; lines[i]; i++)
        n += strcmp(lines[i], line) == 0;
    g_strfreev(lines);
    g_free(text);
    return n;
}

typedef struct {
    const char *log, *line;
    int count;
} LogWait;

static gboolean log_has(gpointer data)
{
    LogWait *w = data;

    return log_count(w->log, w->line) >= w->count;
}

static void wait_log(const char *log, const char *line, int count)
{
    LogWait w = { log, line, count };

    if (!spin(log_has, &w, 3000))
        g_error("%s: \"%s\" %d times, not %d", log, line, log_count(log, line), count);
}

static char *new_log(const char *name)
{
    return g_build_filename(tmp, name, NULL);
}

/* Tests -------------------------------------------------------------------------------------- */

/* Two editors share one server: it is initialized once, and each editor's requests (both use
 * id 1) come back to it with its own id. */
static void test_shared(void)
{
    char *log = new_log("shared.log");
    Editor *a = editor_connect(log), *b = editor_connect(log);

    editor_initialize(a, "0");
    editor_initialize(b, "0");
    wait_log(log, "answer -", 1); /* t-code answered the server's configuration request */
    g_assert_cmpint(log_count(log, "initialize -"), ==, 1);
    g_assert_cmpint(log_count(log, "initialized -"), ==, 1);
    g_assert_cmpuint(g_hash_table_size(servers), ==, 1);

    editor_send(a, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"textDocument/hover\",\"params\":{}}");
    editor_send(b, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"textDocument/definition\",\"params\":{}}");
    expect_answer(a, "1", "\"textDocument/hover\"");
    expect_answer(b, "1", "\"textDocument/definition\"");
    expect_nothing(a);
    expect_nothing(b);
    g_assert_cmpuint(g_hash_table_size(server_for(log)->requests), ==, 0);

    editor_close(a, log);
    editor_close(b, log);
    g_free(log);
}

/* Diagnostics go only to the editors that opened the file. */
static void test_diagnostics(void)
{
    char *log = new_log("diagnostics.log");
    Editor *a = editor_connect(log), *b = editor_connect(log);

    editor_initialize(a, "0");
    editor_initialize(b, "0");
    editor_open(a, "file:///a.c");
    expect_diagnostics(a, "file:///a.c");
    editor_open(b, "file:///b.c");
    expect_diagnostics(b, "file:///b.c");
    expect_nothing(a);
    expect_nothing(b);
    editor_close(a, log);
    editor_close(b, log);
    g_free(log);
}

/* A document is closed in the server when the last editor that opened it goes. */
static void test_close_on_disconnect(void)
{
    char *log = new_log("close.log");
    Editor *a = editor_connect(log), *b = editor_connect(log);

    editor_initialize(a, "0");
    editor_initialize(b, "0");
    editor_open(a, "file:///x.c");
    expect_diagnostics(a, "file:///x.c");
    editor_open(b, "file:///x.c");
    wait_log(log, "textDocument/didOpen file:///x.c", 2);
    expect_diagnostics(b, "file:///x.c");

    editor_close(a, log);
    spin(NULL, NULL, 100);
    g_assert_cmpint(log_count(log, "textDocument/didClose file:///x.c"), ==, 0);
    editor_close(b, log);
    wait_log(log, "textDocument/didClose file:///x.c", 1);
    g_assert_cmpuint(g_hash_table_size(server_for(log)->open), ==, 0);
    g_free(log);
}

/* An editor that goes while its request is out: the late answer is dropped, nothing kept. */
static void test_answer_after_close(void)
{
    char *log = new_log("late.log");
    Editor *a = editor_connect(log), *b = editor_connect(log);

    editor_initialize(a, "0");
    editor_initialize(b, "0");
    editor_send(a, "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"textDocument/hover\",\"params\":{}}");
    editor_close(a, log);
    wait_log(log, "textDocument/hover -", 1);
    spin(NULL, NULL, 100);
    expect_nothing(b);
    g_assert_cmpuint(g_hash_table_size(server_for(log)->requests), ==, 0);
    editor_close(b, log);
    g_free(log);
}

static gboolean server_gone(gpointer data)
{
    return server_for(data) == NULL;
}

/* A server without editors stops after IDLE_SECONDS; its process sees EOF and exits. */
static void test_idle_stop(void)
{
    char *log = new_log("idle.log");
    Editor *a = editor_connect(log);

    editor_initialize(a, "0");
    editor_close(a, log);
    g_assert_nonnull(server_for(log));
    g_assert_true(spin(server_gone, log, IDLE_SECONDS * 1000 + 2000));
    wait_log(log, "eof -", 1);
    g_free(log);
}

/* A malformed header closes only that editor's connection. */
static void test_bad_header(void)
{
    char *log = new_log("bad.log");
    Editor *a = editor_connect(log), *b = editor_connect(log);
    ClientCount one = { NULL, 1 };

    editor_initialize(a, "0");
    editor_initialize(b, "0");
    g_assert_true(write(a->fd, "Content-Type: x\r\n\r\n{}", 22) == 22);
    one.server = server_for(log);
    g_assert_true(spin(has_clients, &one, 3000));
    editor_send(b, "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"textDocument/hover\",\"params\":{}}");
    expect_answer(b, "2", "\"textDocument/hover\"");
    close(a->fd);
    g_byte_array_unref(a->in);
    g_free(a);
    editor_close(b, log);
    g_free(log);
}

/* One editor's whole life: connect, initialize, open a file, ask, go. */
static void editor_life(const char *log, int i)
{
    Editor *e = editor_connect(log);
    char *uri = g_strdup_printf("file:///churn/%d.c", i);

    editor_initialize(e, "0");
    editor_open(e, uri);
    expect_diagnostics(e, uri);
    editor_send(e, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"textDocument/hover\",\"params\":{}}");
    expect_answer(e, "1", "\"textDocument/hover\"");
    editor_close(e, log);
    g_free(uri);
}

/* Hundreds of editors come and go (like tabs opened and closed for hours): nothing is left behind
 * in the server's tables, and the heap does not grow with the number of editors. */
static void test_churn(void)
{
    char *log = new_log("churn.log");
    Server *s;

    for (int i = 0; i < 50; i++) /* warm up: GLib's own caches, the hash tables' sizes */
        editor_life(log, i);
#ifdef HEAP_CHECK
    malloc_trim(0);
    size_t before = mallinfo2().uordblks;
#endif
    for (int i = 50; i < 450; i++)
        editor_life(log, i);
    s = server_for(log);
    g_assert_nonnull(s);
    g_assert_cmpuint(s->clients->len, ==, 0);
    g_assert_cmpuint(g_hash_table_size(s->requests), ==, 0);
    g_assert_cmpuint(g_hash_table_size(s->open), ==, 0);
    g_assert_cmpuint(s->init_waiting->len, ==, 0);
    wait_log(log, "textDocument/didClose file:///churn/449.c", 1);
#ifdef HEAP_CHECK
    malloc_trim(0);
    size_t after = mallinfo2().uordblks;
    g_test_message("heap in use: %zu -> %zu bytes over 400 editors", before, after);
    /* A leak of even 100 bytes per editor would be 40 KB. */
    g_assert_cmpint((long)after - (long)before, <, 32 * 1024);
#endif
    g_free(log);
}

int main(int argc, char **argv)
{
    int status;

    if (argc == 3 && strcmp(argv[1], "--fake-server") == 0)
        return fake_server(argv[2]);
    g_test_init(&argc, &argv, NULL);
    self = g_file_read_link("/proc/self/exe", NULL);
    tmp = g_dir_make_tmp("t-code-lsp-XXXXXX", NULL);
    g_setenv("XDG_RUNTIME_DIR", tmp, TRUE);
    lsp_start(NULL);
    g_assert_nonnull(socket_path);

    g_test_add_func("/lsp/shared", test_shared);
    g_test_add_func("/lsp/diagnostics", test_diagnostics);
    g_test_add_func("/lsp/close-on-disconnect", test_close_on_disconnect);
    g_test_add_func("/lsp/answer-after-close", test_answer_after_close);
    g_test_add_func("/lsp/bad-header", test_bad_header);
    g_test_add_func("/lsp/idle-stop", test_idle_stop);
    g_test_add_func("/lsp/churn", test_churn);
    status = g_test_run();

    lsp_stop(); /* every server and connection freed: the leak check (make test-asan) sees the rest */
    spin(NULL, NULL, 200); /* the cancelled reads and writes finish */
    char *rm[] = { "rm", "-rf", tmp, NULL };
    g_spawn_sync(NULL, rm, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, NULL);
    g_free(tmp);
    g_free(self);
    return status;
}
