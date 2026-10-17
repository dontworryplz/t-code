/*
 * Remembers the window between runs, in ~/.config/t-code/session.ini:
 *   [settings]          the Settings window's choices
 *   [window]            size, pane sizes, which panes are shown, text size
 *   [recent]            the last opened project folders
 *   [project <sha1>]    per project folder: the open tabs of both groups and the active ones
 */
#include "app.h"

#include <string.h>

#define PREVIEW_PREFIX "preview:"
#define MAX_RECENT 10

static const char *const tab_keys[GROUPS] = { "tabs", "tabs-side" };
static const char *const active_keys[GROUPS] = { "active", "active-side" };

static char *session_file(void)
{
    return g_build_filename(g_get_user_config_dir(), "t-code", "session.ini", NULL);
}

static GKeyFile *load(void)
{
    GKeyFile *kf = g_key_file_new();
    char *path = session_file();

    g_key_file_load_from_file(kf, path, G_KEY_FILE_KEEP_COMMENTS, NULL);
    g_free(path);
    return kf;
}

static void store(GKeyFile *kf)
{
    char *path = session_file();
    char *dir = g_path_get_dirname(path);

    g_mkdir_with_parents(dir, 0700);
    g_key_file_save_to_file(kf, path, NULL);
    g_free(dir);
    g_free(path);
}

static char *project_group(App *t)
{
    char *sum = g_compute_checksum_for_string(G_CHECKSUM_SHA1, t->root, -1);
    char *group = g_strdup_printf("project %s", sum);

    g_free(sum);
    return group;
}

static int get_int(GKeyFile *kf, const char *group, const char *key, int fallback)
{
    GError *error = NULL;
    int value = g_key_file_get_integer(kf, group, key, &error);

    if (error) {
        g_error_free(error);
        return fallback;
    }
    return value;
}

static gboolean get_bool(GKeyFile *kf, const char *group, const char *key, gboolean fallback)
{
    GError *error = NULL;
    gboolean value = g_key_file_get_boolean(kf, group, key, &error);

    if (error) {
        g_error_free(error);
        return fallback;
    }
    return value;
}

void session_load_settings(App *t)
{
    GKeyFile *kf = load();

    t->settings.minimap = get_bool(kf, "settings", "minimap", TRUE);
    t->settings.reopen_last = get_bool(kf, "settings", "reopen-last-project", TRUE);
    t->settings.autosave = get_bool(kf, "settings", "autosave", TRUE);
    t->settings.tabs_to_spaces = get_bool(kf, "settings", "tabs-to-spaces", TRUE);
    t->settings.tab_size = CLAMP(get_int(kf, "settings", "tab-size", 4), 1, 16);
    t->settings.detect_indent = get_bool(kf, "settings", "detect-indentation", TRUE);
    t->settings.guides = get_bool(kf, "settings", "indent-guides", TRUE);
    t->plain = !get_bool(kf, "settings", "code-coloring", TRUE);
    g_free(t->settings.font);
    g_free(t->settings.fm_font);
    t->settings.font = g_key_file_get_string(kf, "settings", "font", NULL);
    t->settings.fm_font = g_key_file_get_string(kf, "settings", "file-manager-font", NULL);
    if (!t->settings.font || !*t->settings.font)
        g_set_str(&t->settings.font, DEFAULT_FONT);
    if (!t->settings.fm_font || !*t->settings.fm_font)
        g_set_str(&t->settings.fm_font, DEFAULT_FONT);
    g_key_file_free(kf);
}

void session_save_settings(App *t)
{
    GKeyFile *kf = load();

    g_key_file_set_boolean(kf, "settings", "minimap", t->settings.minimap);
    g_key_file_set_boolean(kf, "settings", "reopen-last-project", t->settings.reopen_last);
    g_key_file_set_boolean(kf, "settings", "autosave", t->settings.autosave);
    g_key_file_set_boolean(kf, "settings", "tabs-to-spaces", t->settings.tabs_to_spaces);
    g_key_file_set_integer(kf, "settings", "tab-size", t->settings.tab_size);
    g_key_file_set_boolean(kf, "settings", "detect-indentation", t->settings.detect_indent);
    g_key_file_set_boolean(kf, "settings", "indent-guides", t->settings.guides);
    g_key_file_set_boolean(kf, "settings", "code-coloring", !t->plain);
    g_key_file_set_string(kf, "settings", "font", t->settings.font);
    g_key_file_set_string(kf, "settings", "file-manager-font", t->settings.fm_font);
    g_key_file_set_double(kf, "window", "font-scale", t->font_scale);
    store(kf);
    g_key_file_free(kf);
}

void session_apply_window(App *t)
{
    GKeyFile *kf = load();
    GError *error = NULL;
    double scale = g_key_file_get_double(kf, "window", "font-scale", &error);

    gtk_window_set_default_size(t->window, get_int(kf, "window", "width", 1400),
                                get_int(kf, "window", "height", 900));
    if (get_bool(kf, "window", "maximized", FALSE))
        gtk_window_maximize(t->window);
    gtk_paned_set_position(t->hpaned, get_int(kf, "window", "files-width", 280));
    gtk_paned_set_position(t->vpaned, get_int(kf, "window", "editor-height", 620));
    gtk_widget_set_visible(t->sidebar, get_bool(kf, "window", "files-visible", TRUE));
    gtk_widget_set_visible(GTK_WIDGET(t->shell), get_bool(kf, "window", "shell-visible", TRUE));
    char *page = g_key_file_get_string(kf, "window", "sidebar-page", NULL);
    if (g_strcmp0(page, "scm") == 0)
        gtk_stack_set_visible_child_name(t->sidebar_stack, "scm");
    g_free(page);
    if (!error && scale > 0)
        set_font_scale(t, scale);
    g_clear_error(&error);
    g_key_file_free(kf);
}

/* Opens the tabs saved for one group into the active group; returns how many opened. */
static int restore_group(App *t, GKeyFile *kf, const char *group_name, int g)
{
    char **tabs = g_key_file_get_string_list(kf, group_name, tab_keys[g], NULL, NULL);
    int opened = 0;

    for (int i = 0; tabs && tabs[i]; i++) {
        gboolean preview = g_str_has_prefix(tabs[i], PREVIEW_PREFIX);
        const char *path = preview ? tabs[i] + strlen(PREVIEW_PREFIX) : tabs[i];
        if (!g_file_test(path, G_FILE_TEST_IS_REGULAR))
            continue;
        if (preview)
            open_preview(t, path);
        else
            open_file(t, path);
        opened++;
    }
    if (opened)
        gtk_notebook_set_current_page(t->groups[g], MIN(get_int(kf, group_name, active_keys[g], 0), opened - 1));
    g_strfreev(tabs);
    return opened;
}

gboolean session_restore_tabs(App *t)
{
    GKeyFile *kf = load();
    char *group = project_group(t);
    int opened;

    t->active = 0;
    opened = restore_group(t, kf, group, 0);
    if (g_key_file_has_key(kf, group, tab_keys[1], NULL)) {
        char **side = g_key_file_get_string_list(kf, group, tab_keys[1], NULL, NULL);
        if (side && side[0]) {
            set_split(t, TRUE);
            t->active = 1;
            if (restore_group(t, kf, group, 1) > 0) {
                int position = get_int(kf, "window", "split-position", 0);
                if (position > 0)
                    gtk_paned_set_position(t->split, position);
                opened++;
            } else {
                set_split(t, FALSE);
            }
        }
        g_strfreev(side);
    }
    t->active = 0;
    if (opened)
        focus_current_tab(t);
    g_free(group);
    g_key_file_free(kf);
    return opened > 0;
}

void session_save(App *t)
{
    GKeyFile *kf = load();
    char *group = project_group(t);
    gboolean maximized = gtk_window_is_maximized(t->window);

    if (!maximized) {
        g_key_file_set_integer(kf, "window", "width", gtk_widget_get_width(GTK_WIDGET(t->window)));
        g_key_file_set_integer(kf, "window", "height", gtk_widget_get_height(GTK_WIDGET(t->window)));
    }
    g_key_file_set_boolean(kf, "window", "maximized", maximized);
    g_key_file_set_integer(kf, "window", "files-width", gtk_paned_get_position(t->hpaned));
    g_key_file_set_integer(kf, "window", "editor-height", gtk_paned_get_position(t->vpaned));
    if (gtk_widget_get_visible(GTK_WIDGET(t->groups[1])))
        g_key_file_set_integer(kf, "window", "split-position", gtk_paned_get_position(t->split));
    g_key_file_set_boolean(kf, "window", "files-visible", gtk_widget_get_visible(t->sidebar));
    g_key_file_set_boolean(kf, "window", "shell-visible", gtk_widget_get_visible(GTK_WIDGET(t->shell)));
    g_key_file_set_string(kf, "window", "sidebar-page", gtk_stack_get_visible_child_name(t->sidebar_stack));
    g_key_file_set_double(kf, "window", "font-scale", t->font_scale);

    g_key_file_set_string(kf, group, "root", t->root);
    for (int g = 0; g < GROUPS; g++) {
        GtkNotebook *notebook = t->groups[g];
        int n = gtk_notebook_get_n_pages(notebook), kept = 0, active = 0;
        int current = gtk_notebook_get_current_page(notebook);
        char **tabs = g_new0(char *, n + 1);
        for (int i = 0; i < n; i++) {
            Tab *tab = g_object_get_data(G_OBJECT(gtk_notebook_get_nth_page(notebook, i)), "t-code-tab");
            if (i == current) /* counted among the kept tabs (an old version: the one before it) */
                active = MAX(kept - (tab->compare_with ? 1 : 0), 0);
            if (tab->compare_with) /* old versions in a comparison live in a temp folder */
                continue;
            tabs[kept++] = tab->kind == TAB_PREVIEW ? g_strconcat(PREVIEW_PREFIX, tab->path, NULL)
                                                    : g_strdup(tab->path);
        }
        g_key_file_set_string_list(kf, group, tab_keys[g], (const char *const *)tabs, kept);
        g_key_file_set_integer(kf, group, active_keys[g], active);
        g_strfreev(tabs);
    }

    store(kf);
    g_free(group);
    g_key_file_free(kf);
}

char **session_recent(void)
{
    GKeyFile *kf = load();
    char **recent = g_key_file_get_string_list(kf, "recent", "folders", NULL, NULL);

    g_key_file_free(kf);
    return recent;
}

/* Puts the current project at the top of the recent folders. */
void session_add_recent(App *t)
{
    GKeyFile *kf = load();
    char **old = g_key_file_get_string_list(kf, "recent", "folders", NULL, NULL);
    GPtrArray *list = g_ptr_array_new();

    g_ptr_array_add(list, t->root);
    for (int i = 0; old && old[i] && list->len < MAX_RECENT; i++)
        if (strcmp(old[i], t->root) != 0)
            g_ptr_array_add(list, old[i]);
    g_key_file_set_string_list(kf, "recent", "folders", (const char *const *)list->pdata, list->len);
    store(kf);

    g_ptr_array_free(list, TRUE);
    g_strfreev(old);
    g_key_file_free(kf);
}
