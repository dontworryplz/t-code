/*
 * The diff gutter (see diffgutter.h): a GtkSourceGutterRenderer drawing each line's mark. The base
 * text comes from git show (HEAD or a commit) or from a file; the marks are worked out again
 * (git/linediff.c) a moment after the text or the base changes.
 */
#include "editor/diffgutter.h"

#include "editor/editorview.h"
#include "editor/tabs.h"
#include "git/linediff.h"

#define UPDATE_MS 300 /* after the last change */
#define WIDTH 4       /* pixels */

typedef enum { BASE_HEAD, BASE_REV, BASE_FILE, BASE_NONE } BaseKind;

typedef struct Loading Loading;

struct DiffGutter {
  Tab *tab;
  GtkWidget *renderer;
  BaseKind kind;
  char *value;           /* the commit, or the file */
  char *base;            /* its text; NULL: no marks (not in git, a new file) */
  GArray *marks;         /* guint8 LineMark per line, NULL before the first */
  Tab *partner;          /* the other tab in a side-by-side comparison, or NULL */
  gboolean is_old_side;  /* TRUE for the right-hand (older) tab in a comparison */
  GArray *gaps;          /* DiffGap per hunk gap when partner != NULL */
  GPtrArray *gap_tags;   /* GtkTextTag* created for the current gaps */
  int line_height;       /* single-line pixel height used for the current gaps */
  gboolean base_pending; /* git show waits for the tab to be shown */
  guint update_id;
  Loading *loading; /* the git show in flight */
};

static void sync_pair_gaps(DiffGutter *g);

/* A git show; dropped when gutter is NULL (the editor went, or another base was asked for). */
struct Loading {
  DiffGutter *gutter;
};

/* The renderer ----------------------------------------------------------------------------- */

struct _TcodeDiffRenderer {
  GtkSourceGutterRenderer parent;
  DiffGutter *gutter;
};

#define TCODE_TYPE_DIFF_RENDERER (tcode_diff_renderer_get_type())
G_DECLARE_FINAL_TYPE(TcodeDiffRenderer, tcode_diff_renderer, TCODE, DIFF_RENDERER, GtkSourceGutterRenderer)
G_DEFINE_FINAL_TYPE(TcodeDiffRenderer, tcode_diff_renderer, GTK_SOURCE_TYPE_GUTTER_RENDERER)

/* VS Code's editorGutter.addedBackground, .modifiedBackground, .deletedBackground. */
static const GdkRGBA added = { 0.18, 0.63, 0.26, 1 }, modified = { 0.0, 0.47, 0.83, 1 },
                     deleted = { 0.97, 0.32, 0.29, 1 };

static void snapshot_line(GtkSourceGutterRenderer *renderer, GtkSnapshot *snapshot, GtkSourceGutterLines *lines,
                          guint line)
{
  DiffGutter *g = TCODE_DIFF_RENDERER(renderer)->gutter;
  double y, height;
  int width = gtk_widget_get_width(GTK_WIDGET(renderer));
  guint8 mark;

  if (!g || !g->marks || line >= g->marks->len || !(mark = g_array_index(g->marks, guint8, line)))
    return;
  gtk_source_gutter_lines_get_line_extent(lines, line, GTK_SOURCE_GUTTER_RENDERER_ALIGNMENT_MODE_FIRST, &y, &height);
  if (mark == LINE_DELETED_ABOVE) { /* a small triangle at the line's top */
    GskPathBuilder *builder = gsk_path_builder_new();
    GskPath *path;
    gsk_path_builder_move_to(builder, 0, y - 4);
    gsk_path_builder_line_to(builder, width, y);
    gsk_path_builder_line_to(builder, 0, y + 4);
    gsk_path_builder_close(builder);
    path = gsk_path_builder_free_to_path(builder);
    gtk_snapshot_append_fill(snapshot, path, GSK_FILL_RULE_WINDING, &deleted);
    gsk_path_unref(path);
  } else {
    const GdkRGBA *color = (mark == LINE_ADDED) ? (g->partner && g->is_old_side ? &deleted : &added) : &modified;
    gtk_snapshot_append_color(snapshot, color, &GRAPHENE_RECT_INIT(0, y, MIN(width, 3), height));
  }
}

static void tcode_diff_renderer_class_init(TcodeDiffRendererClass *klass)
{
  GTK_SOURCE_GUTTER_RENDERER_CLASS(klass)->snapshot_line = snapshot_line;
}

static void tcode_diff_renderer_init(TcodeDiffRenderer *self)
{
  gtk_widget_set_size_request(GTK_WIDGET(self), WIDTH, -1);
}

/* Marks ------------------------------------------------------------------------------------- */

static gboolean update_marks_now(gpointer data)
{
  DiffGutter *g = data;
  GtkTextIter start, end;
  char *text;

  g->update_id = 0;
  g_clear_pointer(&g->marks, g_array_unref);
  if (g->base) {
    gtk_text_buffer_get_bounds(GTK_TEXT_BUFFER(g->tab->buffer), &start, &end);
    text = gtk_text_buffer_get_text(GTK_TEXT_BUFFER(g->tab->buffer), &start, &end, TRUE);
    g->marks = line_diff(g->base, text);
    g_free(text);
  }
  if (g->partner)
    sync_pair_gaps(g);
  gtk_widget_queue_draw(g->renderer);
  return G_SOURCE_REMOVE;
}

static void update_marks_later(DiffGutter *g)
{
  if (!g->update_id)
    g->update_id = g_timeout_add(UPDATE_MS, update_marks_now, g);
}

static void on_text_changed(GtkTextBuffer *buffer G_GNUC_UNUSED, DiffGutter *g)
{
  if (!g->tab->view->loading)
    update_marks_later(g);
}

/* The base ----------------------------------------------------------------------------------- */

static void set_base(DiffGutter *g, char *text)
{
  g_free(g->base);
  g->base = text;
  update_marks_later(g);
}

static void on_shown(GObject *source, GAsyncResult *result, gpointer data)
{
  Loading *loading = data;
  DiffGutter *g = loading->gutter;
  char *out = NULL;
  gboolean ok = g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &out, NULL, NULL) &&
                g_subprocess_get_if_exited(G_SUBPROCESS(source)) &&
                g_subprocess_get_exit_status(G_SUBPROCESS(source)) == 0;

  if (g && g->loading == loading) {
    g->loading = NULL;
    set_base(g, ok ? g_steal_pointer(&out) : NULL);
  }
  g_free(out);
  g_free(loading);
}

/* Reads the base again: git show in the background, or the file. */
static void load_base(DiffGutter *g)
{
  char *dir, *name, *spec;
  GSubprocess *process;

  if (g->loading) {
    g->loading->gutter = NULL;
    g->loading = NULL;
  }
  if (g->kind == BASE_NONE || !g->tab->path) {
    set_base(g, NULL);
    return;
  }
  if (g->kind == BASE_FILE) {
    char *text = NULL;
    g_file_get_contents(g->value, &text, NULL, NULL);
    set_base(g, text);
    return;
  }
  dir = g_path_get_dirname(g->tab->path);
  name = g_path_get_basename(g->tab->path);
  spec = g_strconcat(g->kind == BASE_HEAD ? "HEAD" : g->value, ":./", name, NULL);
  process = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL, "git", "-C", dir,
                             "show", spec, NULL);
  if (process) {
    g->loading = g_new0(Loading, 1);
    g->loading->gutter = g;
    g_subprocess_communicate_utf8_async(process, NULL, NULL, on_shown, g->loading);
    g_object_unref(process);
  }
  g_free(spec);
  g_free(name);
  g_free(dir);
}

/* Reads the base again, now or (git show, for a tab not shown: a restored session would start one
 * git per tab) once the tab is shown. */
static void load_base_when_shown(DiffGutter *g)
{
  g->base_pending = (g->kind == BASE_HEAD || g->kind == BASE_REV) && !gtk_widget_get_mapped(GTK_WIDGET(g->tab->source));
  if (!g->base_pending)
    load_base(g);
}

static void set_kind(Tab *tab, BaseKind kind, const char *value)
{
  DiffGutter *g = tab->view->diff;

  g->kind = kind;
  g_free(g->value);
  g->value = g_strdup(value);
  if (!tab->view->loading)
    load_base_when_shown(g);
}

void diff_gutter_follow_head(Tab *tab)
{
  set_kind(tab, BASE_HEAD, NULL);
}

void diff_gutter_compare_rev(Tab *tab, const char *rev)
{
  set_kind(tab, rev ? BASE_REV : BASE_NONE, rev);
}

void diff_gutter_compare_file(Tab *tab, const char *path)
{
  set_kind(tab, path ? BASE_FILE : BASE_NONE, path);
}

/* Takes the gap rows out of the buffer (the tags that make them). The view is not touched: it
 * may be gone already (the tab closing). */
static void remove_gap_tags(DiffGutter *g)
{
  if (g->gap_tags) {
    GtkTextTagTable *table = gtk_text_buffer_get_tag_table(GTK_TEXT_BUFFER(g->tab->buffer));
    for (guint i = 0; i < g->gap_tags->len; i++)
      gtk_text_tag_table_remove(table, g_ptr_array_index(g->gap_tags, i));
    g_ptr_array_set_size(g->gap_tags, 0);
  }
  g_clear_pointer(&g->gaps, g_array_unref);
}

/* The same, and the view draws again. */
static void clear_side_gaps(DiffGutter *g)
{
  remove_gap_tags(g);
  gtk_widget_queue_draw(GTK_WIDGET(g->tab->source));
}

/* One line's height, from the font: a line's own extent would include the gap rows. */
static int measure_line_height(Tab *tab)
{
  PangoLayout *layout = gtk_widget_create_pango_layout(GTK_WIDGET(tab->source), "Mg");
  int height = 0;

  pango_layout_get_pixel_size(layout, NULL, &height);
  g_object_unref(layout);
  return MAX(height, 1);
}

static void apply_side_gaps(DiffGutter *g, GArray *gaps, int line_h)
{
  GtkTextBuffer *buf = GTK_TEXT_BUFFER(g->tab->buffer);

  if (!g->gap_tags)
    g->gap_tags = g_ptr_array_new();
  g->gaps = gaps;
  g->line_height = line_h;
  for (guint i = 0; i < gaps->len; i++) {
    DiffGap *gap = &g_array_index(gaps, DiffGap, i);
    GtkTextTag *tag = gtk_text_buffer_create_tag(buf, NULL, gap->above ? "pixels-above-lines" : "pixels-below-lines",
                                                 gap->count * line_h, NULL);
    GtkTextIter start, end;
    gtk_text_buffer_get_iter_at_line(buf, &start, gap->line);
    end = start;
    if (!gtk_text_iter_ends_line(&end))
      gtk_text_iter_forward_to_line_end(&end);
    else
      gtk_text_iter_forward_char(&end);
    gtk_text_buffer_apply_tag(buf, tag, &start, &end);
    g_ptr_array_add(g->gap_tags, tag);
  }
  gtk_widget_queue_draw(GTK_WIDGET(g->tab->source));
}

static void sync_pair_gaps(DiffGutter *g)
{
  if (!g || !g->partner)
    return;
  Tab *new_tab = g->is_old_side ? g->partner : g->tab;
  Tab *old_tab = g->is_old_side ? g->tab : g->partner;
  if (!new_tab->view || !old_tab->view || new_tab->view->loading || old_tab->view->loading)
    return;
  DiffGutter *ng = new_tab->view->diff, *og = old_tab->view->diff;
  GtkTextIter s1, e1, s2, e2;

  clear_side_gaps(ng);
  clear_side_gaps(og);
  int line_h = measure_line_height(new_tab);
  gtk_text_buffer_get_bounds(GTK_TEXT_BUFFER(new_tab->buffer), &s1, &e1);
  gtk_text_buffer_get_bounds(GTK_TEXT_BUFFER(old_tab->buffer), &s2, &e2);
  char *new_text = gtk_text_buffer_get_text(GTK_TEXT_BUFFER(new_tab->buffer), &s1, &e1, TRUE);
  char *old_text = gtk_text_buffer_get_text(GTK_TEXT_BUFFER(old_tab->buffer), &s2, &e2, TRUE);

  apply_side_gaps(ng, line_diff_gaps(old_text, new_text, FALSE), line_h);
  apply_side_gaps(og, line_diff_gaps(old_text, new_text, TRUE), line_h);
  g_free(new_text);
  g_free(old_text);
}

void diff_gutter_pair(Tab *new_tab, Tab *old_tab)
{
  if (new_tab && new_tab->view && new_tab->view->diff) {
    DiffGutter *ng = new_tab->view->diff;
    if (ng->partner && ng->partner != old_tab && ng->partner->view && ng->partner->view->diff) {
      clear_side_gaps(ng->partner->view->diff);
      ng->partner->view->diff->partner = NULL;
    }
    if (!old_tab) {
      clear_side_gaps(ng);
      ng->partner = NULL;
      return;
    }
  }
  if (!new_tab || !old_tab || !new_tab->view || !old_tab->view)
    return;
  DiffGutter *ng = new_tab->view->diff, *og = old_tab->view->diff;
  ng->partner = old_tab;
  ng->is_old_side = FALSE;
  og->partner = new_tab;
  og->is_old_side = TRUE;
  sync_pair_gaps(ng);
}

gboolean diff_gutter_line_gaps(Tab *tab, int line, int *above_px, int *below_px)
{
  *above_px = *below_px = 0;
  if (!tab || !tab->view || !tab->view->diff || !tab->view->diff->partner || !tab->view->diff->gaps)
    return FALSE;
  DiffGutter *g = tab->view->diff;
  for (guint i = 0; i < g->gaps->len; i++) {
    DiffGap *gap = &g_array_index(g->gaps, DiffGap, i);
    if (gap->line == line) {
      if (gap->above)
        *above_px += gap->count * g->line_height;
      else
        *below_px += gap->count * g->line_height;
    }
  }
  return !g->is_old_side;
}

static void on_source_mapped(GtkWidget *widget G_GNUC_UNUSED, DiffGutter *g)
{
  if (g->base_pending) {
    g->base_pending = FALSE;
    load_base(g);
  }
  if (g->partner)
    sync_pair_gaps(g);
}

void diff_gutter_loaded(Tab *tab)
{
  load_base_when_shown(tab->view->diff);
  if (tab->view->diff->partner)
    sync_pair_gaps(tab->view->diff);
}

void diff_gutter_head_moved(App *t)
{
  int n = tab_count(t);

  for (int i = 0; i < n; i++) {
    Tab *tab = tab_nth(t, i);
    if (tab->kind == TAB_EDITOR && tab->view->diff->kind == BASE_HEAD && !tab->view->loading)
      load_base_when_shown(tab->view->diff);
  }
}

void diff_gutter_attach(Tab *tab)
{
  DiffGutter *g = g_new0(DiffGutter, 1);
  GtkSourceGutter *gutter = gtk_source_view_get_gutter(tab->source, GTK_TEXT_WINDOW_LEFT);

  g->tab = tab;
  g->kind = BASE_HEAD;
  g->renderer = g_object_ref_sink(g_object_new(TCODE_TYPE_DIFF_RENDERER, NULL)); /* ours too: see below */
  TCODE_DIFF_RENDERER(g->renderer)->gutter = g;
  /* Right of the line numbers (-30) and the marks (-20). */
  gtk_source_gutter_insert(gutter, GTK_SOURCE_GUTTER_RENDERER(g->renderer), -10);
  g_signal_connect(tab->buffer, "changed", G_CALLBACK(on_text_changed), g);
  g_signal_connect(tab->source, "map", G_CALLBACK(on_source_mapped), g);
  tab->view->diff = g;
}

void diff_gutter_free(DiffGutter *g)
{
  if (!g)
    return;
  if (g->partner && g->partner->view && g->partner->view->diff) { /* the other side stays: no gaps there */
    clear_side_gaps(g->partner->view->diff);
    g->partner->view->diff->partner = NULL;
  }
  remove_gap_tags(g);
  if (g->gap_tags)
    g_ptr_array_free(g->gap_tags, TRUE);
  /* The view's "map" handler went with the view, which is gone (the page goes first). */
  g_signal_handlers_disconnect_by_data(g->tab->buffer, g);
  /* The page went first: the renderer lives on with our reference, and must not draw for us. */
  TCODE_DIFF_RENDERER(g->renderer)->gutter = NULL;
  g_object_unref(g->renderer);
  if (g->update_id)
    g_source_remove(g->update_id);
  if (g->loading)
    g->loading->gutter = NULL;
  if (g->marks)
    g_array_unref(g->marks);
  g_free(g->base);
  g_free(g->value);
  g_free(g);
}
