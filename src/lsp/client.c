/*
 * The language server client (see client.h). One server (clangd) per window, in the project folder:
 *   - it starts when the first C or C++ editor opens, and is told about each editor (didOpen);
 *   - every insert and delete in an open editor becomes a change with its range, sent in batches
 *     (didChange) a moment later, or at once before a request about that file;
 *   - until it has answered initialize nothing about the files is sent: then they go whole, with
 *     positions in the encoding it chose (UTF-8 bytes, or UTF-16 units);
 *   - a minute after the last editor closes it stops (clangd holds hundreds of MB); a crash
 *     restarts it, up to MAX_CRASHES times.
 * It reads EOF on its stdin when t-code goes, and exits.
 */
#include "lsp/client.h"

#include "editor/editorview.h"
#include "lsp/channel.h"
#include "lsp/compiledb.h"
#include "lsp/diagnostics.h"
#include "lsp/semantic.h"
#include "window/statusbar.h"

#include <string.h>
#include <unistd.h>

#ifndef IDLE_SECONDS /* tests set a shorter one */
#define IDLE_SECONDS 60
#endif
#ifndef FLUSH_MS
#define FLUSH_MS 50 /* changes typed within this go in one didChange */
#endif
#ifndef MAX_OPEN_DOCS
#define MAX_OPEN_DOCS 5 /* at most this many files stay didOpen in clangd at once (LRU) */
#endif
#define MAX_CRASHES 3

#ifdef __GLIBC__
#define MALLOC_TRIM "--malloc-trim", /* gives freed memory back to the system */
#else
#define MALLOC_TRIM /* glibc's malloc_trim(): clangd built on musl has no such flag and exits on it */
#endif
#ifndef SERVER_ARGV /* tests run a fake server */
#define SERVER_ARGV "clangd", "--background-index", "--pch-storage=disk", "-j=2", "--limit-results=50", MALLOC_TRIM
#define TIDY_FLAG /* --clang-tidy or not, from the settings */
#endif
static const char *const server_argv[] = { SERVER_ARGV NULL };

/* An open editor, as the server knows it. */
typedef struct {
  Tab *tab;
  Lsp *lsp;
  int version;
  guint64 use;      /* LRU order: bumped when opened, shown, edited or queried */
  gboolean opened;  /* didOpen went to the running server */
  GString *changes; /* contentChanges not sent yet, comma separated */
  guint flush_id;
  gboolean include_missing; /* clangd's last diagnostics had an #include it cannot find */
} Doc;

/* A request waiting for its answer. */
typedef struct {
  Lsp *lsp;
  gint64 id;
  Tab *tab; /* NULL for initialize */
  LspReply reply;
  gpointer data;
  GCancellable *cancel;
  gulong cancelled_id;
  gboolean cancelled; /* its answer, if it comes first, is not given */
} Pending;

struct Lsp {
  App *app;
  GSubprocess *process;
  Channel *ch;
  char *root;     /* the folder it runs in */
  gboolean ready; /* initialize answered */
  gboolean utf16; /* positions count UTF-16 units, not UTF-8 bytes */
  JsonNode *capabilities;
  GHashTable *pending; /* id -> Pending */
  gint64 next_id;
  GHashTable *docs; /* Tab -> Doc */
  guint64 use;      /* next LRU stamp for docs */
  guint idle_id, restart_id;
  int crashes;
  gboolean missing; /* the server is not installed: not tried again */
};

/* JSON --------------------------------------------------------------------------------------- */

JsonNode *json_at(JsonNode *node, const char *path)
{
  char **parts = g_strsplit(path, ".", -1);

  for (int i = 0; node && parts[i]; i++) {
    if (JSON_NODE_HOLDS_OBJECT(node)) {
      JsonObject *object = json_node_get_object(node);
      node = json_object_has_member(object, parts[i]) ? json_object_get_member(object, parts[i]) : NULL;
    } else if (JSON_NODE_HOLDS_ARRAY(node) && g_ascii_isdigit(parts[i][0])) {
      JsonArray *array = json_node_get_array(node);
      guint index = (guint)g_ascii_strtoull(parts[i], NULL, 10);
      node = index < json_array_get_length(array) ? json_array_get_element(array, index) : NULL;
    } else {
      node = NULL;
    }
  }
  g_strfreev(parts);
  return node && !JSON_NODE_HOLDS_NULL(node) ? node : NULL;
}

const char *json_string_at(JsonNode *node, const char *path)
{
  node = json_at(node, path);
  return node && json_node_get_value_type(node) == G_TYPE_STRING ? json_node_get_string(node) : NULL;
}

gint64 json_int_at(JsonNode *node, const char *path, gint64 fallback)
{
  node = json_at(node, path);
  return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) != G_TYPE_STRING
             ? json_node_get_int(node)
             : fallback;
}

char *json_quote(const char *text)
{
  JsonNode *node = json_node_init_string(json_node_alloc(), text);
  char *quoted = json_to_string(node, FALSE);

  json_node_unref(node);
  return quoted;
}

char *lsp_uri_to_path(const char *uri)
{
  return g_filename_from_uri(uri, NULL, NULL);
}

char *lsp_path_to_uri(const char *path)
{
  return g_filename_to_uri(path, NULL, NULL);
}

/* Positions ---------------------------------------------------------------------------------- */

/* The LSP position of iter: its line, and how far into it in the server's encoding. */
static void position_of(Lsp *l, const GtkTextIter *iter, int *line, int *character)
{
  GtkTextIter walk = *iter;

  *line = gtk_text_iter_get_line(iter);
  if (!l->utf16) {
    *character = gtk_text_iter_get_line_index(iter);
    return;
  }
  *character = 0;
  for (gtk_text_iter_set_line_offset(&walk, 0); gtk_text_iter_compare(&walk, iter) < 0;
       gtk_text_iter_forward_char(&walk))
    *character += gtk_text_iter_get_char(&walk) >= 0x10000 ? 2 : 1;
}

gboolean lsp_utf16(App *t)
{
  return t->lsp && t->lsp->utf16;
}

void lsp_iter_at(Tab *tab, JsonNode *position, GtkTextIter *iter)
{
  lsp_iter_at_position(tab, json_int_at(position, "line", 0), json_int_at(position, "character", 0), iter);
}

void lsp_iter_at_position(Tab *tab, gint64 line, gint64 character, GtkTextIter *iter)
{
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(tab->buffer);
  gboolean utf16 = lsp_utf16(tab->app);

  if (line >= gtk_text_buffer_get_line_count(buffer)) {
    gtk_text_buffer_get_end_iter(buffer, iter);
    return;
  }
  gtk_text_buffer_get_iter_at_line(buffer, iter, (int)MAX(line, 0));
  /* Character by character, so a position inside a character (or past the line) cannot be. */
  for (gint64 at = 0; !gtk_text_iter_ends_line(iter);) {
    at += utf16 ? (gtk_text_iter_get_char(iter) >= 0x10000 ? 2 : 1)
                : g_unichar_to_utf8(gtk_text_iter_get_char(iter), NULL);
    if (at > character)
      break;
    gtk_text_iter_forward_char(iter);
  }
}

char *lsp_document_params(Tab *tab)
{
  char *uri = lsp_path_to_uri(tab->path), *quoted = json_quote(uri ? uri : "");
  char *params = g_strdup_printf("\"textDocument\":{\"uri\":%s}", quoted);

  g_free(quoted);
  g_free(uri);
  return params;
}

char *lsp_position_params(Tab *tab, const GtkTextIter *iter)
{
  char *document = lsp_document_params(tab), *params;
  int line = gtk_text_iter_get_line(iter), character = gtk_text_iter_get_line_index(iter);

  if (tab->app->lsp)
    position_of(tab->app->lsp, iter, &line, &character);
  params = g_strdup_printf("%s,\"position\":{\"line\":%d,\"character\":%d}", document, line, character);
  g_free(document);
  return params;
}

/* Sending ------------------------------------------------------------------------------------ */

static void notify(Lsp *l, const char *method, const char *params)
{
  char *body = g_strdup_printf("{\"jsonrpc\":\"2.0\",\"method\":\"%s\",\"params\":%s}", method, params);

  if (l->ch)
    channel_send(l->ch, body);
  g_free(body);
}

/* Hands a request its answer and forgets it. */
static void finish(Pending *p, JsonNode *result)
{
  Lsp *l = p->lsp;

  g_hash_table_steal(l->pending, &p->id);
  if (p->cancelled_id)
    g_cancellable_disconnect(p->cancel, p->cancelled_id);
  p->reply(result, p->data);
  g_clear_object(&p->cancel);
  g_free(p);
}

/* Every waiting request (of tab only, unless NULL) gets NULL. */
static void fail_pending(Lsp *l, Tab *tab)
{
  GList *all = g_hash_table_get_values(l->pending);

  for (GList *each = all; each; each = each->next) {
    Pending *p = each->data;
    if (!tab || p->tab == tab)
      finish(p, NULL);
  }
  g_list_free(all);
}

typedef struct {
  Lsp *lsp;
  gint64 id;
} Cancelled;

/* After a request was cancelled (not inside the cancel signal, where disconnecting would hang). */
static gboolean cancel_now(gpointer data)
{
  Cancelled *c = data;
  Pending *p = g_hash_table_lookup(c->lsp->pending, &c->id);

  if (p) {
    char *params = g_strdup_printf("{\"id\":%" G_GINT64_FORMAT "}", c->id);
    notify(c->lsp, "$/cancelRequest", params);
    g_free(params);
    finish(p, NULL);
  }
  g_free(c);
  return G_SOURCE_REMOVE;
}

static void on_cancelled(GCancellable *cancel G_GNUC_UNUSED, Pending *p)
{
  Cancelled *c = g_new(Cancelled, 1);

  p->cancelled = TRUE;
  *c = (Cancelled){ p->lsp, p->id };
  g_idle_add(cancel_now, c);
}

static void send_request(Lsp *l, Tab *tab, const char *method, const char *params, GCancellable *cancel, LspReply reply,
                         gpointer data)
{
  Pending *p = g_new0(Pending, 1);
  char *body;

  p->lsp = l;
  p->id = l->next_id++;
  p->tab = tab;
  p->reply = reply;
  p->data = data;
  g_hash_table_insert(l->pending, &p->id, p);
  if (cancel) {
    p->cancel = g_object_ref(cancel);
    p->cancelled_id = g_cancellable_connect(cancel, G_CALLBACK(on_cancelled), p, NULL);
  }
  body = g_strdup_printf("{\"jsonrpc\":\"2.0\",\"id\":%" G_GINT64_FORMAT ",\"method\":\"%s\",\"params\":%s}", p->id,
                         method, params);
  channel_send(l->ch, body);
  g_free(body);
}

/* Documents ---------------------------------------------------------------------------------- */

static Doc *doc_of(Tab *tab)
{
  return tab->app->lsp ? g_hash_table_lookup(tab->app->lsp->docs, tab) : NULL;
}

/* Sends the changes collected for a document. */
static void flush(Doc *doc)
{
  char *document, *params;

  if (doc->flush_id) {
    g_source_remove(doc->flush_id);
    doc->flush_id = 0;
  }
  if (doc->changes->len == 0)
    return;
  document = lsp_document_params(doc->tab);
  document[strlen(document) - 1] = '\0'; /* the version goes into textDocument */
  params = g_strdup_printf("{%s,\"version\":%d},\"contentChanges\":[%s]}", document, ++doc->version, doc->changes->str);
  notify(doc->lsp, "textDocument/didChange", params);
  g_string_truncate(doc->changes, 0);
  g_free(params);
  g_free(document);
}

static gboolean flush_later(gpointer data)
{
  Doc *doc = data;

  doc->flush_id = 0;
  flush(doc);
  return G_SOURCE_REMOVE;
}

/* One change: range (as an LSP range object) replaced by text. */
static void add_change(Doc *doc, const char *range, const char *text)
{
  char *quoted = json_quote(text);

  doc->use = ++doc->lsp->use;
  g_string_append_printf(doc->changes, "%s{\"range\":%s,\"text\":%s}", doc->changes->len ? "," : "", range, quoted);
  if (!doc->flush_id)
    doc->flush_id = g_timeout_add(FLUSH_MS, flush_later, doc);
  semantic_update(doc->tab);
  g_free(quoted);
}

/* Whether a change in the buffer goes to the server now (not before it opened the file, nor
 * while the file loads: then it gets all of it). */
static gboolean tracking(Doc *doc)
{
  return doc->opened && doc->lsp->ready && !doc->tab->view->loading;
}

static char *range_json(Lsp *l, const GtkTextIter *start, const GtkTextIter *end)
{
  int line1, character1, line2, character2;

  position_of(l, start, &line1, &character1);
  position_of(l, end, &line2, &character2);
  return g_strdup_printf("{\"start\":{\"line\":%d,\"character\":%d},\"end\":{\"line\":%d,\"character\":%d}}", line1,
                         character1, line2, character2);
}

/* Before the buffer takes the text (the signal's default handler inserts it). */
static void on_insert(GtkTextBuffer *buffer G_GNUC_UNUSED, const GtkTextIter *where, const char *text, int len,
                      Doc *doc)
{
  char *range, *inserted;

  if (!tracking(doc))
    return;
  range = range_json(doc->lsp, where, where);
  inserted = g_strndup(text, len);
  add_change(doc, range, inserted);
  g_free(inserted);
  g_free(range);
}

/* Before the buffer deletes the range. */
static void on_delete(GtkTextBuffer *buffer G_GNUC_UNUSED, const GtkTextIter *start, const GtkTextIter *end, Doc *doc)
{
  char *range;

  if (!tracking(doc))
    return;
  range = range_json(doc->lsp, start, end);
  add_change(doc, range, "");
  g_free(range);
}

/* The whole text, as a JSON string. */
static char *quoted_text(Tab *tab)
{
  GtkTextIter start, end;
  char *text, *quoted;

  gtk_text_buffer_get_bounds(GTK_TEXT_BUFFER(tab->buffer), &start, &end);
  text = gtk_text_buffer_get_text(GTK_TEXT_BUFFER(tab->buffer), &start, &end, TRUE);
  quoted = json_quote(text);
  g_free(text);
  return quoted;
}

/* Closes the document on the server (so clangd drops its AST from RAM), keeping Doc in l->docs. */
static void close_on_server(Doc *doc)
{
  if (!doc->opened)
    return;
  flush(doc);
  fail_pending(doc->lsp, doc->tab);
  char *document = lsp_document_params(doc->tab), *params = g_strdup_printf("{%s}", document);
  notify(doc->lsp, "textDocument/didClose", params);
  g_free(params);
  g_free(document);
  doc->opened = FALSE;
}

/* Keeps at most MAX_OPEN_DOCS files open in clangd: the least recently used hidden ones get
 * didClose, and didOpen again when their tab is shown. */
static void evict_docs(Lsp *l)
{
  for (;;) {
    GHashTableIter iter;
    gpointer value;
    Doc *oldest = NULL, *oldest_hidden = NULL;
    int opened = 0;

    g_hash_table_iter_init(&iter, l->docs);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
      Doc *doc = value;
      if (!doc->opened)
        continue;
      opened++;
      if (!oldest || doc->use < oldest->use)
        oldest = doc;
      if ((!doc->tab->source || !gtk_widget_get_mapped(GTK_WIDGET(doc->tab->source))) &&
          (!oldest_hidden || doc->use < oldest_hidden->use))
        oldest_hidden = doc;
    }
    if (opened <= MAX_OPEN_DOCS)
      break;
    close_on_server(oldest_hidden ? oldest_hidden : oldest);
  }
}

/* Tells the server about the file: all of it, as it is now. */
static void send_document(Doc *doc)
{
  char *document = lsp_document_params(doc->tab), *text = quoted_text(doc->tab), *params;

  doc->use = ++doc->lsp->use;
  g_string_truncate(doc->changes, 0);
  document[strlen(document) - 1] = '\0';
  if (doc->opened) {
    params = g_strdup_printf("{%s,\"version\":%d},\"contentChanges\":[{\"text\":%s}]}", document, ++doc->version, text);
    notify(doc->lsp, "textDocument/didChange", params);
  } else {
    const char *id = gtk_source_language_get_id(gtk_source_buffer_get_language(doc->tab->buffer));
    params = g_strdup_printf("{%s,\"languageId\":\"%s\",\"version\":%d,\"text\":%s}}", document,
                             g_str_has_prefix(id, "cpp") ? "cpp" : "c", ++doc->version, text);
    notify(doc->lsp, "textDocument/didOpen", params);
    doc->opened = TRUE;
    evict_docs(doc->lsp);
  }
  semantic_update(doc->tab);
  g_free(params);
  g_free(text);
  g_free(document);
}

static void doc_free(Doc *doc)
{
  if (doc->flush_id)
    g_source_remove(doc->flush_id);
  g_signal_handlers_disconnect_by_data(doc->tab->buffer, doc);
  g_string_free(doc->changes, TRUE);
  g_free(doc);
}

/* The server ------------------------------------------------------------------------------- */

static void server_start(Lsp *l);

/* The window's server state, made on first use. */
static Lsp *lsp_of(App *t)
{
  if (!t->lsp) {
    t->lsp = g_new0(Lsp, 1);
    t->lsp->app = t;
    t->lsp->pending = g_hash_table_new(g_int64_hash, g_int64_equal);
    t->lsp->docs = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, (GDestroyNotify)doc_free);
  }
  return t->lsp;
}

/* Stops the server: stdin closes, and it exits. Requests get NULL; the files are sent again to the
 * next one. */
static void server_stop(Lsp *l)
{
  GHashTableIter iter;
  gpointer doc;

  if (l->ch) {
    channel_close(l->ch);
    l->ch = NULL;
  }
  g_clear_object(&l->process);
  l->ready = FALSE;
  g_clear_pointer(&l->capabilities, json_node_unref);
  g_clear_pointer(&l->root, g_free);
  fail_pending(l, NULL);
  g_hash_table_iter_init(&iter, l->docs);
  while (g_hash_table_iter_next(&iter, NULL, &doc)) {
    ((Doc *)doc)->opened = FALSE;
    g_string_truncate(((Doc *)doc)->changes, 0);
  }
}

static gboolean restart(gpointer data)
{
  Lsp *l = data;

  l->restart_id = 0;
  if (!l->ch && g_hash_table_size(l->docs) > 0)
    server_start(l);
  return G_SOURCE_REMOVE;
}

/* The server went by itself (it crashed): started again for the open files, a few times. */
static void on_server_closed(gpointer owner)
{
  Lsp *l = owner;

  server_stop(l);
  if (++l->crashes <= MAX_CRASHES && !l->restart_id)
    l->restart_id = g_timeout_add_seconds(1, restart, l);
}

/* initialize answered: the encoding it chose, then the most recently used open files. */
static void on_initialized(JsonNode *result, gpointer data)
{
  Lsp *l = data;
  const char *encoding;
  GHashTableIter iter;
  gpointer doc;

  if (!result || !l->ch)
    return;
  if (json_at(result, "capabilities"))
    l->capabilities = json_node_copy(json_at(result, "capabilities"));
  encoding = json_string_at(result, "capabilities.positionEncoding");
  if (!encoding)
    encoding = json_string_at(result, "offsetEncoding"); /* clangd's own name for it */
  l->utf16 = g_strcmp0(encoding, "utf-8") != 0;
  l->ready = TRUE;
  notify(l, "initialized", "{}");
  g_hash_table_iter_init(&iter, l->docs);
  while (g_hash_table_iter_next(&iter, NULL, &doc)) {
    GHashTableIter other_iter;
    gpointer other;
    int newer = 0;
    g_hash_table_iter_init(&other_iter, l->docs);
    while (g_hash_table_iter_next(&other_iter, NULL, &other))
      if (((Doc *)other)->use > ((Doc *)doc)->use)
        newer++;
    if (newer < MAX_OPEN_DOCS)
      send_document(doc);
  }
}

/* The answer the server gets to one of its own requests. */
static void answer_server(Lsp *l, JsonNode *message)
{
  JsonNode *id = json_at(message, "id"), *items = json_at(message, "params.items");
  const char *method = json_string_at(message, "method");
  char *id_text = json_to_string(id, FALSE);
  GString *result = g_string_new("null");

  if (g_strcmp0(method, "workspace/configuration") == 0 && items && JSON_NODE_HOLDS_ARRAY(items)) {
    g_string_assign(result, "[");
    for (guint i = 0; i < json_array_get_length(json_node_get_array(items)); i++)
      g_string_append(result, i ? ",null" : "null");
    g_string_append(result, "]");
  }
  char *body = g_strdup_printf("{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":%s}", id_text, result->str);
  channel_send(l->ch, body);
  g_free(body);
  g_string_free(result, TRUE);
  g_free(id_text);
}

/* The editor whose file uri is, or NULL. */
static Tab *tab_of_uri(Lsp *l, const char *uri)
{
  char *path = uri ? lsp_uri_to_path(uri) : NULL;
  GHashTableIter iter;
  gpointer tab;
  Tab *found = NULL;

  g_hash_table_iter_init(&iter, l->docs);
  while (path && !found && g_hash_table_iter_next(&iter, &tab, NULL))
    if (g_strcmp0(((Tab *)tab)->path, path) == 0)
      found = tab;
  g_free(path);
  return found;
}

/* Whether diagnostics has an #include not found (clangd's code "pp_file_not_found"). */
static gboolean include_not_found(JsonNode *diagnostics)
{
  JsonArray *array = diagnostics && JSON_NODE_HOLDS_ARRAY(diagnostics) ? json_node_get_array(diagnostics) : NULL;

  for (guint i = 0; array && i < json_array_get_length(array); i++)
    if (g_strcmp0(json_string_at(json_array_get_element(array, i), "code"), "pp_file_not_found") == 0)
      return TRUE;
  return FALSE;
}

/* A message from the server: an answer, one of its own requests, or a notification. */
static void on_message(gpointer owner, const char *body)
{
  Lsp *l = owner;
  JsonParser *parser = json_parser_new_immutable();
  JsonNode *message, *id;
  const char *method;

  if (!json_parser_load_from_data(parser, body, -1, NULL) || !(message = json_parser_get_root(parser))) {
    g_object_unref(parser);
    return;
  }
  method = json_string_at(message, "method");
  id = json_at(message, "id");
  if (method && id) {
    answer_server(l, message);
  } else if (g_strcmp0(method, "textDocument/publishDiagnostics") == 0) {
    Tab *tab = tab_of_uri(l, json_string_at(message, "params.uri"));
    if (tab) {
      doc_of(tab)->include_missing = include_not_found(json_at(message, "params.diagnostics"));
      diagnostics_show(tab, json_at(message, "params.diagnostics")); /* updates the status bar */
    }
  } else if (!method && id && JSON_NODE_HOLDS_VALUE(id)) {
    gint64 number = json_node_get_int(id);
    Pending *p = g_hash_table_lookup(l->pending, &number);
    if (p)
      finish(p, p->cancelled ? NULL : json_at(message, "result"));
  }
  g_object_unref(parser);
}

/* "GLIBC_TUNABLES=glibc.malloc.arena_max=2" for the server: clangd's threads each grow a malloc
 * arena, and with 25 files it held 1.5 GB for 130 MB of its own data; two arenas kept it at about
 * 700 MB, as fast. */
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

/* Starts the server in the project folder and sends initialize; the files follow its answer. */
static void server_start(Lsp *l)
{
  GSubprocessLauncher *launcher = g_subprocess_launcher_new(
      G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE);
  GPtrArray *argv = g_ptr_array_new();
  GError *error = NULL;
  char *uri, *quoted, *params;

  for (int i = 0; server_argv[i]; i++)
    g_ptr_array_add(argv, (char *)server_argv[i]);
#ifdef TIDY_FLAG
  g_ptr_array_add(argv, l->app->settings.clang_tidy ? "--clang-tidy" : "--clang-tidy=false");
#endif
  g_ptr_array_add(argv, NULL);
  g_subprocess_launcher_set_cwd(launcher, l->app->root);
  set_malloc_arenas(launcher);
  l->process = g_subprocess_launcher_spawnv(launcher, (const char *const *)argv->pdata, &error);
  g_object_unref(launcher);
  g_ptr_array_free(argv, TRUE);
  if (!l->process) {
    g_warning("t-code: cannot start %s: %s", server_argv[0], error->message);
    l->missing = TRUE;
    g_clear_error(&error);
    return;
  }
  l->root = g_strdup(l->app->root);
  l->ch = channel_new(g_subprocess_get_stdout_pipe(l->process), g_subprocess_get_stdin_pipe(l->process),
                      G_OBJECT(l->process), l);
  l->ch->message = on_message;
  l->ch->on_closed = on_server_closed;
  channel_read(l->ch);

  uri = lsp_path_to_uri(l->root);
  quoted = json_quote(uri ? uri : "");
  params = g_strdup_printf(
      "{\"processId\":%d,\"rootUri\":%s,\"workspaceFolders\":[{\"uri\":%s,\"name\":\"root\"}],"
      "\"capabilities\":{"
      "\"general\":{\"positionEncodings\":[\"utf-8\",\"utf-16\"]},"
      "\"offsetEncoding\":[\"utf-8\",\"utf-16\"],"
      "\"textDocument\":{"
      "\"synchronization\":{\"didSave\":true},"
      "\"publishDiagnostics\":{\"relatedInformation\":false},"
      "\"completion\":{\"contextSupport\":true,\"completionItem\":{\"snippetSupport\":true,"
      "\"documentationFormat\":[\"plaintext\"],\"labelDetailsSupport\":true}},"
      "\"hover\":{\"contentFormat\":[\"plaintext\"]},"
      "\"signatureHelp\":{\"signatureInformation\":{\"documentationFormat\":[\"plaintext\"],"
      "\"parameterInformation\":{\"labelOffsetSupport\":true},\"activeParameterSupport\":true}},"
      "\"definition\":{},\"declaration\":{},\"references\":{},\"formatting\":{},\"rename\":{},"
      "\"semanticTokens\":{\"requests\":{\"full\":true},\"formats\":[\"relative\"],"
      "\"tokenTypes\":[\"namespace\",\"type\",\"class\",\"enum\",\"interface\",\"struct\",\"typeParameter\","
      "\"parameter\",\"variable\",\"property\",\"enumMember\",\"function\",\"method\",\"macro\",\"concept\"],"
      "\"tokenModifiers\":[\"declaration\",\"definition\",\"readonly\",\"static\",\"deprecated\"]}}}}",
      getpid(), quoted, quoted);
  send_request(l, NULL, "initialize", params, NULL, on_initialized, l);
  g_free(params);
  g_free(quoted);
  g_free(uri);
}

/* A minute without editors: the server stops. */
static gboolean idle_stop(gpointer data)
{
  Lsp *l = data;

  l->idle_id = 0;
  if (g_hash_table_size(l->docs) == 0)
    server_stop(l);
  return G_SOURCE_REMOVE;
}

/* Editors ------------------------------------------------------------------------------------ */

/* C and C++ go to clangd; old versions in a comparison do not (away from the project's build
 * setup it would only report noise). */
gboolean lsp_wants(Tab *tab)
{
  GtkSourceLanguage *language = tab->kind == TAB_EDITOR ? gtk_source_buffer_get_language(tab->buffer) : NULL;
  const char *id = language ? gtk_source_language_get_id(language) : "";

  return !tab->view->readonly && !tab->compare_with &&
         (strcmp(id, "c") == 0 || strcmp(id, "chdr") == 0 || strcmp(id, "cpp") == 0 || strcmp(id, "cpphdr") == 0);
}

void lsp_open(Tab *tab)
{
  Lsp *l;
  Doc *doc;

  if (!lsp_wants(tab))
    return;
  l = lsp_of(tab->app);
  if (l->missing)
    return;
  doc = g_hash_table_lookup(l->docs, tab);
  if (!doc) {
    doc = g_new0(Doc, 1);
    doc->tab = tab;
    doc->lsp = l;
    doc->changes = g_string_new(NULL);
    g_hash_table_insert(l->docs, tab, doc);
    g_signal_connect(tab->buffer, "insert-text", G_CALLBACK(on_insert), doc);
    g_signal_connect(tab->buffer, "delete-range", G_CALLBACK(on_delete), doc);
  }
  doc->use = ++l->use;
  if (l->idle_id) {
    g_source_remove(l->idle_id);
    l->idle_id = 0;
  }
  if (l->ch && g_strcmp0(l->root, tab->app->root) != 0) /* another project now */
    server_stop(l);
  if (!l->ch)
    server_start(l); /* the file goes once it has answered initialize */
  else if (l->ready)
    send_document(doc);
}

void lsp_touch(Tab *tab)
{
  Doc *doc = doc_of(tab);

  if (!doc)
    return;
  doc->use = ++doc->lsp->use;
  if (!doc->opened)
    lsp_open(tab);
}

void lsp_saved(Tab *tab)
{
  Doc *doc = doc_of(tab);
  char *document;

  if (!doc || !doc->opened)
    return;
  flush(doc);
  document = lsp_document_params(tab);
  char *params = g_strdup_printf("{%s}", document);
  notify(doc->lsp, "textDocument/didSave", params);
  g_free(params);
  g_free(document);
}

void lsp_close(Tab *tab)
{
  Doc *doc = doc_of(tab);
  Lsp *l;

  if (!doc)
    return;
  l = doc->lsp;
  fail_pending(l, tab);
  if (doc->opened) {
    char *document = lsp_document_params(tab), *params = g_strdup_printf("{%s}", document);
    notify(l, "textDocument/didClose", params);
    g_free(params);
    g_free(document);
  }
  g_hash_table_remove(l->docs, tab);
  if (g_hash_table_size(l->docs) == 0 && l->ch && !l->idle_id)
    l->idle_id = g_timeout_add_seconds(IDLE_SECONDS, idle_stop, l);
}

gboolean lsp_has(Tab *tab)
{
  Doc *doc = doc_of(tab);

  return doc && doc->opened;
}

void lsp_stop(App *t)
{
  Lsp *l = t->lsp;

  if (!l)
    return;
  server_stop(l);
  if (l->idle_id)
    g_source_remove(l->idle_id);
  if (l->restart_id)
    g_source_remove(l->restart_id);
  g_hash_table_destroy(l->docs);
  g_hash_table_destroy(l->pending);
  g_free(l);
  t->lsp = NULL;
}

void lsp_restart(App *t)
{
  Lsp *l = t->lsp;
  GHashTableIter iter;
  gpointer doc;

  if (!l)
    return; /* no C or C++ file was opened yet: the next one starts it */
  if (l->idle_id)
    g_source_remove(l->idle_id);
  if (l->restart_id)
    g_source_remove(l->restart_id);
  l->idle_id = l->restart_id = 0;
  server_stop(l);
  l->crashes = 0;
  l->missing = FALSE; /* it may have been installed meanwhile */
  g_hash_table_iter_init(&iter, l->docs);
  while (g_hash_table_iter_next(&iter, NULL, &doc))
    ((Doc *)doc)->include_missing = FALSE;
  if (g_hash_table_size(l->docs) > 0)
    server_start(l); /* else the next file starts it */
  update_status(t);
}

char *lsp_flags_hint(App *t)
{
  Lsp *l = t->lsp;
  GHashTableIter iter;
  gpointer tab, doc;

  if (!l)
    return NULL;
  g_hash_table_iter_init(&iter, l->docs);
  while (g_hash_table_iter_next(&iter, &tab, &doc)) {
    const char *path = ((Tab *)tab)->path;
    char *database = ((Doc *)doc)->include_missing && path ? compile_db_find(path) : NULL;
    char *misnamed, *name, *hint;
    if (!((Doc *)doc)->include_missing || !path || database) {
      g_free(database);
      continue;
    }
    misnamed = compile_db_misnamed(path, t->root);
    name = g_path_get_basename(path);
    if (misnamed) {
      char *found = g_path_get_basename(misnamed);
      hint = g_strdup_printf("clangd cannot find the headers %s includes, as it has no compile flags for it.\n\n"
                             "clangd does not read \"%s\": rename it to %s, then run "
                             "\"clangd: Restart Language Server\".",
                             name, found,
                             g_str_has_suffix(found, ".json") ? "compile_commands.json" : "compile_flags.txt");
      g_free(found);
    } else {
      hint = g_strdup_printf("clangd cannot find the headers %s includes, as it has no compile flags for it.\n\n"
                             "clangd reads them from compile_commands.json, build/compile_commands.json or "
                             "compile_flags.txt in the file's folder or above it:\n"
                             "  • make: bear -- make (writes compile_commands.json)\n"
                             "  • CMake: -DCMAKE_EXPORT_COMPILE_COMMANDS=ON\n"
                             "  • or compile_flags.txt with one flag per line (-I/usr/include/glib-2.0)\n\n"
                             "Then run \"clangd: Restart Language Server\".",
                             name);
    }
    g_free(name);
    g_free(misnamed);
    return hint;
  }
  return NULL;
}

void lsp_request(Tab *tab, const char *method, const char *params, GCancellable *cancel, LspReply reply, gpointer data)
{
  Doc *doc = doc_of(tab);

  if (!doc || !doc->opened || !doc->lsp->ready || (cancel && g_cancellable_is_cancelled(cancel))) {
    reply(NULL, data);
    return;
  }
  doc->use = ++doc->lsp->use;
  flush(doc);
  send_request(doc->lsp, tab, method, params, cancel, reply, data);
}

JsonNode *lsp_capability(Tab *tab, const char *path)
{
  Lsp *l = tab->app->lsp;

  return l && l->capabilities ? json_at(l->capabilities, path) : NULL;
}
