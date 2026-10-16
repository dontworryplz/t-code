/*
 * t-code: one window with three kinds of terminals.
 *
 *   +-------------+-------------------------------+
 *   |             | [main.c x] [util.c x]         |  editor tabs, one terminal (micro) each;
 *   |    files    |                               |  split into two groups side by side
 *   | (t-code-fm) +-------------------------------+
 *   |             | shell                         |
 *   +-------------+-------------------------------+
 *   | branch  problems  Build  Run      Ln, Col  Keybinds |  status bar
 *
 * This file builds the window and runs the application; the parts live in their own files
 * (see app.h for the map).
 */
#include "app.h"

#include <glib-unix.h>
#include <string.h>
#include <unistd.h>

/* View --------------------------------------------------------------------------------------- */

void set_font_scale(App *t, double scale)
{
    int n = tab_count(t);

    t->font_scale = CLAMP(scale, 0.5, 3.0);
    vte_terminal_set_font_scale(t->fm, t->font_scale);
    vte_terminal_set_font_scale(t->shell, t->font_scale);
    for (int i = 0; i < n; i++) {
        Tab *tab = tab_nth(t, i);
        if (tab->kind == TAB_EDITOR)
            vte_terminal_set_font_scale(tab->term, t->font_scale);
        else
            preview_set_zoom(tab, t->font_scale);
    }
}

void set_pane_visible(App *t, GtkWidget *pane, gboolean visible)
{
    if (!visible && gtk_widget_has_focus(pane))
        focus_current_tab(t);
    gtk_widget_set_visible(pane, visible);
}

/* Shows the sidebar on one of its pages ("files" or "scm") and gives that page the keyboard. */
void show_sidebar_page(App *t, const char *page)
{
    set_pane_visible(t, t->sidebar, TRUE);
    gtk_stack_set_visible_child_name(t->sidebar_stack, page);
    gtk_widget_grab_focus(strcmp(page, "files") == 0 ? GTK_WIDGET(t->fm) : t->scm);
}

void toggle_pane(App *t, GtkWidget *pane)
{
    gboolean show = !gtk_widget_get_visible(pane);

    set_pane_visible(t, pane, show);
    if (show)
        gtk_widget_grab_focus(pane);
}

/* Files t-code needs ------------------------------------------------------------------------- */

/* Looked up next to the executable: the installed layout (bin/t-code, libexec/t-code/t-code-fm,
 * share/t-code/micro) or the build tree (build/t-code, build/t-code-fm, micro). */
static char *find_beside_exe(const char *const *candidates)
{
    char *exe = g_file_read_link("/proc/self/exe", NULL);
    char *dir = exe ? g_path_get_dirname(exe) : g_get_current_dir();
    char *found = NULL;

    for (int i = 0; candidates[i] && !found; i++) {
        char *path = g_build_filename(dir, candidates[i], NULL);
        char *clean = g_canonicalize_filename(path, NULL);
        if (g_file_test(clean, G_FILE_TEST_EXISTS))
            found = g_strdup(clean);
        g_free(clean);
        g_free(path);
    }
    if (!found) /* spawning fails and says so inside the pane */
        found = g_build_filename(dir, candidates[0], NULL);
    g_free(dir);
    g_free(exe);
    return found;
}

static void find_files(App *t)
{
    static const char *const fm[] = { "t-code-fm", "../libexec/t-code/t-code-fm", NULL };
    static const char *const micro[] = { "../share/t-code/micro", "../micro", NULL };
    char *versions = g_strdup_printf("t-code-%d-versions", getpid());

    t->fm_program = find_beside_exe(fm);
    t->micro_config = find_beside_exe(micro);
    t->temp_dir = g_build_filename(g_get_user_runtime_dir(), versions, NULL);
    g_free(versions);
}

/* Window ------------------------------------------------------------------------------------- */

static void close_window(App *t, const char *arg G_GNUC_UNUSED)
{
    session_save(t);
    t->closing = TRUE;
    gtk_window_destroy(t->window);
}

static gboolean on_close_request(GtkWindow *window G_GNUC_UNUSED, App *t)
{
    if (t->closing)
        return FALSE;
    unless_unsaved(t, "Close t-code?", "Close", close_window, NULL);
    return TRUE;
}

static void on_split_toggled(GtkToggleButton *button, App *t)
{
    set_split(t, gtk_toggle_button_get_active(button));
    focus_current_tab(t);
}

/* The left side: the file manager and the Source Control panel, as tabs. */
static GtkWidget *sidebar(App *t)
{
    static const char *const css =
        ".tcode-sidebar { background: #1f1f1f; }"
        ".tcode-sidebar stackswitcher { background: #181818; border-bottom: 1px solid #2b2b2b; }"
        ".tcode-sidebar stackswitcher button { min-height: 0; padding: 4px 10px; border-radius: 0;"
        "  background: none; box-shadow: none; color: #9d9d9d; font-size: 0.8em; font-weight: bold;"
        "  border-bottom: 1px solid transparent; }"
        ".tcode-sidebar stackswitcher button:checked { color: #e7e7e7; border-bottom-color: #0078d4; }"
        ".tcode-sidebar stackswitcher button:hover { color: #e7e7e7; }";
    GtkCssProvider *provider = gtk_css_provider_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *switcher = gtk_stack_switcher_new();

    gtk_css_provider_load_from_string(provider, css);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);

    t->scm = scm_panel(t);
    t->sidebar_stack = GTK_STACK(gtk_stack_new());
    gtk_stack_add_titled(t->sidebar_stack, GTK_WIDGET(t->fm), "files", "FILES");
    gtk_stack_add_titled(t->sidebar_stack, t->scm, "scm", "SOURCE CONTROL");
    gtk_widget_set_vexpand(GTK_WIDGET(t->sidebar_stack), TRUE);
    gtk_stack_switcher_set_stack(GTK_STACK_SWITCHER(switcher), t->sidebar_stack);
    gtk_widget_set_focusable(switcher, FALSE);

    gtk_widget_add_css_class(box, "tcode-sidebar");
    gtk_box_append(GTK_BOX(box), switcher);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(t->sidebar_stack));
    t->sidebar = box;
    return box;
}

/* Open Folder and Recent Folders on the left, Settings and Split Editor on the right. */
static GtkWidget *header_bar(App *t)
{
    GtkWidget *header = gtk_header_bar_new();
    GtkWidget *open = gtk_button_new_from_icon_name("folder-open-symbolic");
    GtkWidget *recent = gtk_menu_button_new();
    GtkWidget *split = gtk_toggle_button_new();
    GtkWidget *settings = gtk_button_new_from_icon_name("emblem-system-symbolic");

    gtk_widget_set_tooltip_text(open, "Open Folder… (Ctrl+Shift+O)");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(open), "win.open-folder");
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), open);

    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(recent), "document-open-recent-symbolic");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(recent), G_MENU_MODEL(t->recent_menu));
    gtk_widget_set_tooltip_text(recent, "Recent Folders");
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), recent);

    gtk_button_set_icon_name(GTK_BUTTON(split), "view-dual-symbolic");
    gtk_widget_set_tooltip_text(split, "Split Editor (Ctrl+\\)");
    t->split_button = GTK_TOGGLE_BUTTON(split);
    g_signal_connect(split, "toggled", G_CALLBACK(on_split_toggled), t);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), split);

    gtk_widget_set_tooltip_text(settings, "Settings (Ctrl+,)");
    g_signal_connect_swapped(settings, "clicked", G_CALLBACK(show_settings), t);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), settings);
    return header;
}

static void build_window(App *t, GtkApplication *app)
{
    GtkWidget *overlay = gtk_overlay_new();
    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    g_object_set(gtk_settings_get_default(), "gtk-application-prefer-dark-theme", TRUE, NULL);
    find_files(t);
    start_socket(t);
    gtk_window_set_default_icon_name("utilities-terminal");

    t->window = GTK_WINDOW(gtk_application_window_new(app));
    gtk_window_set_default_size(t->window, 1400, 900);
    add_project_actions(t);
    gtk_window_set_titlebar(t->window, header_bar(t));
    g_signal_connect(t->window, "close-request", G_CALLBACK(on_close_request), t);
    update_title(t);

    create_fixed_terminals(t);
    create_groups(t);

    t->vpaned = GTK_PANED(gtk_paned_new(GTK_ORIENTATION_VERTICAL));
    gtk_paned_set_start_child(t->vpaned, GTK_WIDGET(t->editor_area));
    gtk_paned_set_end_child(t->vpaned, GTK_WIDGET(t->shell));
    gtk_paned_set_resize_end_child(t->vpaned, FALSE);
    gtk_paned_set_position(t->vpaned, 620);

    t->hpaned = GTK_PANED(gtk_paned_new(GTK_ORIENTATION_HORIZONTAL));
    gtk_paned_set_start_child(t->hpaned, sidebar(t));
    gtk_paned_set_end_child(t->hpaned, GTK_WIDGET(t->vpaned));
    gtk_paned_set_resize_start_child(t->hpaned, FALSE);
    gtk_paned_set_shrink_start_child(t->hpaned, FALSE);
    gtk_paned_set_position(t->hpaned, 280);
    gtk_widget_set_vexpand(GTK_WIDGET(t->hpaned), TRUE);

    gtk_box_append(GTK_BOX(content), GTK_WIDGET(t->hpaned));
    gtk_box_append(GTK_BOX(content), status_bar(t));
    gtk_overlay_set_child(GTK_OVERLAY(overlay), content);
    picker_init(t, GTK_OVERLAY(overlay));
    gtk_window_set_child(t->window, overlay);

    add_shortcuts(t);
    session_apply_window(t);
    session_add_recent(t);
    rebuild_recent_menu(t);
    start_fixed_terminals(t);
    start_status_updates(t);
    gtk_window_present(t->window);
    gtk_widget_grab_focus(GTK_WIDGET(t->fm));
}

/* Application -------------------------------------------------------------------------------- */

/* Started with no folder from the home directory (the app menu does that): reopen the last project
 * instead. Started elsewhere, the current directory is meant. */
static void reopen_last_project(App *t)
{
    char **recent = session_recent();

    if (t->settings.reopen_last && strcmp(t->root, g_get_home_dir()) == 0 && recent && recent[0] &&
        g_file_test(recent[0], G_FILE_TEST_IS_DIR)) {
        g_free(t->root);
        t->root = g_strdup(recent[0]);
    }
    g_strfreev(recent);
}

static void on_activate(GApplication *app, App *t)
{
    reopen_last_project(t);
    build_window(t, GTK_APPLICATION(app));
    session_restore_tabs(t);
}

/* t-code [dir] [file...]: a directory becomes the project root, files open as tabs.
 * With no files, the tabs from the last session in that directory come back. */
static void on_open(GApplication *app, GFile **files, int n, const char *hint G_GNUC_UNUSED, App *t)
{
    gboolean any_file = FALSE;

    for (int i = 0; i < n; i++) {
        if (g_file_query_file_type(files[i], G_FILE_QUERY_INFO_NONE, NULL) == G_FILE_TYPE_DIRECTORY) {
            g_free(t->root);
            t->root = g_file_get_path(files[i]);
        }
    }
    build_window(t, GTK_APPLICATION(app));
    for (int i = 0; i < n; i++) {
        if (g_file_query_file_type(files[i], G_FILE_QUERY_INFO_NONE, NULL) != G_FILE_TYPE_DIRECTORY) {
            char *path = g_file_get_path(files[i]);
            open_file(t, path);
            g_free(path);
            any_file = TRUE;
        }
    }
    if (!any_file)
        session_restore_tabs(t);
}

static void on_shutdown(GApplication *app G_GNUC_UNUSED, App *t)
{
    t->closing = TRUE;
    stop_socket(t);
    remove_temp_dir(t);
}

/* SIGTERM / SIGINT: save the session and quit normally, so the socket file is removed. */
static gboolean on_signal(gpointer data)
{
    App *t = data;

    if (t->window && !t->closing)
        session_save(t);
    t->closing = TRUE;
    g_application_quit(g_application_get_default());
    return G_SOURCE_REMOVE;
}

int main(int argc, char **argv)
{
    App t = { .root = g_get_current_dir(), .font_scale = 1.0 };
    GtkApplication *app = gtk_application_new("io.github.tcode",
                                              G_APPLICATION_HANDLES_OPEN | G_APPLICATION_NON_UNIQUE);
    int status;

    session_load_settings(&t);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), &t);
    g_signal_connect(app, "open", G_CALLBACK(on_open), &t);
    g_signal_connect(app, "shutdown", G_CALLBACK(on_shutdown), &t);
    g_unix_signal_add(SIGTERM, on_signal, &t);
    g_unix_signal_add(SIGINT, on_signal, &t);
    status = g_application_run(G_APPLICATION(app), argc, argv);

    g_object_unref(app);
    g_clear_object(&t.service);
    g_free(t.socket_path);
    g_free(t.fm_program);
    g_free(t.micro_config);
    g_free(t.temp_dir);
    g_free(t.git_top);
    g_free(t.root);
    return status;
}
