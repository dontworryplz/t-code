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
  char *value;   /* the commit, or the file */
  char *base;    /* its text; NULL: no marks (not in git, a new file) */
  GArray *marks; /* guint8 LineMark per line, NULL before the first */
  guint update_id;
  Loading *loading; /* the git show in flight */
};

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
  gtk_source_gutter_lines_get_line_extent(lines, line, GTK_SOURCE_GUTTER_RENDERER_ALIGNMENT_MODE_CELL, &y, &height);
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
    gtk_snapshot_append_color(snapshot, mark == LINE_ADDED ? &added : &modified,
                              &GRAPHENE_RECT_INIT(0, y, MIN(width, 3), height));
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

static void set_kind(Tab *tab, BaseKind kind, const char *value)
{
  DiffGutter *g = tab->view->diff;

  g->kind = kind;
  g_free(g->value);
  g->value = g_strdup(value);
  if (!tab->view->loading)
    load_base(g);
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

void diff_gutter_loaded(Tab *tab)
{
  load_base(tab->view->diff);
}

void diff_gutter_head_moved(App *t)
{
  int n = tab_count(t);

  for (int i = 0; i < n; i++) {
    Tab *tab = tab_nth(t, i);
    if (tab->kind == TAB_EDITOR && tab->view->diff->kind == BASE_HEAD && !tab->view->loading)
      load_base(tab->view->diff);
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
  tab->view->diff = g;
}

void diff_gutter_free(DiffGutter *g)
{
  if (!g)
    return;
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
