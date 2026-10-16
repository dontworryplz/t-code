/*
 * t-code: one window with three kinds of terminals.
 *
 *   +-------------+-------------------------------+
 *   |             | [main.c x] [util.c x]         |  editor tabs, one terminal (micro) each;
 *   |    files    |                               |  split into two groups side by side
 *   | (t-code-fm) +-------------------------------+
 *   |             | shell                         |
 *   +-------------+-------------------------------+
 *
 * The file manager talks to it over a unix socket whose path it gets in $TCODE_SOCKET, one line
 * per request: "open <path>", "open-side <path>" (in the second group), "preview <path>",
 * "project <dir>" (switch to that folder),
 * "copy <text>" (clipboard) and "terminal <dir>" (cd the shell there). Paths are absolute.
 *
 * The status bar at the bottom has Build and Run buttons, like VS Code's CMake Tools.
 *
 * Each editor reports its file and unsaved state through the terminal title, as
 * "t-code <0|1> <absolute path>" (see micro/plug/tcode).
 */
#include "app.h"

#include <gio/gunixsocketaddress.h>
#include <glib-unix.h>
#include <signal.h>
#include <unistd.h>

/* Terminals ---------------------------------------------------------------------------------- */

#define TERMINAL_FONT "Cascadia Code 11"

/* VS Code's terminal colors on its Dark Modern background. */
static const char *const palette_hex[16] = {
    "#000000", "#cd3131", "#0dbc79", "#e5e510", "#2472c8", "#bc3fbc", "#11a8cd", "#e5e5e5",
    "#666666", "#f14c4c", "#23d18b", "#f5f543", "#3b8eea", "#d670d6", "#29b8db", "#e5e5e5",
};

static VteTerminal *new_terminal(App *t)
{
    VteTerminal *term = VTE_TERMINAL(vte_terminal_new());
    PangoFontDescription *font = pango_font_description_from_string(TERMINAL_FONT);
    GdkRGBA fg, bg, palette[16];

    vte_terminal_set_font(term, font);
    vte_terminal_set_font_scale(term, t->font_scale);
    pango_font_description_free(font);

    gdk_rgba_parse(&fg, "#cccccc");
    gdk_rgba_parse(&bg, "#1f1f1f");
    for (int i = 0; i < 16; i++)
        gdk_rgba_parse(&palette[i], palette_hex[i]);
    vte_terminal_set_colors(term, &fg, &bg, palette, 16);
    vte_terminal_set_scrollback_lines(term, 10000);
    vte_terminal_set_mouse_autohide(term, TRUE);
    gtk_widget_set_hexpand(GTK_WIDGET(term), TRUE);
    gtk_widget_set_vexpand(GTK_WIDGET(term), TRUE);
    return term;
}

static void spawn_done(VteTerminal *term, GPid pid, GError *error, gpointer data)
{
    GPid *out = data;

    if (error) {
        char *msg = g_strdup_printf("\r\nt-code: %s\r\n", error->message);
        vte_terminal_feed(term, msg, -1);
        g_free(msg);
        return;
    }
    if (out)
        *out = pid;
}

/* Runs argv in term, in the project root, with $TCODE_SOCKET set; the child's pid goes to *pid. */
static void spawn(App *t, VteTerminal *term, char **argv, GPid *pid)
{
    char **env = g_get_environ();

    env = g_environ_setenv(env, "TCODE_SOCKET", t->socket_path, TRUE);
    vte_terminal_spawn_async(term, VTE_PTY_DEFAULT, t->root, argv, env, G_SPAWN_SEARCH_PATH,
                             NULL, NULL, NULL, -1, NULL, spawn_done, pid);
    g_strfreev(env);
}

static void start_fm(App *t)
{
    char *argv[] = { t->fm_program, NULL };

    spawn(t, t->fm, argv, &t->fm_pid);
}

/* $SHELL first, like most terminals; then the login shell from the user database. */
static void start_shell(App *t)
{
    const char *env = g_getenv("SHELL");
    char *shell = env && *env ? g_strdup(env) : vte_get_user_shell();
    char *argv[] = { shell ? shell : "/bin/sh", NULL };

    spawn(t, t->shell, argv, &t->shell_pid);
    g_free(shell);
}

/* The file manager and the shell come back when they exit (in the current project). */
static void on_fixed_exited(VteTerminal *term, int status G_GNUC_UNUSED, App *t)
{
    if (t->closing)
        return;
    vte_terminal_reset(term, TRUE, TRUE);
    if (term == t->fm)
        start_fm(t);
    else
        start_shell(t);
}

/* Types command into the shell pane and runs it. Ctrl+U first drops a half-typed line. */
void run_in_shell(App *t, const char *command)
{
    char *line = g_strdup_printf("\x15%s\r", command);

    set_pane_visible(t, GTK_WIDGET(t->shell), TRUE);
    vte_terminal_feed_child(t->shell, line, -1);
    gtk_widget_grab_focus(GTK_WIDGET(t->shell));
    g_free(line);
}

/* Tabs --------------------------------------------------------------------------------------- */

static void tab_free(Tab *tab)
{
    if (tab->preview)
        preview_free(tab->preview);
    g_free(tab->path);
    g_free(tab);
}

static Tab *tab_of(GtkWidget *page)
{
    return g_object_get_data(G_OBJECT(page), "t-code-tab");
}

int tab_count(App *t)
{
    return gtk_notebook_get_n_pages(t->groups[0]) + gtk_notebook_get_n_pages(t->groups[1]);
}

Tab *tab_nth(App *t, int index)
{
    int first = gtk_notebook_get_n_pages(t->groups[0]);

    return index < first ? tab_of(gtk_notebook_get_nth_page(t->groups[0], index))
                         : tab_of(gtk_notebook_get_nth_page(t->groups[1], index - first));
}

int tab_group(Tab *tab)
{
    return gtk_notebook_page_num(tab->app->groups[1], tab->page) >= 0 ? 1 : 0;
}

Tab *current_tab(App *t)
{
    for (int k = 0; k < GROUPS; k++) {
        GtkNotebook *group = t->groups[k == 0 ? t->active : 1 - t->active];
        int current = gtk_notebook_get_current_page(group);
        if (current >= 0)
            return tab_of(gtk_notebook_get_nth_page(group, current));
    }
    return NULL;
}

void focus_current_tab(App *t)
{
    Tab *tab = current_tab(t);

    gtk_widget_grab_focus(tab ? tab->page : GTK_WIDGET(t->fm));
}

void select_tab(Tab *tab)
{
    App *t = tab->app;
    int group = tab_group(tab);

    gtk_notebook_set_current_page(t->groups[group], gtk_notebook_page_num(t->groups[group], tab->page));
    t->active = group;
    gtk_widget_grab_focus(tab->page);
}

static void remove_tab(Tab *tab)
{
    App *t = tab->app;
    GtkNotebook *group = t->groups[tab_group(tab)];

    gtk_notebook_remove_page(group, gtk_notebook_page_num(group, tab->page));
    focus_current_tab(t);
}

/* Editors close through micro (Ctrl+Q), so nothing unsaved is dropped. */
static void close_tab(Tab *tab)
{
    if (tab->kind != TAB_EDITOR || tab->pid <= 0) {
        remove_tab(tab);
        return;
    }
    select_tab(tab);
    vte_terminal_feed_child(tab->term, "\x11", 1);
}

static void on_tab_middle_click(GtkGestureClick *gesture G_GNUC_UNUSED, int n_press G_GNUC_UNUSED,
                                double x G_GNUC_UNUSED, double y G_GNUC_UNUSED, Tab *tab)
{
    close_tab(tab);
}

void update_tab_label(Tab *tab)
{
    char *name = g_path_get_basename(tab->path);
    char *text = tab->kind == TAB_PREVIEW ? g_strdup_printf("Preview %s", name)
                                          : g_strdup_printf("%s%s", tab->modified ? "● " : "", name);

    gtk_label_set_text(tab->label, text);
    gtk_widget_set_tooltip_text(gtk_widget_get_parent(GTK_WIDGET(tab->label)), tab->path);
    g_free(text);
    g_free(name);
}

static GtkWidget *tab_label(Tab *tab)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *close = gtk_button_new_from_icon_name("window-close-symbolic");
    GtkGesture *middle = gtk_gesture_click_new();

    tab->label = GTK_LABEL(gtk_label_new(NULL));
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(tab->label));
    gtk_button_set_has_frame(GTK_BUTTON(close), FALSE);
    gtk_widget_set_focusable(close, FALSE);
    g_signal_connect_swapped(close, "clicked", G_CALLBACK(close_tab), tab);
    gtk_box_append(GTK_BOX(box), close);

    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(middle), GDK_BUTTON_MIDDLE);
    g_signal_connect(middle, "pressed", G_CALLBACK(on_tab_middle_click), tab);
    gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(middle));
    return box;
}

static void insert_page(GtkNotebook *group, GtkWidget *page, GtkWidget *label, int position)
{
    int index = gtk_notebook_insert_page(group, page, label, position);

    gtk_notebook_set_tab_reorderable(group, page, TRUE);
    gtk_notebook_set_tab_detachable(group, page, TRUE); /* drag between the two groups */
    gtk_notebook_set_current_page(group, index);
}

/* Adds page as a tab of the active group, right after its current tab, and switches to it. */
Tab *add_tab(App *t, TabKind kind, GtkWidget *page, const char *path)
{
    GtkNotebook *group = t->groups[t->active];
    Tab *tab = g_new0(Tab, 1);
    GtkWidget *label;

    tab->app = t;
    tab->kind = kind;
    tab->page = page;
    tab->path = g_strdup(path);
    g_object_set_data_full(G_OBJECT(page), "t-code-tab", tab, (GDestroyNotify)tab_free);
    label = tab_label(tab);
    update_tab_label(tab);
    insert_page(group, page, label, gtk_notebook_get_current_page(group) + 1);
    return tab;
}

/* Moves a tab (with its running editor) to the end of the other group. */
static void move_tab(Tab *tab, int to)
{
    App *t = tab->app;
    int from = tab_group(tab);
    GtkWidget *label = gtk_notebook_get_tab_label(t->groups[from], tab->page);

    if (from == to)
        return;
    g_object_ref(tab->page);
    g_object_ref(label);
    gtk_notebook_remove_page(t->groups[from], gtk_notebook_page_num(t->groups[from], tab->page));
    insert_page(t->groups[to], tab->page, label, -1);
    g_object_unref(label);
    g_object_unref(tab->page);
}

static void on_editor_exited(VteTerminal *term G_GNUC_UNUSED, int status G_GNUC_UNUSED, Tab *tab)
{
    if (!tab->app->closing)
        remove_tab(tab);
}

/* "t-code <0|1> <absolute path>" from the tcode micro plugin. */
static void on_editor_title(VteTerminal *term, const char *prop G_GNUC_UNUSED, Tab *tab)
{
    const char *title = vte_terminal_get_termprop_string(term, VTE_TERMPROP_XTERM_TITLE, NULL);

    if (!title || !g_str_has_prefix(title, "t-code ") || (title[7] != '0' && title[7] != '1'))
        return;
    tab->modified = title[7] == '1';
    if (title[8] == ' ' && g_path_is_absolute(title + 9) && g_strcmp0(tab->path, title + 9) != 0) {
        g_free(tab->path);
        tab->path = g_strdup(title + 9);
    }
    update_tab_label(tab);
}

static Tab *find_tab(App *t, TabKind kind, const char *path)
{
    int n = tab_count(t);

    for (int i = 0; i < n; i++) {
        Tab *tab = tab_nth(t, i);
        if (tab->kind == kind && g_strcmp0(tab->path, path) == 0)
            return tab;
    }
    return NULL;
}

void open_file(App *t, const char *path)
{
    Tab *existing = find_tab(t, TAB_EDITOR, path);

    if (existing) {
        select_tab(existing);
        return;
    }

    /* micro shows the path it was given in its status line: keep it short inside the project. */
    GFile *root = g_file_new_for_path(t->root), *file = g_file_new_for_path(path);
    char *relative = g_file_get_relative_path(root, file);
    char *argv[] = { "micro", "-config-dir", t->micro_config, "-syntax", t->plain ? "false" : "true",
                     relative ? relative : (char *)path, NULL };
    VteTerminal *term = new_terminal(t);
    Tab *tab = add_tab(t, TAB_EDITOR, GTK_WIDGET(term), path);

    tab->term = term;
    g_signal_connect(term, "child-exited", G_CALLBACK(on_editor_exited), tab);
    g_signal_connect(term, "termprop-changed::" VTE_TERMPROP_XTERM_TITLE, G_CALLBACK(on_editor_title), tab);
    spawn(t, term, argv, &tab->pid);
    g_free(relative);
    g_object_unref(file);
    g_object_unref(root);
    focus_current_tab(t);
}

/* Opens the file in the second group, splitting first. An already open file moves there, so the
 * same file is never edited by two editors at once. */
static void open_to_side(App *t, const char *path)
{
    Tab *existing = find_tab(t, TAB_EDITOR, path);

    set_split(t, TRUE);
    t->active = 1;
    if (existing) {
        move_tab(existing, 1);
        select_tab(existing);
    } else {
        open_file(t, path);
    }
}

/* Split editor ------------------------------------------------------------------------------- */

static void update_editor_area(App *t)
{
    gboolean split = gtk_widget_get_visible(GTK_WIDGET(t->groups[1]));

    gtk_stack_set_visible_child_name(t->editor_area, tab_count(t) > 0 || split ? "tabs" : "empty");
}

void set_split(App *t, gboolean split)
{
    GtkWidget *side = GTK_WIDGET(t->groups[1]);

    if (!split) {
        while (gtk_notebook_get_n_pages(t->groups[1]) > 0)
            move_tab(tab_of(gtk_notebook_get_nth_page(t->groups[1], 0)), 0);
        t->active = 0;
    }
    if (split != gtk_widget_get_visible(side)) {
        gtk_widget_set_visible(side, split);
        int width = gtk_widget_get_width(GTK_WIDGET(t->split));
        if (split && width > 0)
            gtk_paned_set_position(t->split, width / 2);
        if (split)
            t->active = 1;
    }
    if (gtk_toggle_button_get_active(t->split_button) != split)
        gtk_toggle_button_set_active(t->split_button, split);
    update_editor_area(t);
}

static void on_split_toggled(GtkToggleButton *button, App *t)
{
    set_split(t, gtk_toggle_button_get_active(button));
    focus_current_tab(t);
}

/* The second group goes away when its last tab closes. */
static gboolean unsplit_if_empty(gpointer data)
{
    App *t = data;

    if (!t->closing && gtk_widget_get_visible(GTK_WIDGET(t->groups[1])) &&
        gtk_notebook_get_n_pages(t->groups[1]) == 0)
        set_split(t, FALSE);
    return G_SOURCE_REMOVE;
}

static void on_tabs_changed(GtkNotebook *group, GtkWidget *child G_GNUC_UNUSED, guint index G_GNUC_UNUSED, App *t)
{
    if (t->closing)
        return;
    update_editor_area(t);
    if (group == t->groups[1] && gtk_notebook_get_n_pages(group) == 0)
        g_idle_add(unsplit_if_empty, t);
}

static void on_tab_switched(GtkNotebook *group, GtkWidget *page G_GNUC_UNUSED, guint index G_GNUC_UNUSED, App *t)
{
    t->active = group == t->groups[1];
    update_status(t);
}

static void on_group_focus(GtkEventControllerFocus *focus, App *t)
{
    GtkWidget *group = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(focus));

    t->active = group == GTK_WIDGET(t->groups[1]);
    update_status(t);
}

/* Status bar --------------------------------------------------------------------------------- */

static const char *const status_css =
    ".tcode-status { background: #181818; border-top: 1px solid #2b2b2b; padding: 0 6px; }"
    ".tcode-status button { min-height: 0; padding: 1px 8px; border-radius: 0; color: #cccccc; }"
    ".tcode-status button:hover { background: rgba(255, 255, 255, .12); }"
    ".tcode-status label.dim { color: #9d9d9d; padding: 0 8px; }";

void update_status(App *t)
{
    if (!t->build_label || t->closing)
        return;
    char *summary = project_summary(t);
    gtk_label_set_text(t->build_label, summary);
    g_free(summary);
}

static void status_button(GtkWidget *bar, const char *text, const char *tooltip, GCallback clicked, App *t)
{
    GtkWidget *button = gtk_button_new_with_label(text);

    gtk_button_set_has_frame(GTK_BUTTON(button), FALSE);
    gtk_widget_set_focusable(button, FALSE);
    gtk_widget_set_tooltip_text(button, tooltip);
    g_signal_connect_swapped(button, "clicked", clicked, t);
    gtk_box_append(GTK_BOX(bar), button);
}

static void on_build_clicked(App *t)
{
    build_project(t, FALSE);
}

static void on_run_clicked(App *t)
{
    build_project(t, TRUE);
}

static GtkWidget *status_bar(App *t)
{
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkCssProvider *provider = gtk_css_provider_new();

    gtk_css_provider_load_from_string(provider, status_css);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
    gtk_widget_add_css_class(bar, "tcode-status");

    status_button(bar, "⚙ Build", "Build (Ctrl+Shift+B)", G_CALLBACK(on_build_clicked), t);
    status_button(bar, "▶ Run", "Build and run (F5)", G_CALLBACK(on_run_clicked), t);
    t->build_label = GTK_LABEL(gtk_label_new(NULL));
    gtk_widget_add_css_class(GTK_WIDGET(t->build_label), "dim");
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(t->build_label));
    return bar;
}

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

static gboolean any_unsaved(App *t)
{
    int n = tab_count(t);

    for (int i = 0; i < n; i++)
        if (tab_nth(t, i)->modified)
            return TRUE;
    return FALSE;
}

/* Asks before throwing away unsaved changes; runs then(t, arg) on "yes" (or right away). */
typedef void (*ConfirmedFunc)(App *t, const char *arg);

typedef struct {
    App *app;
    ConfirmedFunc then;
    char *arg;
} Confirm;

static void on_confirm_answer(GObject *dialog, GAsyncResult *result, gpointer data)
{
    Confirm *c = data;

    if (gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(dialog), result, NULL) == 1)
        c->then(c->app, c->arg);
    g_free(c->arg);
    g_free(c);
}

static void unless_unsaved(App *t, const char *question, const char *button, ConfirmedFunc then, const char *arg)
{
    if (!any_unsaved(t)) {
        then(t, arg);
        return;
    }

    GtkAlertDialog *dialog = gtk_alert_dialog_new("%s", question);
    const char *buttons[] = { "Cancel", button, NULL };
    Confirm *c = g_new0(Confirm, 1);

    c->app = t;
    c->then = then;
    c->arg = g_strdup(arg);
    gtk_alert_dialog_set_detail(dialog, "Some files have changes that are not saved yet.");
    gtk_alert_dialog_set_buttons(dialog, buttons);
    gtk_alert_dialog_set_cancel_button(dialog, 0);
    gtk_alert_dialog_set_default_button(dialog, 1);
    gtk_alert_dialog_choose(dialog, t->window, NULL, on_confirm_answer, c);
    g_object_unref(dialog);
}

/* Projects ----------------------------------------------------------------------------------- */

static void update_title(App *t)
{
    char *title = g_strdup_printf("t-code - %s", t->root);

    gtk_window_set_title(t->window, title);
    g_free(title);
}

static char *home_relative(const char *path)
{
    const char *home = g_get_home_dir();
    size_t n = strlen(home);

    if (strncmp(path, home, n) == 0 && (path[n] == '/' || path[n] == '\0'))
        return g_strconcat("~", path + n, NULL);
    return g_strdup(path);
}

static void rebuild_recent_menu(App *t)
{
    char **recent = session_recent();
    int shown = 0;

    g_menu_remove_all(t->recent_menu);
    for (int i = 0; recent && recent[i]; i++) {
        if (strcmp(recent[i], t->root) == 0 || !g_file_test(recent[i], G_FILE_TEST_IS_DIR))
            continue;
        char *label = home_relative(recent[i]);
        GMenuItem *item = g_menu_item_new(label, NULL);
        g_menu_item_set_action_and_target_value(item, "win.open-recent", g_variant_new_string(recent[i]));
        g_menu_append_item(t->recent_menu, item);
        g_object_unref(item);
        g_free(label);
        shown++;
    }
    if (!shown)
        g_menu_append(t->recent_menu, "No recent folders", "win.none");
    g_strfreev(recent);
}

/* Leaves the current project for root: its tabs are remembered and closed, the file manager and
 * the shell restart there, and root's own tabs from last time come back. */
static void switch_project(App *t, const char *root)
{
    if (strcmp(root, t->root) == 0)
        return;
    session_save(t);

    t->closing = TRUE; /* quiet while the tabs go */
    for (int g = 0; g < GROUPS; g++)
        while (gtk_notebook_get_n_pages(t->groups[g]) > 0)
            gtk_notebook_remove_page(t->groups[g], 0);
    t->closing = FALSE;
    set_split(t, FALSE);

    g_free(t->root);
    t->root = g_strdup(root);
    update_title(t);
    session_add_recent(t);
    rebuild_recent_menu(t);

    /* They restart in the new root when they exit (on_fixed_exited). */
    if (t->fm_pid > 0)
        kill(t->fm_pid, SIGHUP);
    if (t->shell_pid > 0)
        kill(t->shell_pid, SIGHUP);

    session_restore_tabs(t);
    update_editor_area(t);
    update_status(t);
    if (!current_tab(t))
        gtk_widget_grab_focus(GTK_WIDGET(t->fm));
}

static void open_project(App *t, const char *root)
{
    unless_unsaved(t, "Open another folder?", "Open", switch_project, root);
}

static void on_folder_chosen(GObject *source, GAsyncResult *result, gpointer data)
{
    GFile *folder = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(source), result, NULL);

    if (!folder)
        return;
    char *path = g_file_get_path(folder);
    if (path)
        open_project(data, path);
    g_free(path);
    g_object_unref(folder);
}

static void choose_folder(App *t)
{
    GtkFileDialog *dialog = gtk_file_dialog_new();
    GFile *start = g_file_new_for_path(t->root);

    gtk_file_dialog_set_title(dialog, "Open Folder");
    gtk_file_dialog_set_initial_folder(dialog, start);
    gtk_file_dialog_select_folder(dialog, t->window, NULL, on_folder_chosen, t);
    g_object_unref(start);
    g_object_unref(dialog);
}

static void on_open_folder_action(GSimpleAction *action G_GNUC_UNUSED, GVariant *param G_GNUC_UNUSED, gpointer t)
{
    choose_folder(t);
}

static void on_open_recent_action(GSimpleAction *action G_GNUC_UNUSED, GVariant *param, gpointer t)
{
    open_project(t, g_variant_get_string(param, NULL));
}

/* Socket for the file manager --------------------------------------------------------------- */

static void cd_shell(App *t, const char *dir)
{
    char *quoted = g_shell_quote(dir);
    /* The leading space keeps the command out of bash history. */
    char *command = g_strdup_printf(" cd -- %s", quoted);

    run_in_shell(t, command);
    g_free(command);
    g_free(quoted);
}

static void on_line(GObject *source, GAsyncResult *result, gpointer data)
{
    GDataInputStream *in = G_DATA_INPUT_STREAM(source);
    App *t = data;
    char *line = g_data_input_stream_read_line_finish_utf8(in, result, NULL, NULL);
    const char *arg = line ? strchr(line, ' ') : NULL;

    if (!line) { /* closed, or not UTF-8 */
        g_object_unref(in);
        return;
    }
    if (arg) {
        arg++;
        if (g_str_has_prefix(line, "copy "))
            gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(t->window)), arg);
        else if (!g_path_is_absolute(arg))
            ;
        else if (g_str_has_prefix(line, "open "))
            open_file(t, arg);
        else if (g_str_has_prefix(line, "open-side "))
            open_to_side(t, arg);
        else if (g_str_has_prefix(line, "preview "))
            open_preview(t, arg);
        else if (g_str_has_prefix(line, "project ") && g_file_test(arg, G_FILE_TEST_IS_DIR))
            open_project(t, arg);
        else if (g_str_has_prefix(line, "terminal "))
            cd_shell(t, arg);
    }
    g_free(line);
    g_data_input_stream_read_line_async(in, G_PRIORITY_DEFAULT, NULL, on_line, t);
}

static gboolean on_incoming(GSocketService *service G_GNUC_UNUSED, GSocketConnection *conn,
                            GObject *source G_GNUC_UNUSED, App *t)
{
    GDataInputStream *in = g_data_input_stream_new(g_io_stream_get_input_stream(G_IO_STREAM(conn)));

    g_object_set_data_full(G_OBJECT(in), "connection", g_object_ref(conn), g_object_unref);
    g_data_input_stream_read_line_async(in, G_PRIORITY_DEFAULT, NULL, on_line, t);
    return TRUE;
}

/* Files t-code needs, looked up next to the executable: the installed layout
 * (bin/t-code, libexec/t-code/t-code-fm, share/t-code/micro) or the build tree
 * (build/t-code, build/t-code-fm, micro). */
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

    t->fm_program = find_beside_exe(fm);
    t->micro_config = find_beside_exe(micro);
}

static void start_socket(App *t)
{
    GSocketAddress *address;
    GError *error = NULL;

    t->socket_path = g_strdup_printf("%s/t-code-%d.sock", g_get_user_runtime_dir(), getpid());
    unlink(t->socket_path);
    address = g_unix_socket_address_new(t->socket_path);
    t->service = g_socket_service_new();
    if (!g_socket_listener_add_address(G_SOCKET_LISTENER(t->service), address, G_SOCKET_TYPE_STREAM,
                                       G_SOCKET_PROTOCOL_DEFAULT, NULL, NULL, &error)) {
        g_warning("t-code: cannot listen on %s: %s", t->socket_path, error->message);
        g_clear_error(&error);
    }
    g_object_unref(address);
    g_signal_connect(t->service, "incoming", G_CALLBACK(on_incoming), t);
    g_socket_service_start(t->service);
}

/* Shortcuts ---------------------------------------------------------------------------------- */

#define SHORTCUT(name) static gboolean name(GtkWidget *w G_GNUC_UNUSED, GVariant *a G_GNUC_UNUSED, gpointer data)

static void step_tab(App *t, int step)
{
    Tab *tab = current_tab(t);

    if (!tab)
        return;
    GtkNotebook *group = t->groups[tab_group(tab)];
    int n = gtk_notebook_get_n_pages(group);
    gtk_notebook_set_current_page(group, (gtk_notebook_get_current_page(group) + step + n) % n);
    focus_current_tab(t);
}

SHORTCUT(next_tab)
{
    step_tab(data, 1);
    return TRUE;
}

SHORTCUT(prev_tab)
{
    step_tab(data, -1);
    return TRUE;
}

/* Turns code coloring off or on in every open editor; new ones start the same way. */
SHORTCUT(toggle_syntax)
{
    App *t = data;
    int n = tab_count(t);

    t->plain = !t->plain;
    for (int i = 0; i < n; i++) {
        Tab *tab = tab_nth(t, i);
        if (tab->kind == TAB_EDITOR) /* Ctrl+E opens micro's command bar */
            vte_terminal_feed_child(tab->term,
                                    t->plain ? "\x05setlocal syntax off\r" : "\x05setlocal syntax on\r", -1);
    }
    return TRUE;
}

SHORTCUT(run)
{
    build_project(data, TRUE);
    return TRUE;
}

SHORTCUT(build)
{
    build_project(data, FALSE);
    return TRUE;
}

SHORTCUT(zoom_in)
{
    App *t = data;
    set_font_scale(t, t->font_scale * 1.1);
    return TRUE;
}

SHORTCUT(zoom_out)
{
    App *t = data;
    set_font_scale(t, t->font_scale / 1.1);
    return TRUE;
}

SHORTCUT(zoom_reset)
{
    set_font_scale(data, 1.0);
    return TRUE;
}

SHORTCUT(toggle_files)
{
    App *t = data;
    gboolean show = !gtk_widget_get_visible(GTK_WIDGET(t->fm));

    set_pane_visible(t, GTK_WIDGET(t->fm), show);
    if (show)
        gtk_widget_grab_focus(GTK_WIDGET(t->fm));
    return TRUE;
}

SHORTCUT(toggle_shell)
{
    App *t = data;
    gboolean show = !gtk_widget_get_visible(GTK_WIDGET(t->shell));

    set_pane_visible(t, GTK_WIDGET(t->shell), show);
    if (show)
        gtk_widget_grab_focus(GTK_WIDGET(t->shell));
    return TRUE;
}

SHORTCUT(toggle_split)
{
    App *t = data;

    set_split(t, !gtk_widget_get_visible(GTK_WIDGET(t->groups[1])));
    focus_current_tab(t);
    return TRUE;
}

static void focus_group(App *t, int group)
{
    if (group == 1 && !gtk_widget_get_visible(GTK_WIDGET(t->groups[1])))
        return;
    t->active = group;
    focus_current_tab(t);
}

SHORTCUT(focus_left)
{
    focus_group(data, 0);
    return TRUE;
}

SHORTCUT(focus_right)
{
    focus_group(data, 1);
    return TRUE;
}

SHORTCUT(open_folder)
{
    choose_folder(data);
    return TRUE;
}

SHORTCUT(quick_open)
{
    quick_open_show(data);
    return TRUE;
}

/* Opens the markdown preview of the file in the current editor tab. */
SHORTCUT(preview)
{
    Tab *tab = current_tab(data);

    if (tab && tab->kind == TAB_EDITOR && is_markdown(tab->path))
        open_preview(data, tab->path);
    return TRUE;
}

SHORTCUT(focus_fm)
{
    App *t = data;
    set_pane_visible(t, GTK_WIDGET(t->fm), TRUE);
    gtk_widget_grab_focus(GTK_WIDGET(t->fm));
    return TRUE;
}

SHORTCUT(focus_editor)
{
    focus_current_tab(data);
    return TRUE;
}

SHORTCUT(focus_shell)
{
    App *t = data;
    set_pane_visible(t, GTK_WIDGET(t->shell), TRUE);
    gtk_widget_grab_focus(GTK_WIDGET(t->shell));
    return TRUE;
}

static void add_shortcuts(App *t)
{
    static const struct {
        const char *trigger;
        GtkShortcutFunc func;
    } keys[] = {
        { "<Control>Page_Down|<Alt>period", next_tab },
        { "<Control>Page_Up|<Alt>comma", prev_tab },
        { "<Alt>1", focus_fm },
        { "<Alt>2", focus_editor },
        { "<Alt>3", focus_shell },
        { "<Control>1", focus_left },
        { "<Control>2", focus_right },
        { "<Control>backslash", toggle_split },
        { "<Control><Shift>o", open_folder },
        { "F5", run },
        { "<Control><Shift>b", build },
        { "F7", toggle_syntax },
        { "<Control>equal|<Control>plus|<Control>KP_Add", zoom_in },
        { "<Control>minus|<Control>KP_Subtract", zoom_out },
        { "<Control>0|<Control>KP_0", zoom_reset },
        { "<Control>b", toggle_files },
        { "<Control>j", toggle_shell },
        { "<Control>p", quick_open },
        { "<Control><Shift>v", preview },
    };
    GtkEventController *controller = gtk_shortcut_controller_new();

    /* Capture phase: the window sees these keys before the focused terminal does. */
    gtk_event_controller_set_propagation_phase(controller, GTK_PHASE_CAPTURE);
    for (size_t i = 0; i < G_N_ELEMENTS(keys); i++)
        gtk_shortcut_controller_add_shortcut(
            GTK_SHORTCUT_CONTROLLER(controller),
            gtk_shortcut_new(gtk_shortcut_trigger_parse_string(keys[i].trigger),
                             gtk_callback_action_new(keys[i].func, t, NULL)));
    gtk_widget_add_controller(GTK_WIDGET(t->window), controller);
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

static GtkWidget *header_bar(App *t)
{
    GtkWidget *header = gtk_header_bar_new();
    GtkWidget *open = gtk_button_new_from_icon_name("folder-open-symbolic");
    GtkWidget *recent = gtk_menu_button_new();
    GtkWidget *split = gtk_toggle_button_new();

    gtk_widget_set_tooltip_text(open, "Open Folder… (Ctrl+Shift+O)");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(open), "win.open-folder");
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), open);

    t->recent_menu = g_menu_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(recent), "document-open-recent-symbolic");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(recent), G_MENU_MODEL(t->recent_menu));
    gtk_widget_set_tooltip_text(recent, "Recent Folders");
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), recent);

    gtk_button_set_icon_name(GTK_BUTTON(split), "view-dual-symbolic");
    gtk_widget_set_tooltip_text(split, "Split Editor (Ctrl+\\)");
    t->split_button = GTK_TOGGLE_BUTTON(split);
    g_signal_connect(split, "toggled", G_CALLBACK(on_split_toggled), t);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), split);
    return header;
}

static GtkNotebook *new_group(App *t)
{
    GtkNotebook *group = GTK_NOTEBOOK(gtk_notebook_new());
    GtkEventController *focus = gtk_event_controller_focus_new();

    gtk_notebook_set_scrollable(group, TRUE);
    gtk_notebook_set_group_name(group, "t-code-editors");
    gtk_widget_set_hexpand(GTK_WIDGET(group), TRUE);
    g_signal_connect(group, "page-added", G_CALLBACK(on_tabs_changed), t);
    g_signal_connect(group, "page-removed", G_CALLBACK(on_tabs_changed), t);
    g_signal_connect(group, "switch-page", G_CALLBACK(on_tab_switched), t);
    g_signal_connect(focus, "enter", G_CALLBACK(on_group_focus), t);
    gtk_widget_add_controller(GTK_WIDGET(group), focus);
    return group;
}

static void build_window(App *t, GtkApplication *app)
{
    static const GActionEntry actions[] = {
        { .name = "open-folder", .activate = on_open_folder_action },
        { .name = "open-recent", .activate = on_open_recent_action, .parameter_type = "s" },
    };
    GtkWidget *overlay = gtk_overlay_new();
    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *hint = gtk_label_new("Open a file from the file manager on the left, or press Ctrl+P");

    g_object_set(gtk_settings_get_default(), "gtk-application-prefer-dark-theme", TRUE, NULL);
    find_files(t);
    start_socket(t);
    gtk_window_set_default_icon_name("utilities-terminal");

    t->window = GTK_WINDOW(gtk_application_window_new(app));
    gtk_window_set_default_size(t->window, 1400, 900);
    gtk_window_set_titlebar(t->window, header_bar(t));
    g_action_map_add_action_entries(G_ACTION_MAP(t->window), actions, G_N_ELEMENTS(actions), t);
    g_signal_connect(t->window, "close-request", G_CALLBACK(on_close_request), t);
    update_title(t);

    t->fm = new_terminal(t);
    t->shell = new_terminal(t);
    g_signal_connect(t->fm, "child-exited", G_CALLBACK(on_fixed_exited), t);
    g_signal_connect(t->shell, "child-exited", G_CALLBACK(on_fixed_exited), t);

    for (int g = 0; g < GROUPS; g++)
        t->groups[g] = new_group(t);
    t->split = GTK_PANED(gtk_paned_new(GTK_ORIENTATION_HORIZONTAL));
    gtk_paned_set_start_child(t->split, GTK_WIDGET(t->groups[0]));
    gtk_paned_set_end_child(t->split, GTK_WIDGET(t->groups[1]));
    gtk_widget_set_visible(GTK_WIDGET(t->groups[1]), FALSE);

    t->editor_area = GTK_STACK(gtk_stack_new());
    gtk_widget_add_css_class(hint, "dim-label");
    gtk_stack_add_named(t->editor_area, hint, "empty");
    gtk_stack_add_named(t->editor_area, GTK_WIDGET(t->split), "tabs");
    gtk_stack_set_visible_child_name(t->editor_area, "empty");

    t->vpaned = GTK_PANED(gtk_paned_new(GTK_ORIENTATION_VERTICAL));
    gtk_paned_set_start_child(t->vpaned, GTK_WIDGET(t->editor_area));
    gtk_paned_set_end_child(t->vpaned, GTK_WIDGET(t->shell));
    gtk_paned_set_resize_end_child(t->vpaned, FALSE);
    gtk_paned_set_position(t->vpaned, 620);

    t->hpaned = GTK_PANED(gtk_paned_new(GTK_ORIENTATION_HORIZONTAL));
    gtk_paned_set_start_child(t->hpaned, GTK_WIDGET(t->fm));
    gtk_paned_set_end_child(t->hpaned, GTK_WIDGET(t->vpaned));
    gtk_paned_set_resize_start_child(t->hpaned, FALSE);
    gtk_paned_set_shrink_start_child(t->hpaned, FALSE);
    gtk_paned_set_position(t->hpaned, 280);

    gtk_widget_set_vexpand(GTK_WIDGET(t->hpaned), TRUE);
    gtk_box_append(GTK_BOX(content), GTK_WIDGET(t->hpaned));
    gtk_box_append(GTK_BOX(content), status_bar(t));
    gtk_overlay_set_child(GTK_OVERLAY(overlay), content);
    quick_open_init(t, GTK_OVERLAY(overlay));
    gtk_window_set_child(t->window, overlay);
    add_shortcuts(t);
    session_apply_window(t);
    session_add_recent(t);
    rebuild_recent_menu(t);
    start_fm(t);
    start_shell(t);
    update_status(t);
    gtk_window_present(t->window);
    gtk_widget_grab_focus(GTK_WIDGET(t->fm));
}

/* Application -------------------------------------------------------------------------------- */

static void on_activate(GApplication *app, App *t)
{
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
    if (t->service)
        g_socket_service_stop(t->service);
    if (t->socket_path)
        unlink(t->socket_path);
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
    g_free(t.root);
    return status;
}
