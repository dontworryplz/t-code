/*
 * Breakpoints in the editor (see breakpoints.h). In an open file each breakpoint is a source mark at
 * the end of its line: Enter at the line's start takes it down with the text, Enter at its end leaves
 * it. t->debug.breakpoints holds every file's lines; an open file's are read again from its marks
 * after each change, and the debugger is told when they moved.
 */
#include "editor/breakpoints.h"

#include "debug/debugger.h"
#include "editor/editorview.h"
#include "editor/tabs.h"
#include "window/palette.h"

#include <stdlib.h>
#include <string.h>

#define CATEGORY "tcode-breakpoint"
#define DOT_WIDTH 16 /* pixels */

struct Breakpoints {
  Tab *tab;
  GtkWidget *renderer;
  int hover;         /* the line under the pointer, or -1 */
  gboolean placed;   /* the marks are in (loaded): until then the text says nothing about the lines */
  GtkTextMark *stop; /* at the start of the line the program stopped at, or NULL */
};

/* The lines ------------------------------------------------------------------------------------ */

static GHashTable *table(App *t)
{
  if (!t->debug.breakpoints)
    t->debug.breakpoints = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_array_unref);
  return t->debug.breakpoints;
}

GArray *breakpoints_in(App *t, const char *path)
{
  return path && t->debug.breakpoints ? g_hash_table_lookup(t->debug.breakpoints, path) : NULL;
}

/* Whether two lists of lines are the same. */
static gboolean same_lines(GArray *a, GArray *b)
{
  guint na = a ? a->len : 0, nb = b ? b->len : 0;

  return na == nb && (na == 0 || memcmp(a->data, b->data, na * sizeof(int)) == 0);
}

/* path's lines are lines now (empty or NULL: none); the debugger hears of a change. Takes lines. */
static void set_lines(App *t, const char *path, GArray *lines)
{
  GArray *old = breakpoints_in(t, path);

  if (lines && lines->len == 0)
    g_clear_pointer(&lines, g_array_unref);
  if (same_lines(old, lines)) {
    if (lines)
      g_array_unref(lines);
    return;
  }
  if (lines)
    g_hash_table_replace(table(t), g_strdup(path), lines);
  else
    g_hash_table_remove(table(t), path);
  debug_breakpoints_changed(t, path);
}

/* Whether this editor takes breakpoints: not an old version (read-only, in a temporary folder). */
static gboolean can_break(Tab *tab)
{
  return tab->view && !tab->view->readonly && tab->path;
}

/* The marks ------------------------------------------------------------------------------------ */

/* The breakpoint marks on line, or NULL (free the list). */
static GSList *marks_on(Tab *tab, int line)
{
  return gtk_source_buffer_get_source_marks_at_line(tab->buffer, line, CATEGORY);
}

static void add_mark(Tab *tab, int line)
{
  GtkTextIter end;

  gtk_text_buffer_get_iter_at_line(GTK_TEXT_BUFFER(tab->buffer), &end, line);
  if (!gtk_text_iter_ends_line(&end))
    gtk_text_iter_forward_to_line_end(&end);
  gtk_source_buffer_create_source_mark(tab->buffer, NULL, CATEGORY, &end);
}

/* The lines of the marks in the text as it is now (1-based, ascending); two that came onto one line
 * (the line break between them was deleted) become one. */
static GArray *lines_of_marks(Tab *tab)
{
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(tab->buffer);
  GArray *lines = g_array_new(FALSE, FALSE, sizeof(int));
  GtkTextIter iter;

  gtk_text_buffer_get_start_iter(buffer, &iter);
  GSList *first = gtk_source_buffer_get_source_marks_at_iter(tab->buffer, &iter, CATEGORY);
  g_slist_free(first);
  if (!first && !gtk_source_buffer_forward_iter_to_source_mark(tab->buffer, &iter, CATEGORY))
    return lines;
  do {
    int line = gtk_text_iter_get_line(&iter) + 1;
    GSList *here = gtk_source_buffer_get_source_marks_at_iter(tab->buffer, &iter, CATEGORY);
    if (lines->len == 0 || g_array_index(lines, int, lines->len - 1) != line)
      g_array_append_val(lines, line);
    else /* a second mark on that line */
      for (GSList *m = here; m; m = m->next)
        gtk_text_buffer_delete_mark(buffer, m->data);
    g_slist_free(here);
  } while (gtk_source_buffer_forward_iter_to_source_mark(tab->buffer, &iter, CATEGORY));
  return lines;
}

/* The table follows the marks. */
static void sync_lines(Tab *tab)
{
  set_lines(tab->app, tab->path, lines_of_marks(tab));
  gtk_widget_queue_draw(tab->view->breakpoints->renderer);
}

static void remove_marks(Tab *tab)
{
  GtkTextIter start, end;

  gtk_text_buffer_get_bounds(GTK_TEXT_BUFFER(tab->buffer), &start, &end);
  gtk_source_buffer_remove_source_marks(tab->buffer, &start, &end, CATEGORY);
}

/* The marks follow the table (the file was loaded, the breakpoints came from the session). */
static void place_marks(Tab *tab)
{
  GArray *lines = breakpoints_in(tab->app, tab->path);
  int count = gtk_text_buffer_get_line_count(GTK_TEXT_BUFFER(tab->buffer));

  remove_marks(tab);
  tab->view->breakpoints->placed = TRUE;
  for (guint i = 0; lines && i < lines->len; i++)
    if (g_array_index(lines, int, i) <= count)
      add_mark(tab, g_array_index(lines, int, i) - 1);
  gtk_widget_queue_draw(tab->view->breakpoints->renderer);
}

void breakpoint_toggle(Tab *tab, int line)
{
  GSList *marks;

  if (!can_break(tab) || tab->view->loading)
    return;
  if (!tab->view->breakpoints->placed)
    place_marks(tab);
  marks = marks_on(tab, line);
  if (marks)
    for (GSList *m = marks; m; m = m->next)
      gtk_text_buffer_delete_mark(GTK_TEXT_BUFFER(tab->buffer), m->data);
  else
    add_mark(tab, line);
  g_slist_free(marks);
  sync_lines(tab);
}

void breakpoint_toggle_at_cursor(Tab *tab)
{
  GtkTextIter cursor;

  gtk_text_buffer_get_iter_at_mark(GTK_TEXT_BUFFER(tab->buffer), &cursor,
                                   gtk_text_buffer_get_insert(GTK_TEXT_BUFFER(tab->buffer)));
  breakpoint_toggle(tab, gtk_text_iter_get_line(&cursor));
}

/* The text changed: the breakpoints may be on other lines now. Not while the loader replaces it:
 * once loaded, the marks are put back from the table. */
static void on_changed(GtkTextBuffer *buffer G_GNUC_UNUSED, Breakpoints *b)
{
  if (b->placed && !b->tab->view->loading && can_break(b->tab) && breakpoints_in(b->tab->app, b->tab->path))
    sync_lines(b->tab);
}

/* The stopped line ----------------------------------------------------------------------------- */

/* The stopped line's mark goes where t->debug says (or away). */
static void place_stop(Tab *tab)
{
  Breakpoints *b = tab->view->breakpoints;
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(tab->buffer);
  App *t = tab->app;
  gboolean here = t->debug.stop_path && g_strcmp0(t->debug.stop_path, tab->path) == 0 && !tab->view->loading;

  if (b->stop) {
    gtk_text_buffer_delete_mark(buffer, b->stop);
    b->stop = NULL;
  }
  if (here && t->debug.stop_line >= 1 && t->debug.stop_line <= gtk_text_buffer_get_line_count(buffer)) {
    GtkTextIter start;
    gtk_text_buffer_get_iter_at_line(buffer, &start, t->debug.stop_line - 1);
    b->stop = gtk_text_buffer_create_mark(buffer, NULL, &start, FALSE);
  }
  gtk_widget_queue_draw(b->renderer);
  gtk_widget_queue_draw(GTK_WIDGET(tab->source));
}

/* The stopped line in tab, or -1. */
static int stop_line(Tab *tab)
{
  Breakpoints *b = tab->view ? tab->view->breakpoints : NULL;
  GtkTextIter at;

  if (!b || !b->stop)
    return -1;
  gtk_text_buffer_get_iter_at_mark(GTK_TEXT_BUFFER(tab->buffer), &at, b->stop);
  return gtk_text_iter_get_line(&at);
}

void breakpoints_show_stop(App *t, const char *path, int line)
{
  char *old = t->debug.stop_path;

  t->debug.stop_path = g_strdup(path);
  t->debug.stop_line = line;
  for (int i = 0; i < tab_count(t); i++) {
    Tab *tab = tab_nth(t, i);
    if (tab->kind == TAB_EDITOR && tab->view && tab->view->breakpoints &&
        (g_strcmp0(tab->path, old) == 0 || g_strcmp0(tab->path, path) == 0))
      place_stop(tab);
  }
  g_free(old);
}

void breakpoints_draw(Tab *tab, GtkTextView *view, GtkSnapshot *snapshot)
{
  int line = stop_line(tab);
  GtkTextIter start;
  GdkRectangle visible;
  GdkRGBA color;
  int y, height;

  if (line < 0)
    return;
  gtk_text_buffer_get_iter_at_line(GTK_TEXT_BUFFER(tab->buffer), &start, line);
  gtk_text_view_get_line_yrange(view, &start, &y, &height);
  gtk_text_view_get_visible_rect(view, &visible);
  color = *palette_rgba(PAL_WARNING);
  color.alpha = 0.22f;
  gtk_snapshot_append_color(snapshot, &color, &GRAPHENE_RECT_INIT(visible.x, y, visible.width, height));
}

/* Loading, saving ------------------------------------------------------------------------------ */

void breakpoints_loaded(Tab *tab)
{
  if (!tab->view || !tab->view->breakpoints)
    return;
  if (can_break(tab))
    place_marks(tab);
  place_stop(tab);
}

/* Every open editor puts its marks where the table says. */
static void place_everywhere(App *t)
{
  for (int i = 0; i < tab_count(t); i++) {
    Tab *tab = tab_nth(t, i);
    if (tab->kind == TAB_EDITOR && tab->view && tab->view->breakpoints && !tab->view->loading && can_break(tab))
      place_marks(tab);
  }
}

void breakpoints_remove_all(App *t)
{
  GList *paths = t->debug.breakpoints ? g_hash_table_get_keys(t->debug.breakpoints) : NULL;
  char **copy = g_new0(char *, g_list_length(paths) + 1);
  int n = 0;

  for (GList *p = paths; p; p = p->next)
    copy[n++] = g_strdup(p->data);
  g_list_free(paths);
  for (int i = 0; i < n; i++)
    set_lines(t, copy[i], NULL);
  g_strfreev(copy);
  place_everywhere(t);
}

char **breakpoints_save(App *t)
{
  GPtrArray *list = g_ptr_array_new();
  GHashTableIter iter;
  gpointer path, lines;

  if (t->debug.breakpoints) {
    g_hash_table_iter_init(&iter, t->debug.breakpoints);
    while (g_hash_table_iter_next(&iter, &path, &lines))
      for (guint i = 0; i < ((GArray *)lines)->len; i++)
        g_ptr_array_add(list, g_strdup_printf("%d:%s", g_array_index((GArray *)lines, int, i), (char *)path));
  }
  if (list->len == 0) {
    g_ptr_array_free(list, TRUE);
    return NULL;
  }
  g_ptr_array_add(list, NULL);
  return (char **)g_ptr_array_free(list, FALSE);
}

static int compare_lines(gconstpointer a, gconstpointer b)
{
  return *(const int *)a - *(const int *)b;
}

void breakpoints_restore(App *t, char **saved)
{
  g_clear_pointer(&t->debug.breakpoints, g_hash_table_destroy);
  for (int i = 0; saved && saved[i]; i++) {
    char *end;
    long line = strtol(saved[i], &end, 10);
    if (*end != ':' || line < 1 || !g_path_is_absolute(end + 1))
      continue;
    GArray *lines = g_hash_table_lookup(table(t), end + 1);
    if (!lines) {
      lines = g_array_new(FALSE, FALSE, sizeof(int));
      g_hash_table_insert(table(t), g_strdup(end + 1), lines);
    }
    int n = (int)line;
    g_array_append_val(lines, n);
  }
  if (t->debug.breakpoints) {
    GHashTableIter iter;
    gpointer lines;
    g_hash_table_iter_init(&iter, t->debug.breakpoints);
    while (g_hash_table_iter_next(&iter, NULL, &lines))
      g_array_sort(lines, compare_lines);
  }
  place_everywhere(t);
}

/* The dots ------------------------------------------------------------------------------------- */

struct _TcodeBreakpointRenderer {
  GtkSourceGutterRenderer parent;
  Breakpoints *breakpoints;
};

#define TCODE_TYPE_BREAKPOINT_RENDERER (tcode_breakpoint_renderer_get_type())
G_DECLARE_FINAL_TYPE(TcodeBreakpointRenderer, tcode_breakpoint_renderer, TCODE, BREAKPOINT_RENDERER,
                     GtkSourceGutterRenderer)
G_DEFINE_FINAL_TYPE(TcodeBreakpointRenderer, tcode_breakpoint_renderer, GTK_SOURCE_TYPE_GUTTER_RENDERER)

static void draw_dot(GtkSnapshot *snapshot, double y, double height, GdkRGBA color)
{
  double size = MIN(10, height - 4);
  GskRoundedRect dot;

  gsk_rounded_rect_init_from_rect(
      &dot, &GRAPHENE_RECT_INIT((DOT_WIDTH - size) / 2, y + (height - size) / 2, size, size), size / 2);
  gtk_snapshot_push_rounded_clip(snapshot, &dot);
  gtk_snapshot_append_color(snapshot, &color, &dot.bounds);
  gtk_snapshot_pop(snapshot);
}

/* VS Code's arrow at the stopped line: a box pointing right. */
static void draw_arrow(GtkSnapshot *snapshot, double y, double height)
{
  GskPathBuilder *builder = gsk_path_builder_new();
  double h = MIN(10, height - 4), top = y + (height - h) / 2, left = 2, tip = DOT_WIDTH - 2;
  GskPath *path;

  gsk_path_builder_move_to(builder, left, top);
  gsk_path_builder_line_to(builder, tip - h / 2, top);
  gsk_path_builder_line_to(builder, tip, top + h / 2);
  gsk_path_builder_line_to(builder, tip - h / 2, top + h);
  gsk_path_builder_line_to(builder, left, top + h);
  gsk_path_builder_close(builder);
  path = gsk_path_builder_free_to_path(builder);
  gtk_snapshot_append_fill(snapshot, path, GSK_FILL_RULE_WINDING, palette_rgba(PAL_WARNING));
  gsk_path_unref(path);
}

static void snapshot_dot(GtkSourceGutterRenderer *renderer, GtkSnapshot *snapshot, GtkSourceGutterLines *lines,
                         guint line)
{
  Breakpoints *b = TCODE_BREAKPOINT_RENDERER(renderer)->breakpoints;
  GSList *marks;
  double y, height;
  GdkRGBA red;

  if (!b || !b->tab->view || b->tab->view->loading)
    return;
  gtk_source_gutter_lines_get_line_extent(lines, line, GTK_SOURCE_GUTTER_RENDERER_ALIGNMENT_MODE_FIRST, &y, &height);
  red = *palette_rgba(PAL_ERROR);
  marks = marks_on(b->tab, (int)line);
  if (marks)
    draw_dot(snapshot, y, height, red);
  else if (b->hover == (int)line && can_break(b->tab)) {
    red.alpha = 0.4f;
    draw_dot(snapshot, y, height, red);
  }
  g_slist_free(marks);
  if (stop_line(b->tab) == (int)line)
    draw_arrow(snapshot, y, height);
}

static gboolean query_dot(GtkSourceGutterRenderer *renderer, GtkTextIter *iter G_GNUC_UNUSED,
                          GdkRectangle *area G_GNUC_UNUSED)
{
  Breakpoints *b = TCODE_BREAKPOINT_RENDERER(renderer)->breakpoints;

  return b && can_break(b->tab);
}

static void activate_dot(GtkSourceGutterRenderer *renderer, GtkTextIter *iter, GdkRectangle *area G_GNUC_UNUSED,
                         guint button G_GNUC_UNUSED, GdkModifierType state G_GNUC_UNUSED, int n_presses G_GNUC_UNUSED)
{
  Breakpoints *b = TCODE_BREAKPOINT_RENDERER(renderer)->breakpoints;

  if (b)
    breakpoint_toggle(b->tab, gtk_text_iter_get_line(iter));
}

static void tcode_breakpoint_renderer_class_init(TcodeBreakpointRendererClass *klass)
{
  GtkSourceGutterRendererClass *renderer = GTK_SOURCE_GUTTER_RENDERER_CLASS(klass);

  renderer->snapshot_line = snapshot_dot;
  renderer->query_activatable = query_dot;
  renderer->activate = activate_dot;
}

static void tcode_breakpoint_renderer_init(TcodeBreakpointRenderer *self)
{
  gtk_widget_set_size_request(GTK_WIDGET(self), DOT_WIDTH, -1);
}

/* The faint dot follows the pointer. */
static void on_motion(GtkEventControllerMotion *motion G_GNUC_UNUSED, double x G_GNUC_UNUSED, double y, Breakpoints *b)
{
  GtkTextView *view = GTK_TEXT_VIEW(b->tab->source);
  GtkTextIter at;
  int buffer_y, line;

  gtk_text_view_window_to_buffer_coords(view, GTK_TEXT_WINDOW_LEFT, 0, (int)y, NULL, &buffer_y);
  gtk_text_view_get_line_at_y(view, &at, buffer_y, NULL);
  line = gtk_text_iter_get_line(&at);
  if (line != b->hover) {
    b->hover = line;
    gtk_widget_queue_draw(b->renderer);
  }
}

static void on_leave(GtkEventControllerMotion *motion G_GNUC_UNUSED, Breakpoints *b)
{
  b->hover = -1;
  gtk_widget_queue_draw(b->renderer);
}

void breakpoints_attach(Tab *tab)
{
  Breakpoints *b = g_new0(Breakpoints, 1);
  GtkSourceGutter *gutter = gtk_source_view_get_gutter(tab->source, GTK_TEXT_WINDOW_LEFT);
  GtkEventController *motion = gtk_event_controller_motion_new();

  b->tab = tab;
  b->hover = -1;
  b->renderer = g_object_ref_sink(g_object_new(TCODE_TYPE_BREAKPOINT_RENDERER, NULL)); /* ours too: see free */
  TCODE_BREAKPOINT_RENDERER(b->renderer)->breakpoints = b;
  gtk_source_gutter_insert(gutter, GTK_SOURCE_GUTTER_RENDERER(b->renderer), -40); /* left of the line numbers */
  g_signal_connect(motion, "motion", G_CALLBACK(on_motion), b);
  g_signal_connect(motion, "leave", G_CALLBACK(on_leave), b);
  gtk_widget_add_controller(b->renderer, motion);
  g_signal_connect(tab->buffer, "changed", G_CALLBACK(on_changed), b);
  tab->view->breakpoints = b;
}

void breakpoints_free(Breakpoints *b)
{
  if (!b)
    return;
  g_signal_handlers_disconnect_by_data(b->tab->buffer, b);
  /* The page went first: the renderer lives on with our reference, and must not reach us. */
  TCODE_BREAKPOINT_RENDERER(b->renderer)->breakpoints = NULL;
  g_object_unref(b->renderer);
  g_free(b); /* the marks go with the buffer */
}
