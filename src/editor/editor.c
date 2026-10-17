/*
 * What runs in an editor tab: micro, in a terminal (see editor.h). micro reports its state through
 * the terminal title (micro/plug/tcode), as
 *   "t-code <modified> <line> <column> <errors> <warnings> <top> <height> <lines> <tabsize>
 *    <startcol> <visualcol> <problems> <path>"
 * and takes commands through a named pipe (control.c).
 */
#include "editor/editor.h"

#include "dialogs/settings.h"
#include "editor/control.h"
#include "editor/editorview.h"
#include "editor/minimap.h"
#include "editor/tabs.h"
#include "git/compare.h"
#include "git/gitstatus.h"
#include "ipc/lsp.h"
#include "project/session.h"
#include "window/statusbar.h"
#include "window/terminal.h"

#include <stdio.h>
#include <string.h>

/* Starting and following micro --------------------------------------------------------------- */

/* micro exited (the tab was closed through it, or it crashed): the tab goes too. */
static void on_editor_exited(VteTerminal *term G_GNUC_UNUSED, int status G_GNUC_UNUSED, Tab *tab)
{
    tab->pid = 0;
    if (tab->app->closing)
        return;
    if (tab->compare_with)
        forget_comparison(tab);
    remove_tab(tab);
}

/* micro wrote a new terminal title (see the top of this file): takes its state into the tab, the
 * minimap, the scrollbar and the status bar. "t-code-open" titles come from Go to Definition into
 * another file. */
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
    minimap_set_problems(tab, spec);
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

/* The tab is hidden (another tab was chosen): micro stops reporting until it is shown again. */
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

/* Talking to micro --------------------------------------------------------------------------- */

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

typedef struct {
    VteTerminal *term;
    char *keys;
} PendingKeys;

/* feed_editor()'s timeout: the keys, once micro has taken the Escape alone. */
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

/* save_all()'s check every 30ms: are all editors saved (or 15 tries gone)? Then the callback. */
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

/* An open file is not opened twice: its tab is shown and its cursor moved. */
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

void start_autosave(App *t)
{
    g_timeout_add_seconds(1, autosave_tick, t);
}
