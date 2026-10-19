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

  if (!tag) {
    tag = gtk_text_buffer_create_tag(GTK_TEXT_BUFFER(tab->buffer), tag_names[severity], NULL);
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
    d->end = gtk_text_buffer_create_mark(buffer, NULL, &end, TRUE); /* text typed right after is not in it */
    if (d->severity != SEVERITY_HINT)                               /* VS Code draws hints as faint dots: none here */
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

static void draw_wave(cairo_t *cr, double x1, double x2, double y)
{
  double p = 3.6;
  double amp = 0.8;
  double x = x1;

  cairo_move_to(cr, x, y);
  while (x + p <= x2) {
    cairo_curve_to(cr, x + p * 0.25, y - amp, x + p * 0.25, y - amp, x + p * 0.5, y);
    cairo_curve_to(cr, x + p * 0.75, y + amp, x + p * 0.75, y + amp, x + p, y);
    x += p;
  }
  if (x + p * 0.5 <= x2) {
    cairo_curve_to(cr, x + p * 0.25, y - amp, x + p * 0.25, y - amp, x + p * 0.5, y);
    x += p * 0.5;
  }
  if (x < x2)
    cairo_line_to(cr, x2, y);
}

void diagnostics_draw(Tab *tab, GtkTextView *view, GtkSnapshot *snapshot)
{
  if (!tab || !tab->view || !tab->view->diagnostics || tab->view->diagnostics->len == 0)
    return;

  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(tab->buffer);
  GdkRectangle visible;
  gtk_text_view_get_visible_rect(view, &visible);

  GtkTextIter line_iter;
  gtk_text_view_get_line_at_y(view, &line_iter, visible.y, NULL);
  int first_line = gtk_text_iter_get_line(&line_iter);
  gtk_text_view_get_line_at_y(view, &line_iter, visible.y + visible.height, NULL);
  int last_line = gtk_text_iter_get_line(&line_iter);

  graphene_rect_t bounds =
      GRAPHENE_RECT_INIT((float)visible.x, (float)visible.y, (float)visible.width, (float)visible.height);
  cairo_t *cr = NULL;

  static const double rgb[][3] = {
    { 0.0, 0.0, 0.0 },
    { 0.945, 0.298, 0.298 }, /* SEVERITY_ERROR: #F14C4C */
    { 0.800, 0.655, 0.000 }, /* SEVERITY_WARNING: #CCA700 */
    { 0.216, 0.580, 1.000 }, /* SEVERITY_INFO: #3794FF */
    { 0.216, 0.580, 1.000 }  /* SEVERITY_HINT */
  };

  for (guint i = 0; i < tab->view->diagnostics->len; i++) {
    Diagnostic *d = g_ptr_array_index(tab->view->diagnostics, i);
    if (d->severity < SEVERITY_ERROR || d->severity > SEVERITY_INFO)
      continue;

    GtkTextIter start, end;
    gtk_text_buffer_get_iter_at_mark(buffer, &start, d->start);
    gtk_text_buffer_get_iter_at_mark(buffer, &end, d->end);

    int s_line = gtk_text_iter_get_line(&start);
    int e_line = gtk_text_iter_get_line(&end);
    if (e_line < first_line || s_line > last_line)
      continue;

    if (!cr) {
      cr = gtk_snapshot_append_cairo(snapshot, &bounds);
      cairo_set_line_width(cr, 1.1);
      cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
      cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    }

    cairo_set_source_rgb(cr, rgb[d->severity][0], rgb[d->severity][1], rgb[d->severity][2]);

    for (int line = MAX(s_line, first_line); line <= MIN(e_line, last_line); line++) {
      GtkTextIter line_start, line_end;
      gtk_text_buffer_get_iter_at_line(buffer, &line_start, line);
      line_end = line_start;
      if (!gtk_text_iter_ends_line(&line_end))
        gtk_text_iter_forward_to_line_end(&line_end);

      GtkTextIter seg_start = (line == s_line) ? start : line_start;
      GtkTextIter seg_end = (line == e_line) ? end : line_end;

      if (gtk_text_iter_compare(&seg_start, &seg_end) >= 0)
        continue;

      GdkRectangle r1, r2;
      gtk_text_view_get_iter_location(view, &seg_start, &r1);
      gtk_text_view_get_iter_location(view, &seg_end, &r2);

      double x1 = r1.x;
      double x2 = r2.x;
      if (x2 <= x1)
        x2 = x1 + MAX(r1.width, 8);
      double y = r1.y + r1.height - 1.2;

      draw_wave(cr, x1, x2, y);
      cairo_stroke(cr);
    }
  }

  if (cr)
    cairo_destroy(cr);
}
