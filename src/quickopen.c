/*
 * Ctrl+P: open a file by typing part of its name, like VS Code's Quick Open.
 * The file list comes from git when the project is a repository (so .gitignore applies),
 * otherwise from walking the project, skipping hidden folders and build output.
 */
#include "app.h"

#include <string.h>

#define MAX_RESULTS 50
#define MAX_WALK 20000

struct QuickOpen {
    App *app;
    GtkWidget *box;
    GtkSearchEntry *entry;
    GtkListBox *list;
    GPtrArray *files; /* paths relative to the project root */
};

static const char *const css =
    ".tcode-quickopen { background: #222222; border: 1px solid #454545; border-radius: 6px;"
    "  box-shadow: 0 4px 16px rgba(0,0,0,.6); padding: 6px; }"
    ".tcode-quickopen list { background: transparent; }"
    ".tcode-quickopen row { padding: 3px 8px; border-radius: 3px; }"
    ".tcode-quickopen row:selected { background: #04395e; }"
    ".tcode-quickopen .dir { color: #9d9d9d; font-size: smaller; }";

/* File list ---------------------------------------------------------------------------------- */

static gboolean from_git(QuickOpen *q)
{
    char *argv[] = { "git", "-C", q->app->root, "ls-files", "-z", "-co", "--exclude-standard", NULL };
    char *out = NULL;
    int status;

    if (!g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL,
                      &out, NULL, &status, NULL) ||
        !g_spawn_check_wait_status(status, NULL)) {
        g_free(out);
        return FALSE;
    }
    for (const char *p = out; *p; p += strlen(p) + 1)
        g_ptr_array_add(q->files, g_strdup(p));
    g_free(out);
    return TRUE;
}

static void walk(QuickOpen *q, const char *dir, const char *prefix)
{
    GDir *d = g_dir_open(dir, 0, NULL);
    const char *name;

    if (!d)
        return;
    while ((name = g_dir_read_name(d)) && q->files->len < MAX_WALK) {
        char *path = g_build_filename(dir, name, NULL);
        char *rel = *prefix ? g_strconcat(prefix, "/", name, NULL) : g_strdup(name);
        if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
            if (name[0] != '.' && strcmp(name, "build") != 0 && strcmp(name, "node_modules") != 0)
                walk(q, path, rel);
            g_free(rel);
        } else {
            g_ptr_array_add(q->files, rel);
        }
        g_free(path);
    }
    g_dir_close(d);
}

static void load_files(QuickOpen *q)
{
    g_ptr_array_set_size(q->files, 0);
    if (!from_git(q))
        walk(q, q->app->root, "");
}

/* Matching ----------------------------------------------------------------------------------- */

/* Higher is better; -1 when the query letters do not all appear in order. Matches at the start
 * of a name part and runs of consecutive letters count more, and so do matches in the file name. */
static int score(const char *query, const char *path)
{
    const char *base = strrchr(path, '/');
    const char *p = path;
    int total = 0, prev = -2;

    base = base ? base + 1 : path;
    for (const char *q = query; *q; q++) {
        char c = g_ascii_tolower(*q);
        while (*p && g_ascii_tolower(*p) != c)
            p++;
        if (!*p)
            return -1;
        int at = p - path;
        total += 1;
        if (at == prev + 1)
            total += 5;
        if (at == 0 || strchr("/_-. ", path[at - 1]))
            total += 8;
        if (p >= base)
            total += 3;
        prev = at;
        p++;
    }
    return total * 16 - (int)strlen(path);
}

typedef struct {
    const char *path;
    int score;
} Match;

static int by_score(gconstpointer a, gconstpointer b)
{
    const Match *x = a, *y = b;

    return x->score != y->score ? y->score - x->score : strcmp(x->path, y->path);
}

static GtkWidget *result_row(const char *path)
{
    GtkWidget *row = gtk_list_box_row_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    char *name = g_path_get_basename(path);
    char *dir = g_path_get_dirname(path);
    GtkWidget *name_label = gtk_label_new(name);
    GtkWidget *dir_label = gtk_label_new(strcmp(dir, ".") == 0 ? "" : dir);

    gtk_widget_add_css_class(dir_label, "dir");
    gtk_label_set_ellipsize(GTK_LABEL(dir_label), PANGO_ELLIPSIZE_START);
    gtk_box_append(GTK_BOX(box), name_label);
    gtk_box_append(GTK_BOX(box), dir_label);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
    g_object_set_data_full(G_OBJECT(row), "path", g_strdup(path), g_free);
    g_free(dir);
    g_free(name);
    return row;
}

static void refilter(QuickOpen *q)
{
    const char *query = gtk_editable_get_text(GTK_EDITABLE(q->entry));
    GArray *matches = g_array_new(FALSE, FALSE, sizeof(Match));

    gtk_list_box_remove_all(q->list);
    for (guint i = 0; i < q->files->len; i++) {
        const char *path = g_ptr_array_index(q->files, i);
        int s = *query ? score(query, path) : 0;
        if (s >= 0 || !*query) {
            Match m = { path, s };
            g_array_append_val(matches, m);
        }
    }
    if (*query)
        g_array_sort(matches, by_score);
    for (guint i = 0; i < matches->len && i < MAX_RESULTS; i++)
        gtk_list_box_append(q->list, result_row(g_array_index(matches, Match, i).path));
    gtk_list_box_select_row(q->list, gtk_list_box_get_row_at_index(q->list, 0));
    g_array_free(matches, TRUE);
}

/* Showing and choosing ------------------------------------------------------------------------- */

static void hide(QuickOpen *q)
{
    gtk_widget_set_visible(q->box, FALSE);
    focus_current_tab(q->app);
}

static void choose(QuickOpen *q, GtkListBoxRow *row)
{
    if (!row)
        return;
    char *path = g_build_filename(q->app->root, g_object_get_data(G_OBJECT(row), "path"), NULL);
    gtk_widget_set_visible(q->box, FALSE);
    open_file(q->app, path);
    g_free(path);
}

static void on_activate(GtkSearchEntry *entry G_GNUC_UNUSED, QuickOpen *q)
{
    choose(q, gtk_list_box_get_selected_row(q->list));
}

static void on_row_activated(GtkListBox *list G_GNUC_UNUSED, GtkListBoxRow *row, QuickOpen *q)
{
    choose(q, row);
}

/* Up and Down move through the results while typing goes on in the entry. */
static gboolean on_key(GtkEventControllerKey *controller G_GNUC_UNUSED, guint key, guint code G_GNUC_UNUSED,
                       GdkModifierType state G_GNUC_UNUSED, QuickOpen *q)
{
    if (key != GDK_KEY_Up && key != GDK_KEY_Down)
        return FALSE;
    GtkListBoxRow *row = gtk_list_box_get_selected_row(q->list);
    int index = row ? gtk_list_box_row_get_index(row) + (key == GDK_KEY_Down ? 1 : -1) : 0;
    GtkListBoxRow *next = gtk_list_box_get_row_at_index(q->list, MAX(index, 0));
    if (next) {
        gtk_list_box_select_row(q->list, next);
        gtk_widget_grab_focus(GTK_WIDGET(q->entry));
    }
    return TRUE;
}

void quick_open_init(App *t, GtkOverlay *overlay)
{
    QuickOpen *q = g_new0(QuickOpen, 1);
    GtkCssProvider *provider = gtk_css_provider_new();
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkEventController *keys = gtk_event_controller_key_new();

    q->app = t;
    q->files = g_ptr_array_new_with_free_func(g_free);
    q->box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    q->entry = GTK_SEARCH_ENTRY(gtk_search_entry_new());
    q->list = GTK_LIST_BOX(gtk_list_box_new());
    t->quick_open = q;

    gtk_css_provider_load_from_string(provider, css);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);

    gtk_widget_add_css_class(q->box, "tcode-quickopen");
    gtk_widget_set_size_request(q->box, 600, -1);
    gtk_widget_set_halign(q->box, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(q->box, GTK_ALIGN_START);
    gtk_widget_set_margin_top(q->box, 40);
    gtk_search_entry_set_placeholder_text(q->entry, "Search files by name");
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 420);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(q->list));
    gtk_box_append(GTK_BOX(q->box), GTK_WIDGET(q->entry));
    gtk_box_append(GTK_BOX(q->box), scroll);

    g_signal_connect_swapped(q->entry, "search-changed", G_CALLBACK(refilter), q);
    g_signal_connect(q->entry, "activate", G_CALLBACK(on_activate), q);
    g_signal_connect_swapped(q->entry, "stop-search", G_CALLBACK(hide), q);
    g_signal_connect(q->list, "row-activated", G_CALLBACK(on_row_activated), q);
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key), q);
    gtk_widget_add_controller(GTK_WIDGET(q->entry), keys);

    gtk_widget_set_visible(q->box, FALSE);
    gtk_overlay_add_overlay(overlay, q->box);
}

void quick_open_show(App *t)
{
    QuickOpen *q = t->quick_open;

    if (gtk_widget_get_visible(q->box)) {
        hide(q);
        return;
    }
    load_files(q);
    gtk_editable_set_text(GTK_EDITABLE(q->entry), "");
    refilter(q);
    gtk_widget_set_visible(q->box, TRUE);
    gtk_widget_grab_focus(GTK_WIDGET(q->entry));
}
