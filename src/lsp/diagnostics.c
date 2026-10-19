/*
 * Diagnostics in an editor (see diagnostics.h). Each one keeps its range as two marks, so it moves
 * with the text typed before it until the server sends the next list; a tag per severity draws the
 * wave under it.
 */
#include "lsp/diagnostics.h"

#include "editor/editorview.h"
#include "lsp/client.h"
#include "window/statusbar.h"

/* LSP's severities. */
enum { SEVERITY_ERROR = 1, SEVERITY_WARNING, SEVERITY_INFO, SEVERITY_HINT };

typedef struct {
  GtkTextMark *start, *end;
  int severity;
  char *message;
} Diagnostic;

/* VS Code's editorError / editorWarning / editorInfo foregrounds. */
static const char *const colors[] = { NULL, "#F14C4C", "#CCA700", "#3794FF", "#3794FF" };
static const char *const tag_names[] = { NULL, "lsp-error", "lsp-warning", "lsp-info", "lsp-info" };
static const char *const labels[] = { NULL, "error", "warning", "note", "hint" };

/* The tag for a severity, made in the buffer the first time. */
static GtkTextTag *tag_for(Tab *tab, int severity)
{
  GtkTextTagTable *table = gtk_text_buffer_get_tag_table(GTK_TEXT_BUFFER(tab->buffer));
  GtkTextTag *tag = gtk_text_tag_table_lookup(table, tag_names[severity]);
  GdkRGBA color;

  if (!tag) {
    gdk_rgba_parse(&color, colors[severity]);
    tag = gtk_text_buffer_create_tag(GTK_TEXT_BUFFER(tab->buffer), tag_names[severity], "underline",
                                     PANGO_UNDERLINE_ERROR, "underline-rgba", &color, NULL);
  }
  return tag;
}

static void diagnostic_free(Diagnostic *d)
{
  GtkTextBuffer *buffer = gtk_text_mark_get_buffer(d->start);

  if (buffer) {
    gtk_text_buffer_delete_mark(buffer, d->start);
    gtk_text_buffer_delete_mark(buffer, d->end);
  }
  g_free(d->message);
  g_free(d);
}

/* Takes the waves off and forgets the list. */
static void remove_all(Tab *tab)
{
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(tab->buffer);
  GtkTextIter start, end;

  if (!tab->view->diagnostics)
    return;
  gtk_text_buffer_get_bounds(buffer, &start, &end);
  for (int s = SEVERITY_ERROR; s <= SEVERITY_INFO; s++)
    if (gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table(buffer), tag_names[s]))
      gtk_text_buffer_remove_tag(buffer, tag_for(tab, s), &start, &end);
  g_ptr_array_set_size(tab->view->diagnostics, 0);
}

void diagnostics_clear(Tab *tab)
{
  remove_all(tab);
  g_clear_pointer(&tab->view->diagnostics, g_ptr_array_unref);
}

/* A range with nothing in it (a missing ";") gets the character after it, or else the one before:
 * a wave needs something to go under. */
static void widen(GtkTextIter *start, GtkTextIter *end)
{
  if (!gtk_text_iter_equal(start, end))
    return;
  if (!gtk_text_iter_ends_line(end))
    gtk_text_iter_forward_char(end);
  else if (!gtk_text_iter_starts_line(start))
    gtk_text_iter_backward_char(start);
}

void diagnostics_show(Tab *tab, JsonNode *list)
{
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(tab->buffer);
  JsonArray *array = list && JSON_NODE_HOLDS_ARRAY(list) ? json_node_get_array(list) : NULL;
  guint n = array ? json_array_get_length(array) : 0;

  if (!tab->view->diagnostics)
    tab->view->diagnostics = g_ptr_array_new_with_free_func((GDestroyNotify)diagnostic_free);
  remove_all(tab);
  tab->errors = tab->warnings = 0;
  for (guint i = 0; i < n; i++) {
    JsonNode *node = json_array_get_element(array, i);
    Diagnostic *d = g_new0(Diagnostic, 1);
    GtkTextIter start, end;

    d->severity = (int)CLAMP(json_int_at(node, "severity", SEVERITY_ERROR), SEVERITY_ERROR, SEVERITY_HINT);
    d->message = g_strdup(json_string_at(node, "message") ? json_string_at(node, "message") : "");
    lsp_iter_at(tab, json_at(node, "range.start"), &start);
    lsp_iter_at(tab, json_at(node, "range.end"), &end);
    widen(&start, &end);
    d->start = gtk_text_buffer_create_mark(buffer, NULL, &start, TRUE);
    d->end = gtk_text_buffer_create_mark(buffer, NULL, &end, FALSE);
    if (d->severity != SEVERITY_HINT) /* VS Code draws hints as faint dots: none here */
      gtk_text_buffer_apply_tag(buffer, tag_for(tab, d->severity), &start, &end);
    tab->errors += d->severity == SEVERITY_ERROR;
    tab->warnings += d->severity == SEVERITY_WARNING;
    g_ptr_array_add(tab->view->diagnostics, d);
  }
  editor_view_show_problems(tab);
  update_status(tab->app);
}

char *diagnostics_at(Tab *tab, const GtkTextIter *iter, gboolean markup)
{
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(tab->buffer);
  GString *text = g_string_new(NULL);

  for (guint i = 0; tab->view->diagnostics && i < tab->view->diagnostics->len; i++) {
    Diagnostic *d = g_ptr_array_index(tab->view->diagnostics, i);
    GtkTextIter start, end;
    gtk_text_buffer_get_iter_at_mark(buffer, &start, d->start);
    gtk_text_buffer_get_iter_at_mark(buffer, &end, d->end);
    if (gtk_text_iter_compare(iter, &start) < 0 || gtk_text_iter_compare(iter, &end) > 0)
      continue;
    if (text->len)
      g_string_append_c(text, '\n');
    if (markup) {
      char *escaped = g_markup_escape_text(d->message, -1);
      g_string_append_printf(text, "<span foreground=\"%s\">%s:</span> %s", colors[d->severity], labels[d->severity],
                             escaped);
      g_free(escaped);
    } else {
      g_string_append_printf(text, "%s: %s", labels[d->severity], d->message);
    }
  }
  return g_string_free(text, text->len == 0);
}

void diagnostics_lines(Tab *tab, GArray *errors, GArray *warnings)
{
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(tab->buffer);

  for (guint i = 0; tab->view->diagnostics && i < tab->view->diagnostics->len; i++) {
    Diagnostic *d = g_ptr_array_index(tab->view->diagnostics, i);
    GtkTextIter start;
    int line;
    gtk_text_buffer_get_iter_at_mark(buffer, &start, d->start);
    line = gtk_text_iter_get_line(&start);
    if (d->severity == SEVERITY_ERROR)
      g_array_append_val(errors, line);
    else if (d->severity == SEVERITY_WARNING)
      g_array_append_val(warnings, line);
  }
}
