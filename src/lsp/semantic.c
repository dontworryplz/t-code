/*
 * Semantic token_colors (see semantic.h): textDocument/semanticTokens/full, a while after the last change.
 * Each token type has a tag; the tags sit above GtkSourceView's own, so they win. An answer for
 * text that has changed since is dropped (the next one is due anyway).
 */
#include "lsp/semantic.h"

#include "editor/editorview.h"
#include "lsp/client.h"

#include <string.h>

#define UPDATE_MS 300 /* after the last change */

typedef struct TokensRequest TokensRequest;

struct Semantic {
  Tab *tab;
  guint update_id;
  TokensRequest *request;
  GCancellable *cancel;
};

/* One request; dropped when semantic is NULL (the editor went) or a newer one went. */
struct TokensRequest {
  Semantic *semantic;
  guint changes; /* the buffer's change count when it was asked */
};

/* VS Code Dark Modern's token_colors for clangd's token types; a readonly variable is a constant. */
typedef struct {
  const char *type, *color;
} TokenColor;

/* clang-format off */
static const TokenColor token_colors[] = {
  { "function", "#DCDCAA" }, { "method", "#DCDCAA" },
  { "type", "#4EC9B0" }, { "class", "#4EC9B0" }, { "struct", "#4EC9B0" }, { "enum", "#4EC9B0" },
  { "interface", "#4EC9B0" }, { "typeParameter", "#4EC9B0" }, { "concept", "#4EC9B0" }, { "namespace", "#4EC9B0" },
  { "macro", "#569CD6" },
  { "enumMember", "#4FC1FF" },
  { "variable", "#9CDCFE" }, { "parameter", "#9CDCFE" }, { "property", "#9CDCFE" },
};
/* clang-format on */

#define CONSTANT_COLOR "#4FC1FF"
#define TAG_PREFIX "semantic-"

/* The tag for a color, made the first time. */
static GtkTextTag *token_tag(Tab *tab, const char *color)
{
  GtkTextTagTable *table = gtk_text_buffer_get_tag_table(GTK_TEXT_BUFFER(tab->buffer));
  char *name = g_strconcat(TAG_PREFIX, color, NULL);
  GtkTextTag *tag = gtk_text_tag_table_lookup(table, name);

  if (!tag)
    tag = gtk_text_buffer_create_tag(GTK_TEXT_BUFFER(tab->buffer), name, "foreground", color, NULL);
  g_free(name);
  return tag;
}

/* GtkTextTagTableForeach: collects the semantic tags. */
static void collect(GtkTextTag *tag, gpointer data)
{
  char *name = NULL;

  g_object_get(tag, "name", &name, NULL);
  if (name && g_str_has_prefix(name, TAG_PREFIX))
    g_ptr_array_add(data, tag);
  g_free(name);
}

/* The semantic tags go above all others: GtkSourceView makes its own as it token_colors more of the
 * text, and a newer tag would win. */
static void raise_tags(GtkTextTagTable *table)
{
  GPtrArray *tags = g_ptr_array_new();

  gtk_text_tag_table_foreach(table, collect, tags);
  for (guint i = 0; i < tags->len; i++)
    gtk_text_tag_set_priority(g_ptr_array_index(tags, i), gtk_text_tag_table_get_size(table) - 1);
  g_ptr_array_free(tags, TRUE);
}

static void on_tag_added(GtkTextTagTable *table, GtkTextTag *tag, gpointer data G_GNUC_UNUSED)
{
  char *name = NULL;

  g_object_get(tag, "name", &name, NULL);
  if (!name || !g_str_has_prefix(name, TAG_PREFIX))
    raise_tags(table);
  g_free(name);
}

static void remove_colors(Tab *tab)
{
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(tab->buffer);
  GPtrArray *tags = g_ptr_array_new();
  GtkTextIter start, end;

  gtk_text_tag_table_foreach(gtk_text_buffer_get_tag_table(buffer), collect, tags);
  gtk_text_buffer_get_bounds(buffer, &start, &end);
  for (guint i = 0; i < tags->len; i++)
    gtk_text_buffer_remove_tag(buffer, g_ptr_array_index(tags, i), &start, &end);
  g_ptr_array_free(tags, TRUE);
}

/* The color for a token type name, or NULL. */
static const char *color_of(const char *type)
{
  for (size_t i = 0; type && i < G_N_ELEMENTS(token_colors); i++)
    if (strcmp(token_colors[i].type, type) == 0)
      return token_colors[i].color;
  return NULL;
}

/* The tokens: five numbers each (line and start relative to the token before, length, type,
 * modifiers). Each type's tag is looked up once, from the server's legend. */
static void apply(Tab *tab, JsonNode *result)
{
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(tab->buffer);
  JsonNode *data = json_at(result, "data");
  JsonNode *types = lsp_capability(tab, "semanticTokensProvider.legend.tokenTypes");
  JsonNode *modifiers = lsp_capability(tab, "semanticTokensProvider.legend.tokenModifiers");
  JsonArray *numbers = data && JSON_NODE_HOLDS_ARRAY(data) ? json_node_get_array(data) : NULL;
  guint n = numbers ? json_array_get_length(numbers) : 0;
  guint n_types = types && JSON_NODE_HOLDS_ARRAY(types) ? json_array_get_length(json_node_get_array(types)) : 0;
  GtkTextTag **tags = g_new0(GtkTextTag *, MAX(n_types, 1));
  gboolean *variable = g_new0(gboolean, MAX(n_types, 1)); /* readonly ones are constants */
  GtkTextTag *constant = token_tag(tab, CONSTANT_COLOR);
  gint64 line = 0, start = 0, readonly = 0;

  for (guint k = 0; k < n_types; k++) {
    const char *type = json_array_get_string_element(json_node_get_array(types), k);
    const char *color = color_of(type);
    tags[k] = color ? token_tag(tab, color) : NULL;
    variable[k] = g_strcmp0(type, "variable") == 0 || g_strcmp0(type, "property") == 0;
  }
  for (guint bit = 0; modifiers && JSON_NODE_HOLDS_ARRAY(modifiers) &&
                      bit < json_array_get_length(json_node_get_array(modifiers)) && bit < 63;
       bit++)
    if (g_strcmp0(json_array_get_string_element(json_node_get_array(modifiers), bit), "readonly") == 0)
      readonly = (gint64)1 << bit;

  remove_colors(tab);
  for (guint i = 0; i + 4 < n; i += 5) {
    gint64 delta_line = json_array_get_int_element(numbers, i);
    gint64 delta_start = json_array_get_int_element(numbers, i + 1);
    gint64 length = json_array_get_int_element(numbers, i + 2);
    gint64 type = json_array_get_int_element(numbers, i + 3);
    GtkTextTag *tag;
    GtkTextIter from, to;
    line += delta_line;
    start = delta_line ? delta_start : start + delta_start;
    if (type < 0 || type >= n_types || !(tag = tags[type]))
      continue;
    if (variable[type] && (json_array_get_int_element(numbers, i + 4) & readonly))
      tag = constant;
    lsp_iter_at_position(tab, line, start, &from);
    lsp_iter_at_position(tab, line, start + length, &to);
    gtk_text_buffer_apply_tag(buffer, tag, &from, &to);
  }
  raise_tags(gtk_text_buffer_get_tag_table(buffer));
  g_free(variable);
  g_free(tags);
}

static void on_tokens(JsonNode *result, gpointer data)
{
  TokensRequest *request = data;
  Semantic *s = request->semantic;

  if (s && s->request == request) {
    s->request = NULL;
    if (result && request->changes == s->tab->view->changes && !s->tab->app->plain)
      apply(s->tab, result);
  }
  g_free(request);
}

static gboolean update_now(gpointer data)
{
  Semantic *s = data;
  char *document, *params;

  s->update_id = 0;
  if (!lsp_has(s->tab) || !lsp_capability(s->tab, "semanticTokensProvider"))
    return G_SOURCE_REMOVE;
  if (s->request)
    s->request->semantic = NULL;
  if (s->cancel) {
    g_cancellable_cancel(s->cancel);
    g_object_unref(s->cancel);
  }
  s->cancel = g_cancellable_new();
  s->request = g_new0(TokensRequest, 1);
  s->request->semantic = s;
  s->request->changes = s->tab->view->changes;
  document = lsp_document_params(s->tab);
  params = g_strdup_printf("{%s}", document);
  lsp_request(s->tab, "textDocument/semanticTokens/full", params, s->cancel, on_tokens, s->request);
  g_free(params);
  g_free(document);
  return G_SOURCE_REMOVE;
}

void semantic_update(Tab *tab)
{
  Semantic *s = tab->view->semantic;

  if (tab->app->plain) {
    remove_colors(tab);
    return;
  }
  if (!s) {
    s = tab->view->semantic = g_new0(Semantic, 1);
    s->tab = tab;
    g_signal_connect(gtk_text_buffer_get_tag_table(GTK_TEXT_BUFFER(tab->buffer)), "tag-added", G_CALLBACK(on_tag_added),
                     NULL);
  }
  if (s->update_id)
    g_source_remove(s->update_id);
  s->update_id = g_timeout_add(UPDATE_MS, update_now, s);
}

void semantic_free(Semantic *s)
{
  if (!s)
    return;
  if (s->update_id)
    g_source_remove(s->update_id);
  if (s->request)
    s->request->semantic = NULL;
  if (s->cancel) {
    g_cancellable_cancel(s->cancel);
    g_object_unref(s->cancel);
  }
  g_free(s);
}
