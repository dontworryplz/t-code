/*
 * The language server client: one clangd per window, started in the project folder when the first
 * C or C++ file opens and stopped a minute after the last one closes (its memory comes back).
 * Open editors are kept in sync with it change by change (only what changed is sent); what it
 * says about them comes back as diagnostics (lsp/diagnostics.c). Other modules ask it things
 * (hover, completion, definition) with lsp_request().
 */
#pragma once

#include "app.h"

#include <json-glib/json-glib.h>

/* Documents ---------------------------------------------------------------------------------- */

/* An editor's file was loaded (or loaded again): the server gets it, when it is C or C++ and not a
 * read-only old version. */
void lsp_open(Tab *tab);

/* The editor was shown: marks it most recently used, and re-opens it on the server if it was
 * evicted from the open-document LRU pool. */
void lsp_touch(Tab *tab);

/* The editor was saved. */
void lsp_saved(Tab *tab);

/* The editor goes: the server forgets the file; requests still waiting for it get NULL. */
void lsp_close(Tab *tab);

/* Whether the server has the editor's file open (it can answer about it). */
gboolean lsp_has(Tab *tab);

/* Stops the window's server (t-code quits). */
void lsp_stop(App *t);

/* Starts the server again with the open files (its settings changed, or the user asked: it reads
 * the compile flags again); one that gave up after crashing or was not found is tried again. */
void lsp_restart(App *t);

/* Why clangd cannot find an open file's #include, for the status bar: the C library's headers are
 * not installed (problem "No C headers"), or no compilation database applies to the file ("No
 * compile flags", naming a misspelled one like "compile-flags.txt"); what to do about it, else NULL.
 * problem is set only with a hint. */
char *lsp_flags_hint(App *t, const char **problem);

/* Whether an editor's file goes to the server (C or C++, not a read-only old version). */
gboolean lsp_wants(Tab *tab);

/* Requests ----------------------------------------------------------------------------------- */

/* The answer to a request: its "result", or NULL (an error, no server, the editor went, or it was
 * cancelled). Called exactly once. */
typedef void (*LspReply)(JsonNode *result, gpointer data);

/* Sends a request about an editor's file; params is the JSON object of its parameters (see
 * lsp_position_params). Changes not sent yet go first. cancel (may be NULL) drops it: reply gets
 * NULL (right after the cancel signal, or with the answer if that comes first). */
void lsp_request(Tab *tab, const char *method, const char *params, GCancellable *cancel, LspReply reply, gpointer data);

/* The server's capability at path ("completionProvider.triggerCharacters"), or NULL before it has
 * answered initialize. */
JsonNode *lsp_capability(Tab *tab, const char *path);

/* Positions ---------------------------------------------------------------------------------- */

/* '"textDocument": {"uri": ...}, "position": {...}' for iter, to go inside a request's params. */
char *lsp_position_params(Tab *tab, const GtkTextIter *iter);

/* '"textDocument": {"uri": ...}' alone. */
char *lsp_document_params(Tab *tab);

/* Whether the server counts positions in UTF-16 units (else in UTF-8 bytes). */
gboolean lsp_utf16(App *t);

/* The place in the buffer of an LSP position ({"line": ., "character": .}), kept inside the text. */
void lsp_iter_at(Tab *tab, JsonNode *position, GtkTextIter *iter);

/* The same from the line and character. */
void lsp_iter_at_position(Tab *tab, gint64 line, gint64 character, GtkTextIter *iter);

/* JSON --------------------------------------------------------------------------------------- */

/* The node at a dotted path in node ("range.start.line", "items.0.label"), or NULL. */
JsonNode *json_at(JsonNode *node, const char *path);

/* The string / number at path, or NULL / fallback. */
const char *json_string_at(JsonNode *node, const char *path);
gint64 json_int_at(JsonNode *node, const char *path, gint64 fallback);

/* text as a JSON string, quotes included. */
char *json_quote(const char *text);

/* "file:///a%20b.c" -> "/a b.c" and back. */
char *lsp_uri_to_path(const char *uri);
char *lsp_path_to_uri(const char *path);
