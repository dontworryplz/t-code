/*
 * An editor tab's page (see editorview.h): building it, the copy of the saved file that the
 * minimap draws and the guides fall back on, and scrolling: micro reports what it shows, and the
 * scrollbar and the minimap scroll it back through the control pipe.
 */
#include "editor/editorview.h"

#include "editor/control.h"
#include "editor/guides.h"
#include "editor/lexer.h"
#include "editor/minimap.h"

#include <string.h>

#define MAX_TEXT (4 << 20)   /* bigger files get no minimap */
#define OWN_SCROLL_US 300000 /* reports this soon after our own scroll may predate it */

/* The file ----------------------------------------------------------------------------------- */

static void clear_text(EditorView *v)
{
    g_clear_pointer(&v->text, g_free);
    v->size = 0;
    g_array_set_size(v->starts, 0);
    g_array_set_size(v->in_comment, 0);
}

/* Reads the saved file into v->text, with where each line starts and whether it starts inside a
 * block comment (so the minimap can color any line without lexing all lines before it). */
static void load_text(EditorView *v)
{
    char *text = NULL;
    gsize size = 0;
    gboolean comment = FALSE;

    clear_text(v);
    v->text_version++;
    if (!v->tab->path || !g_file_get_contents(v->tab->path, &text, &size, NULL) || size > MAX_TEXT) {
        g_free(text);
        gtk_widget_queue_draw(v->minimap);
        return;
    }
    v->text = text;
    v->size = size;
    for (gsize start = 0; start <= size;) {
        const char *newline = memchr(text + start, '\n', size - start);
        gsize end = newline ? (gsize)(newline - text) : size;
        g_array_append_val(v->starts, start);
        g_array_append_val(v->in_comment, comment);
        if (v->c_like)
            comment = lex_line(text + start, end - start, comment, NULL, NULL);
        if (!newline)
            break;
        start = end + 1;
    }
    gtk_widget_queue_draw(v->minimap);
}

/* on_file_changed()'s timeout. */
static gboolean reload_now(gpointer data)
{
    EditorView *v = data;

    v->reload_id = 0;
    load_text(v);
    return G_SOURCE_REMOVE;
}

/* The file was saved (by micro or anything else): read it again shortly, once for a burst. */
static void on_file_changed(GFileMonitor *monitor G_GNUC_UNUSED, GFile *file G_GNUC_UNUSED,
                            GFile *other G_GNUC_UNUSED, GFileMonitorEvent event G_GNUC_UNUSED, EditorView *v)
{
    if (!v->reload_id)
        v->reload_id = g_timeout_add(200, reload_now, v);
}

/* The lexer knows C and C++ only: other files (Makefiles, scripts, Markdown) would get '#'
 * comments drawn as preprocessor lines and apostrophes as unclosed character literals. */
static gboolean is_c_like(const char *path)
{
    static const char *const extensions[] = {
        ".c", ".h", ".cc", ".cpp", ".cxx", ".c++", ".hh", ".hpp", ".hxx", ".h++", ".inl", ".ipp", ".tpp", NULL,
    };
    const char *dot = path ? strrchr(path, '.') : NULL;

    for (int i = 0; dot && extensions[i]; i++)
        if (g_ascii_strcasecmp(dot, extensions[i]) == 0)
            return TRUE;
    return FALSE;
}

void editor_view_set_path(Tab *tab)
{
    EditorView *v = tab->view;
    GFile *file;

    if (!v)
        return;
    v->c_like = is_c_like(tab->path);
    if (v->monitor) {
        g_file_monitor_cancel(v->monitor);
        g_clear_object(&v->monitor);
    }
    file = g_file_new_for_path(tab->path);
    v->monitor = g_file_monitor_file(file, G_FILE_MONITOR_WATCH_MOVES, NULL, NULL);
    if (v->monitor)
        g_signal_connect(v->monitor, "changed", G_CALLBACK(on_file_changed), v);
    g_object_unref(file);
    load_text(v);
}

/* Scrolling ---------------------------------------------------------------------------------- */

void editor_view_scroll(EditorView *v, int top)
{
    Tab *tab = v->tab;
    int most = MAX(0, tab->lines - tab->height);
    char *line;

    top = CLAMP(top, 0, most);
    if (top == tab->top)
        return;
    tab->top = top;
    v->scrolled_at = g_get_monotonic_time();
    line = g_strdup_printf("scroll %d", top);
    editor_control(tab, line);
    g_free(line);
    v->updating = TRUE;
    gtk_adjustment_set_value(v->adjustment, top);
    v->updating = FALSE;
    gtk_widget_queue_draw(v->minimap);
}

void editor_view_update(Tab *tab, int top)
{
    EditorView *v = tab->view;

    if (!v)
        return;
    /* While t-code itself scrolls, reports from before that would pull the view back. */
    if (g_get_monotonic_time() - v->scrolled_at > OWN_SCROLL_US)
        tab->top = top;
    v->updating = TRUE;
    gtk_adjustment_configure(v->adjustment, tab->top, 0, MAX(MAX(tab->lines, tab->top + tab->height), 1),
                             1, MAX(tab->height - 1, 1), MAX(tab->height, 1));
    v->updating = FALSE;
    gtk_widget_queue_draw(v->minimap);
    gtk_widget_queue_draw(v->guides);
}

/* The user moved the scrollbar (not a report from micro moving it). */
static void on_scrollbar(GtkAdjustment *adjustment, EditorView *v)
{
    if (!v->updating)
        editor_view_scroll(v, (int)(gtk_adjustment_get_value(adjustment) + 0.5));
}

/* Setting up --------------------------------------------------------------------------------- */

static const char *const css =
    ".tcode-editor scrollbar { background: #1f1f1f; border: none; }"
    ".tcode-editor scrollbar slider { min-width: 6px; margin: 2px; border-radius: 6px; background: rgba(121, 121, 121, .4); }"
    ".tcode-editor scrollbar slider:hover { background: rgba(100, 100, 100, .7); }"
    ".tcode-editor .minimap { background: #1f1f1f; }"
    ".tcode-editor vte-terminal { padding: 1px; }"; /* TERM_PADDING */

GtkWidget *editor_page(VteTerminal *term)
{
    static gboolean styled;
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0), *overlay;

    if (!styled) { /* the first editor: the CSS and colors all editors share */
        GtkCssProvider *provider = gtk_css_provider_new();
        gtk_css_provider_load_from_string(provider, css);
        gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
                                                   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_object_unref(provider);
        minimap_init_colors();
        guides_init_colors();
        styled = TRUE;
    }
    gtk_widget_add_css_class(page, "tcode-editor");
    gtk_widget_set_hexpand(GTK_WIDGET(term), TRUE);
    overlay = gtk_overlay_new(); /* the guides go over the terminal */
    gtk_widget_set_hexpand(overlay, TRUE);
    gtk_overlay_set_child(GTK_OVERLAY(overlay), GTK_WIDGET(term));
    gtk_box_append(GTK_BOX(page), overlay);
    return page;
}

const char *editor_view_init(Tab *tab, int number)
{
    EditorView *v = g_new0(EditorView, 1);
    GtkWidget *scrollbar;

    tab->view = v;
    v->tab = tab;
    v->starts = g_array_new(FALSE, FALSE, sizeof(gsize));
    v->in_comment = g_array_new(FALSE, FALSE, sizeof(gboolean));
    v->problem_lines = g_array_new(FALSE, FALSE, sizeof(Problem));

    guides_attach(v, GTK_OVERLAY(gtk_widget_get_parent(GTK_WIDGET(tab->term))));

    v->minimap = minimap_new(v);
    gtk_widget_set_visible(v->minimap, tab->app->settings.minimap);
    gtk_box_append(GTK_BOX(tab->page), v->minimap);

    v->adjustment = gtk_adjustment_new(0, 0, 1, 1, 1, 1);
    g_signal_connect(v->adjustment, "value-changed", G_CALLBACK(on_scrollbar), v);
    scrollbar = gtk_scrollbar_new(GTK_ORIENTATION_VERTICAL, v->adjustment);
    gtk_box_append(GTK_BOX(tab->page), scrollbar);

    v->control_fd = control_open(number, &v->control_path);
    editor_view_set_path(tab);
    return v->control_path;
}

void editor_view_show_minimap(Tab *tab, gboolean show)
{
    if (tab->view)
        gtk_widget_set_visible(tab->view->minimap, show);
}

void editor_view_redraw(Tab *tab)
{
    if (tab->view) {
        gtk_widget_queue_draw(tab->view->minimap);
        gtk_widget_queue_draw(tab->view->guides);
    }
}

/* Closing the pipe ends the plugin's reader (cat). */
void editor_view_free(EditorView *v)
{
    if (!v)
        return;
    control_close(v->control_fd, v->control_path);
    if (v->monitor) {
        g_file_monitor_cancel(v->monitor);
        g_object_unref(v->monitor);
    }
    if (v->reload_id)
        g_source_remove(v->reload_id);
    g_free(v->text);
    g_array_free(v->starts, TRUE);
    g_array_free(v->in_comment, TRUE);
    g_array_free(v->problem_lines, TRUE);
    g_free(v->problems);
    g_free(v);
}
