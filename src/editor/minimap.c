/*
 * The minimap next to each editor (see minimap.h): every line of the saved file as thin colored
 * bars, like GNOME Text Editor's, clangd's problem lines across it in red and yellow, and the
 * lines the editor shows shaded. Clicking or dragging it scrolls the editor.
 */
#include "editor/minimap.h"

#include "editor/editorview.h"
#include "editor/lexer.h"

#include <stdlib.h>

#define MINIMAP_WIDTH 100
#define LINE_PITCH 3     /* pixels from one line of the minimap to the next */
#define LINE_HEIGHT 2

/* micro/colorschemes/dark-modern.micro: identifier, comment, constant.string, constant.number,
 * keyword (also macros and the name after #define), type, statement (also preprocessor
 * directives), identifier.func, identifier.constant, symbol. */
static const char *const ink_hex[INK_COUNT] = {
    "#9CDCFE", "#6A9955", "#CE9178", "#B5CEA8", "#569CD6", "#4EC9B0", "#C586C0", "#DCDCAA", "#4FC1FF", "#CCCCCC",
};
static GdkRGBA inks[INK_COUNT], plain_ink, error_ink, warning_ink;

void minimap_init_colors(void)
{
    for (int i = 0; i < INK_COUNT; i++) {
        gdk_rgba_parse(&inks[i], ink_hex[i]);
        inks[i].alpha = 0.75;
    }
    gdk_rgba_parse(&plain_ink, "#cccccc");
    plain_ink.alpha = 0.45;
    gdk_rgba_parse(&error_ink, "#f14c4c");   /* gutter-error */
    gdk_rgba_parse(&warning_ink, "#cca700"); /* gutter-warning */
}

/* Drawing ------------------------------------------------------------------------------------ */

/* Where the minimap starts (in pixels of the whole file), so that the shown lines stay in view:
 * all of it while it fits, else it moves along with the editor like VS Code's. */
static double minimap_offset(EditorView *v, int area)
{
    Tab *tab = v->tab;
    double whole = (double)tab->lines * LINE_PITCH;
    int most = MAX(1, tab->lines - tab->height);

    if (whole <= area)
        return 0;
    return MIN(1.0, (double)tab->top / most) * (whole - area);
}

/* The line at the top of the editor that puts the viewport's top at y. */
static int top_for_viewport(EditorView *v, double y, int area)
{
    Tab *tab = v->tab;
    double whole = (double)tab->lines * LINE_PITCH;
    int most = MAX(1, tab->lines - tab->height);
    double per_line = whole <= area ? LINE_PITCH : (area - (double)tab->height * LINE_PITCH) / most;

    if (per_line <= 0) /* the viewport is as tall as the minimap: go by proportion */
        return (int)(y / area * most);
    return (int)(y / per_line + 0.5);
}

/* PaintFunc for code coloring: a bar of the run's color, cut at the minimap's width. */
static void paint_run(cairo_t *cr, int column, int length, Ink ink)
{
    if (column >= MINIMAP_WIDTH)
        return;
    gdk_cairo_set_source_rgba(cr, ink == INK_COUNT ? &plain_ink : &inks[ink]);
    cairo_rectangle(cr, column, 0, MIN(length, MINIMAP_WIDTH - column), LINE_HEIGHT);
    cairo_fill(cr);
}

/* PaintFunc without code coloring: every run in one gray. */
static void paint_plain(cairo_t *cr, int column, int length, Ink ink G_GNUC_UNUSED)
{
    paint_run(cr, column, length, INK_COUNT);
}

/* Draws the visible part of the file, the problem lines over it and the shaded viewport (the
 * lines the editor shows). */
static void draw_minimap(GtkDrawingArea *area G_GNUC_UNUSED, cairo_t *cr, int width, int height, gpointer data)
{
    EditorView *v = data;
    Tab *tab = v->tab;
    double offset = minimap_offset(v, height);
    int first = (int)(offset / LINE_PITCH), n = (int)v->starts->len;

    for (int i = first; v->text && i < n && (i - first) * LINE_PITCH < height + LINE_PITCH; i++) {
        gsize start = g_array_index(v->starts, gsize, i);
        gsize end = i + 1 < n ? g_array_index(v->starts, gsize, i + 1) - 1 : v->size;
        cairo_save(cr);
        cairo_translate(cr, 2, i * LINE_PITCH - offset);
        lex_line(v->text + start, end - start, g_array_index(v->in_comment, gboolean, i), cr,
                 tab->app->plain || !v->c_like ? paint_plain : paint_run);
        cairo_restore(cr);
    }
    /* Problem lines: the whole width, over the text. */
    for (guint k = 0; k < v->problem_lines->len; k++) {
        Problem *p = &g_array_index(v->problem_lines, Problem, k);
        double y = p->line * LINE_PITCH - offset;
        if (y < -LINE_PITCH || y > height)
            continue;
        gdk_cairo_set_source_rgba(cr, p->error ? &error_ink : &warning_ink);
        cairo_rectangle(cr, 0, y, width, LINE_PITCH);
        cairo_fill(cr);
    }
    if (tab->lines > 0 && tab->height > 0) {
        cairo_set_source_rgba(cr, 1, 1, 1, v->dragging ? 0.14 : 0.08);
        cairo_rectangle(cr, 0, tab->top * LINE_PITCH - offset, width, tab->height * LINE_PITCH);
        cairo_fill(cr);
    }
}

/* Scrolling with it -------------------------------------------------------------------------- */

/* A press on the minimap grabs the viewport, after centering it there when the press is outside it. */
static void on_drag_begin(GtkGestureDrag *gesture G_GNUC_UNUSED, double x G_GNUC_UNUSED, double y, EditorView *v)
{
    Tab *tab = v->tab;
    int area = gtk_widget_get_height(v->minimap);
    double offset = minimap_offset(v, area);
    double view_y = tab->top * LINE_PITCH - offset, view_h = tab->height * LINE_PITCH;

    /* A click outside the viewport centers it there; then dragging moves it. */
    if (y < view_y || y > view_y + view_h) {
        int line = (int)((y + offset) / LINE_PITCH);
        editor_view_scroll(v, line - tab->height / 2);
        view_y = tab->top * LINE_PITCH - minimap_offset(v, area);
    }
    v->grab = y - view_y;
    v->dragging = TRUE;
    gtk_widget_grab_focus(GTK_WIDGET(tab->term));
}

/* Dragging moves the viewport with the pointer. */
static void on_drag_update(GtkGestureDrag *gesture, double dx G_GNUC_UNUSED, double dy, EditorView *v)
{
    double x, y;

    gtk_gesture_drag_get_start_point(gesture, &x, &y);
    editor_view_scroll(v, top_for_viewport(v, y + dy - v->grab, gtk_widget_get_height(v->minimap)));
}

/* The drag is over: the viewport shade goes back to its normal brightness. */
static void on_drag_end(GtkGestureDrag *gesture G_GNUC_UNUSED, double dx G_GNUC_UNUSED, double dy G_GNUC_UNUSED,
                        EditorView *v)
{
    v->dragging = FALSE;
    gtk_widget_queue_draw(v->minimap);
}

/* The wheel over the minimap scrolls the editor three lines a step. */
static gboolean on_minimap_wheel(GtkEventControllerScroll *scroll G_GNUC_UNUSED, double dx G_GNUC_UNUSED,
                                 double dy, EditorView *v)
{
    editor_view_scroll(v, v->tab->top + (int)(dy * 3));
    return TRUE;
}

/* The widget and the problem lines ----------------------------------------------------------- */

GtkWidget *minimap_new(EditorView *v)
{
    GtkWidget *minimap = gtk_drawing_area_new();
    GtkGesture *drag = gtk_gesture_drag_new();
    GtkEventController *wheel = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);

    gtk_widget_add_css_class(minimap, "minimap");
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(minimap), MINIMAP_WIDTH + 4);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(minimap), draw_minimap, v, NULL);
    g_signal_connect(drag, "drag-begin", G_CALLBACK(on_drag_begin), v);
    g_signal_connect(drag, "drag-update", G_CALLBACK(on_drag_update), v);
    g_signal_connect(drag, "drag-end", G_CALLBACK(on_drag_end), v);
    gtk_widget_add_controller(minimap, GTK_EVENT_CONTROLLER(drag));
    g_signal_connect(wheel, "scroll", G_CALLBACK(on_minimap_wheel), v);
    gtk_widget_add_controller(minimap, wheel);
    return minimap;
}

/* micro's problem lines ("e12,w30", or "-" for none) for the minimap. */
void minimap_set_problems(Tab *tab, const char *spec)
{
    EditorView *v = tab->view;
    char **marks;

    if (!v || g_strcmp0(v->problems, spec) == 0)
        return;
    g_set_str(&v->problems, spec);
    g_array_set_size(v->problem_lines, 0);
    marks = g_strsplit(spec, ",", -1);
    for (int i = 0; marks[i]; i++) {
        int line = atoi(marks[i] + (marks[i][0] ? 1 : 0));
        if ((marks[i][0] == 'e' || marks[i][0] == 'w') && line > 0)
            g_array_append_val(v->problem_lines, ((Problem){ line - 1, marks[i][0] == 'e' }));
    }
    g_strfreev(marks);
    gtk_widget_queue_draw(v->minimap);
}
