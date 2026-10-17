/*
 * Editor tabs, in two groups side by side (the second one only while split). Each editor is a
 * terminal running micro; micro reports its state through the terminal title, as
 * "t-code <modified> <line> <column> <errors> <warnings> <top> <height> <lines> <tabsize> <startcol>
 * <visualcol> <path>"
 * (see micro/plug/tcode), and takes commands through a named pipe (see editorview.c).
 */
#define _DEFAULT_SOURCE /* kill() under -std=c23 with musl */
#include "app.h"

#include <signal.h>
#include <string.h>

static void tab_free(Tab *tab)
{
    if (tab->start_id)
        g_source_remove(tab->start_id);
    if (tab->pid > 0) {
        kill(-tab->pid, SIGTERM);
        kill(tab->pid, SIGTERM);
    }
    if (tab->preview)
        preview_free(tab->preview);
    editor_view_free(tab->view);
    g_strfreev(tab->argv);
    g_strfreev(tab->env);
    g_free(tab->compare_with);
    g_free(tab->title);
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

Tab *find_tab(App *t, TabKind kind, const char *path)
{
    int n = tab_count(t);

    for (int i = 0; i < n; i++) {
        Tab *tab = tab_nth(t, i);
        if (tab->kind == kind && g_strcmp0(tab->path, path) == 0)
            return tab;
    }
    return NULL;
}

gboolean any_unsaved(App *t)
{
    int n = tab_count(t);

    for (int i = 0; i < n; i++)
        if (tab_nth(t, i)->modified)
            return TRUE;
    return FALSE;
}

/* The widget that takes the keyboard in a tab: an editor's terminal, not the box around it. */
static GtkWidget *focus_widget(Tab *tab)
{
    return tab->term ? GTK_WIDGET(tab->term) : tab->page;
}

void focus_current_tab(App *t)
{
    Tab *tab = current_tab(t);

    gtk_widget_grab_focus(tab ? focus_widget(tab) : GTK_WIDGET(t->fm));
}

void focus_group(App *t, int group)
{
    if (group == 1 && !gtk_widget_get_visible(GTK_WIDGET(t->groups[1])))
        return;
    t->active = group;
    focus_current_tab(t);
}

void select_tab(Tab *tab)
{
    App *t = tab->app;
    int group = tab_group(tab);

    gtk_notebook_set_current_page(t->groups[group], gtk_notebook_page_num(t->groups[group], tab->page));
    t->active = group;
    gtk_widget_grab_focus(focus_widget(tab));
}

void step_tab(App *t, int step)
{
    Tab *tab = current_tab(t);

    if (!tab)
        return;
    GtkNotebook *group = t->groups[tab_group(tab)];
    int n = gtk_notebook_get_n_pages(group);
    gtk_notebook_set_current_page(group, (gtk_notebook_get_current_page(group) + step + n) % n);
    focus_current_tab(t);
}

static void remove_tab(Tab *tab)
{
    App *t = tab->app;
    GtkNotebook *group = t->groups[tab_group(tab)];

    gtk_notebook_remove_page(group, gtk_notebook_page_num(group, tab->page));
    focus_current_tab(t);
}

/* Asks micro to quit, which asks about unsaved changes first; the tab goes when micro exits.
 * Typed Ctrl+Q only without the control pipe: in a prompt (find, command bar) a typed key
 * goes to the prompt. */
void quit_editor(Tab *tab)
{
    if (tab->argv) { /* never shown: no micro to ask */
        remove_tab(tab);
        return;
    }
    if (!editor_control(tab, "command quit"))
        vte_terminal_feed_child(tab->term, "\x11", 1);
}

/* Editors close through micro, so nothing unsaved is dropped. */
static void close_tab(Tab *tab)
{
    if (tab->kind != TAB_EDITOR || tab->pid <= 0) {
        remove_tab(tab);
        return;
    }
    select_tab(tab);
    quit_editor(tab);
}

void close_all_tabs(App *t)
{
    t->closing = TRUE; /* quiet while the tabs go */
    for (int g = 0; g < GROUPS; g++)
        while (gtk_notebook_get_n_pages(t->groups[g]) > 0)
            gtk_notebook_remove_page(t->groups[g], 0);
    t->closing = FALSE;
}

/* Labels ------------------------------------------------------------------------------------- */

void update_tab_label(Tab *tab)
{
    char *name = tab->title ? g_strdup(tab->title) : g_path_get_basename(tab->path);
    char *text = tab->kind == TAB_PREVIEW ? g_strdup_printf("Preview %s", name)
                                          : g_strdup_printf("%s%s", tab->modified ? "● " : "", name);

    gtk_label_set_text(tab->label, text);
    gtk_widget_set_tooltip_text(gtk_widget_get_parent(GTK_WIDGET(tab->label)), tab->path);
    g_free(text);
    g_free(name);
}

static void on_tab_middle_click(GtkGestureClick *gesture G_GNUC_UNUSED, int n_press G_GNUC_UNUSED,
                                double x G_GNUC_UNUSED, double y G_GNUC_UNUSED, Tab *tab)
{
    close_tab(tab);
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

/* Adding and moving ----------------------------------------------------------------------------- */

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
void move_tab(Tab *tab, int to)
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

/* Editors ------------------------------------------------------------------------------------ */

static void on_editor_exited(VteTerminal *term G_GNUC_UNUSED, int status G_GNUC_UNUSED, Tab *tab)
{
    tab->pid = 0;
    if (tab->app->closing)
        return;
    if (tab->compare_with)
        forget_comparison(tab);
    remove_tab(tab);
}

static void on_editor_title(VteTerminal *term, const char *prop G_GNUC_UNUSED, Tab *tab)
{
    const char *title = vte_terminal_get_termprop_string(term, VTE_TERMPROP_XTERM_TITLE, NULL);
    int modified, line, column, errors, warnings, top, height, lines, tab_size, start_col, visual_col, end = 0;

    if (!title)
        return;
    if (g_str_has_prefix(title, "t-code-open ")) {
        /* "t-code-open <n> <line> <column> <path>": n counts up, so that jumping to the same place
         * twice changes the title again (the signal only comes for a new title). */
        if (sscanf(title + 12, "%*u %d %d %n", &line, &column, &end) >= 2 &&
            g_path_is_absolute(title + 12 + end))
            open_file_at(tab->app, title + 12 + end, line, column);
        return;
    }
    if (!g_str_has_prefix(title, "t-code ") ||
        sscanf(title + 7, "%d %d %d %d %d %d %d %d %d %d %d %n", &modified, &line, &column, &errors, &warnings,
               &top, &height, &lines, &tab_size, &start_col, &visual_col, &end) < 11)
        return;
    if (tab->modified && !modified)
        refresh_branch(tab->app); /* saved: the working tree changed */
    tab->modified = modified != 0;
    tab->line = line;
    tab->column = column;
    tab->errors = errors;
    tab->warnings = warnings;
    tab->height = height;
    tab->lines = lines;
    tab->tab_size = MAX(tab_size, 1);
    tab->start_col = start_col;
    tab->visual_col = visual_col;

    /* The problem lines ("e12,w30" or "-"), then the path. */
    const char *problems = title + 7 + end, *path = strchr(problems, ' ');
    if (!path)
        return;
    char *spec = g_strndup(problems, path++ - problems);
    editor_view_set_problems(tab, spec);
    g_free(spec);
    editor_view_update(tab, top);

    if (!tab->title && g_path_is_absolute(path) && g_strcmp0(tab->path, path) != 0) {
        /* Another file was opened inside micro (Ctrl+E open). When a tab has it already, that tab
         * takes over: two editors on one file would overwrite each other's saves. */
        Tab *other = find_tab(tab->app, TAB_EDITOR, path);
        if (other) {
            quit_editor(tab);
            select_tab(other);
            return;
        }
        g_free(tab->path);
        tab->path = g_strdup(path);
        editor_view_set_path(tab);
    }
    update_tab_label(tab);
    if (tab == current_tab(tab->app))
        update_status(tab->app);
}

/* micro starts when its tab is first shown: restoring a session with many tabs starts one or two
 * editors instead of all of them (each with a language server connection and its open file in
 * clangd's memory). Until then the commands sent to it wait in its control pipe. Shown and hidden
 * editors are told so: a hidden one stops its once-a-second report (micro/plug/tcode). */
static gboolean start_editor(gpointer data)
{
    Tab *tab = data;

    tab->start_id = 0;
    if (tab->argv && gtk_widget_get_mapped(GTK_WIDGET(tab->term))) {
        spawn(tab->app, tab->term, tab->argv, tab->env, &tab->pid);
        g_clear_pointer(&tab->argv, g_strfreev);
        g_clear_pointer(&tab->env, g_strfreev);
    }
    return G_SOURCE_REMOVE;
}

/* Not at once: in a shown window, every tab added is mapped while it is the current one, also the
 * ones a restored session adds before the next. */
static void on_editor_mapped(GtkWidget *term G_GNUC_UNUSED, Tab *tab)
{
    if (tab->argv && !tab->start_id)
        tab->start_id = g_idle_add(start_editor, tab);
    editor_control(tab, "visible 1");
}

static void on_editor_unmapped(GtkWidget *term G_GNUC_UNUSED, Tab *tab)
{
    if (!tab->argv)
        editor_control(tab, "visible 0");
}

/* Opens path in a new tab of the active group; micro starts once the tab is shown. line > 0 puts
 * the cursor there. */
Tab *open_editor(App *t, const char *path, int line, int column, gboolean readonly, char **extra_env)
{
    /* micro shows the path it was given in its status line: keep it short inside the project. */
    GFile *root = g_file_new_for_path(t->root), *file = g_file_new_for_path(path);
    char *relative = g_file_get_relative_path(root, file);
    char *position = line > 0 ? g_strdup_printf("+%d:%d", line, MAX(column, 1)) : NULL;
    GPtrArray *argv = g_ptr_array_new_with_free_func(g_free);
    GPtrArray *env = g_ptr_array_new_with_free_func(g_free);
    VteTerminal *term = new_terminal(t);
    Tab *tab = add_tab(t, TAB_EDITOR, editor_page(term), path);
    static int editors;
    const char *control;

    g_ptr_array_add(argv, g_strdup("micro"));
    g_ptr_array_add(argv, g_strdup("-config-dir"));
    g_ptr_array_add(argv, g_strdup(t->micro_config));
    g_ptr_array_add(argv, g_strdup("-syntax"));
    g_ptr_array_add(argv, g_strdup(t->plain ? "false" : "true"));
    settings_editor_args(t, argv);
    if (readonly) {
        g_ptr_array_add(argv, g_strdup("-readonly"));
        g_ptr_array_add(argv, g_strdup("true"));
    }
    if (position)
        g_ptr_array_add(argv, g_strdup(position));
    g_ptr_array_add(argv, g_strdup(relative ? relative : (char *)path));
    g_ptr_array_add(argv, NULL);

    tab->term = term;
    control = editor_view_init(tab, ++editors);
    for (int i = 0; extra_env && extra_env[i]; i++)
        g_ptr_array_add(env, g_strdup(extra_env[i]));
    if (control)
        g_ptr_array_add(env, g_strconcat("TCODE_CONTROL=", control, NULL));
    if (t->settings.detect_indent)
        g_ptr_array_add(env, g_strdup("TCODE_DETECT_INDENT=1"));
    /* micro-clip (micro/bin): the clipboard without micro freezing on a wl-paste that never returns. */
    if (t->micro_config)
        g_ptr_array_add(env, g_strdup_printf("PATH=%s/bin:%s", t->micro_config,
                                             g_getenv("PATH") ? g_getenv("PATH") : "/usr/bin:/bin"));
    lsp_editor_env(t, env);
    g_ptr_array_add(env, NULL);
    g_signal_connect(term, "child-exited", G_CALLBACK(on_editor_exited), tab);
    g_signal_connect(term, "termprop-changed::" VTE_TERMPROP_XTERM_TITLE, G_CALLBACK(on_editor_title), tab);
    g_signal_connect(term, "map", G_CALLBACK(on_editor_mapped), tab);
    g_signal_connect(term, "unmap", G_CALLBACK(on_editor_unmapped), tab);
    tab->argv = (char **)g_ptr_array_free(argv, FALSE);
    tab->env = (char **)g_ptr_array_free(env, FALSE);
    if (gtk_widget_get_mapped(GTK_WIDGET(term)))
        on_editor_mapped(GTK_WIDGET(term), tab);

    g_free(position);
    g_free(relative);
    g_object_unref(file);
    g_object_unref(root);
    focus_current_tab(t);
    return tab;
}

typedef struct {
    VteTerminal *term;
    char *keys;
} PendingKeys;

static gboolean feed_pending_keys(gpointer data)
{
    PendingKeys *pending = data;

    if (vte_terminal_get_pty(pending->term))
        vte_terminal_feed_child(pending->term, pending->keys, -1);
    g_object_unref(pending->term);
    g_free(pending->keys);
    g_free(pending);
    return G_SOURCE_REMOVE;
}

/* Feeds keys to an editor after a lone Escape that closes any active prompt. The keys must come
 * later than micro's escape timeout: an Escape directly followed by Ctrl+E reads as Alt+Ctrl+E,
 * and the rest would then be typed into the file. */
void feed_editor(Tab *tab, const char *keys)
{
    PendingKeys *pending = g_new(PendingKeys, 1);

    vte_terminal_feed_child(tab->term, "\x1b", 1);
    pending->term = g_object_ref(tab->term);
    pending->keys = g_strdup(keys);
    g_timeout_add(100, feed_pending_keys, pending);
}

/* Runs a micro command in an editor: through the plugin's pipe, or else typed into its command
 * bar (Ctrl+E). */
void editor_command(Tab *tab, const char *command)
{
    char *line = g_strdup_printf("command %s", command);

    if (!editor_control(tab, line)) {
        char *keys = g_strdup_printf("\x05%s\r", command);
        feed_editor(tab, keys);
        g_free(keys);
    }
    g_free(line);
}

typedef struct {
    App *app;
    SavedFunc then;
    gpointer data;
    int tries;
} PendingSave;

static gboolean when_saved(gpointer data)
{
    PendingSave *pending = data;

    if (!pending->app->closing && any_unsaved(pending->app) && ++pending->tries < 15)
        return G_SOURCE_CONTINUE;
    pending->then(pending->app, pending->data);
    g_free(pending);
    return G_SOURCE_REMOVE;
}

/* Saves every changed editor, then calls then (also when a save does not finish within ~450ms,
 * and while the app closes: then checks t->closing itself). The save goes through micro's command,
 * which works with its find or command bar open, where a typed Ctrl+S would be ignored. */
void save_all(App *t, SavedFunc then, gpointer data)
{
    PendingSave *pending = g_new0(PendingSave, 1);
    int n = tab_count(t);
    gboolean saving = FALSE;

    for (int i = 0; i < n; i++) {
        Tab *tab = tab_nth(t, i);
        if (tab->kind == TAB_EDITOR && tab->modified) {
            editor_command(tab, "save");
            saving = TRUE;
        }
    }
    pending->app = t;
    pending->then = then;
    pending->data = data;
    if (saving)
        g_timeout_add(30, when_saved, pending);
    else
        when_saved(pending);
}

void open_file_at(App *t, const char *path, int line, int column)
{
    Tab *existing = find_tab(t, TAB_EDITOR, path);

    if (!existing) {
        open_editor(t, path, line, column, FALSE, NULL);
        return;
    }
    select_tab(existing);
    if (line > 0) {
        char *command = g_strdup_printf("goto %d:%d", line, MAX(column, 1));
        editor_command(existing, command);
        g_free(command);
    }
}

void open_file(App *t, const char *path)
{
    open_file_at(t, path, 0, 0);
}

/* Opens the file in the second group, splitting first. An already open file moves there, so the
 * same file is never edited by two editors at once. */
void open_to_side(App *t, const char *path)
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

/* Turns code coloring off or on in every open editor; new ones start the same way. */
void toggle_syntax(App *t)
{
    int n = tab_count(t);

    t->plain = !t->plain;
    session_save_settings(t);
    for (int i = 0; i < n; i++) {
        Tab *tab = tab_nth(t, i);
        if (tab->kind == TAB_EDITOR) {
            editor_command(tab, t->plain ? "setlocal syntax off" : "setlocal syntax on");
            editor_view_redraw(tab);
        }
    }
}

/* Groups ------------------------------------------------------------------------------------- */

void update_editor_area(App *t)
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
    if (t->split_button && gtk_toggle_button_get_active(t->split_button) != split)
        gtk_toggle_button_set_active(t->split_button, split);
    update_editor_area(t);
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
    update_status(t);
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

/* The autosave setting. micro's own autosave timer woke every editor each second, changed or not,
 * and every wake redrew its screen for t-code to read: instead, once a second, the editors that
 * report unsaved changes are told to save. */
static gboolean autosave_tick(gpointer data)
{
    App *t = data;
    int n = tab_count(t);

    for (int i = 0; t->settings.autosave && !t->closing && i < n; i++) {
        Tab *tab = tab_nth(t, i);
        if (tab->kind == TAB_EDITOR && tab->modified && !tab->compare_with)
            editor_command(tab, "save");
    }
    return G_SOURCE_CONTINUE;
}

/* The editor area: a hint while nothing is open, otherwise the two groups. */
void create_groups(App *t)
{
    GtkWidget *hint = gtk_label_new("Open a file from the file manager on the left, or press Ctrl+P");

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
    g_timeout_add_seconds(1, autosave_tick, t);
}
