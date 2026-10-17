/*
 * Terminals: their look, starting programs in them, and the two that are always there (the file
 * manager and the shell). In the shell, Ctrl+click on a "file.c:12:5" location (compiler output)
 * opens the file at that line.
 */
#include "app.h"

#include <signal.h>
#include <string.h>
#include <unistd.h>

/* VS Code's terminal colors on its Dark Modern background. */
static const char *const palette_hex[16] = {
    "#000000", "#cd3131", "#0dbc79", "#e5e510", "#2472c8", "#bc3fbc", "#11a8cd", "#e5e5e5",
    "#666666", "#f14c4c", "#23d18b", "#f5f543", "#3b8eea", "#d670d6", "#29b8db", "#e5e5e5",
};

void set_terminal_font(App *t, VteTerminal *term, const char *font)
{
    PangoFontDescription *desc = pango_font_description_from_string(font ? font : DEFAULT_FONT);

    vte_terminal_set_font(term, desc);
    vte_terminal_set_font_scale(term, t->font_scale);
    pango_font_description_free(desc);
}

/* Gives every terminal the font the settings choose for it. */
void apply_fonts(App *t)
{
    int n = tab_count(t);

    set_terminal_font(t, t->fm, t->settings.fm_font);
    set_terminal_font(t, t->shell, t->settings.font);
    for (int i = 0; i < n; i++) {
        Tab *tab = tab_nth(t, i);
        if (tab->kind == TAB_EDITOR)
            set_terminal_font(t, tab->term, t->settings.font);
    }
}

/* A terminal in the editor font; the file manager gets its own in create_fixed_terminals(). */
VteTerminal *new_terminal(App *t)
{
    VteTerminal *term = VTE_TERMINAL(vte_terminal_new());
    GdkRGBA fg, bg, palette[16];

    set_terminal_font(t, term, t->settings.font);

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

/* Runs argv in term, in the project root, with $TCODE_SOCKET and extra_env ("NAME=value", may be
 * NULL) set; the child's pid goes to *pid. */
void spawn(App *t, VteTerminal *term, char **argv, char **extra_env, GPid *pid)
{
    char **env = g_get_environ();

    env = g_environ_setenv(env, "TCODE_SOCKET", t->socket_path, TRUE);
    for (int i = 0; extra_env && extra_env[i]; i++) {
        char **pair = g_strsplit(extra_env[i], "=", 2);
        if (pair[0] && pair[1])
            env = g_environ_setenv(env, pair[0], pair[1], TRUE);
        g_strfreev(pair);
    }
    vte_terminal_spawn_async(term, VTE_PTY_DEFAULT, t->root, argv, env, G_SPAWN_SEARCH_PATH,
                             NULL, NULL, NULL, -1, NULL, spawn_done, pid);
    g_strfreev(env);
}

/* The file manager and the shell ------------------------------------------------------------- */

static void start_fm(App *t)
{
    char *argv[] = { t->fm_program, NULL };

    spawn(t, t->fm, argv, NULL, &t->fm_pid);
}

/* $SHELL first, like most terminals; then the login shell from the user database. */
static void start_shell(App *t)
{
    const char *env = g_getenv("SHELL");
    char *shell = env && *env ? g_strdup(env) : vte_get_user_shell();
    char *argv[] = { shell ? shell : "/bin/sh", NULL };

    spawn(t, t->shell, argv, NULL, &t->shell_pid);
    g_free(shell);
}

void start_fixed_terminals(App *t)
{
    start_fm(t);
    start_shell(t);
}

/* They come back when they exit, in the current project (see on_fixed_exited). */
void restart_fixed_terminals(App *t)
{
    if (t->fm_pid > 0)
        kill(t->fm_pid, SIGHUP);
    if (t->shell_pid > 0)
        kill(t->shell_pid, SIGHUP);
}

static void on_fixed_exited(VteTerminal *term, int status G_GNUC_UNUSED, App *t)
{
    if (t->closing)
        return;
    if (term == t->fm)
        t->fm_pid = 0; /* gone: nothing may signal its pid until the new one starts */
    else
        t->shell_pid = 0;
    vte_terminal_reset(term, TRUE, TRUE);
    if (term == t->fm)
        start_fm(t);
    else
        start_shell(t);
}

/* The process group in the foreground of the shell pane: the shell itself when it waits at its
 * prompt, else the program it runs. 0 when unknown. */
static pid_t shell_foreground(App *t)
{
    VtePty *pty = vte_terminal_get_pty(t->shell);
    pid_t group = pty ? tcgetpgrp(vte_pty_get_fd(pty)) : -1;

    return group > 0 ? group : 0;
}

typedef struct {
    App *app;
    char *command;
    int tries;
} ShellRun;

static ShellRun *waiting; /* the command that waits for the shell's prompt */

static void type_in_shell(App *t, const char *command)
{
    char *line = g_strdup_printf("\x15%s\r", command);

    vte_terminal_feed_child(t->shell, line, -1);
    g_free(line);
}

static void show_shell_busy(App *t, pid_t group)
{
    char *path = g_strdup_printf("/proc/%d/comm", group), *name = NULL;
    GtkAlertDialog *dialog = gtk_alert_dialog_new("The terminal is busy");
    char *detail;

    g_file_get_contents(path, &name, NULL, NULL);
    detail = g_strdup_printf("%s is still running in the terminal and did not stop on Ctrl+C. "
                             "Close it, then try again.", name ? g_strstrip(name) : "A program");
    gtk_alert_dialog_set_detail(dialog, detail);
    gtk_alert_dialog_show(dialog, t->window);
    g_object_unref(dialog);
    g_free(detail);
    g_free(name);
    g_free(path);
}

/* Waits for an interrupted program to give the prompt back (2 seconds at most), then types the
 * command. */
static gboolean run_when_idle(gpointer data)
{
    ShellRun *run = data;
    App *t = run->app;
    pid_t group = shell_foreground(t);
    gboolean busy = group && group != t->shell_pid;

    if (busy && !t->closing && ++run->tries < 40)
        return G_SOURCE_CONTINUE;
    if (busy && !t->closing)
        show_shell_busy(t, group);
    else if (!t->closing)
        type_in_shell(t, run->command);
    waiting = NULL;
    g_free(run->command);
    g_free(run);
    return G_SOURCE_REMOVE;
}

/* Types command into the shell pane and runs it. Ctrl+U first drops a half-typed line. When a
 * program runs in the shell (the last Run, gdb), it gets Ctrl+C first, so the command does not go
 * to its input. */
void run_in_shell(App *t, const char *command)
{
    pid_t group = shell_foreground(t);
    gboolean busy = group && group != t->shell_pid;

    set_pane_visible(t, GTK_WIDGET(t->shell), TRUE);
    gtk_widget_grab_focus(GTK_WIDGET(t->shell));
    if (waiting) {
        /* Build or Run again while the last one waits: one command runs, the newest. */
        g_free(waiting->command);
        waiting->command = g_strdup(command);
        waiting->tries = 0;
        if (busy)
            kill(-group, SIGINT);
        return;
    }
    if (!busy) {
        type_in_shell(t, command);
        return;
    }
    kill(-group, SIGINT);
    waiting = g_new0(ShellRun, 1);
    waiting->app = t;
    waiting->command = g_strdup(command);
    g_timeout_add(50, run_when_idle, waiting);
}

/* Clickable compiler messages ------------------------------------------------------------------ */

#define PCRE2_MULTILINE 0x00000400u /* VTE wants its match patterns multiline */

/* path/to/file.ext:line or path/to/file.ext:line:column */
static const char *const location_pattern =
    "(?:[A-Za-z0-9_.~+-]*/)*[A-Za-z0-9_.+-]+\\.[A-Za-z0-9]+:[0-9]+(?::[0-9]+)?";

/* Where the shell is now (bash reports it on Fedora), or else the project root. */
static char *shell_directory(App *t)
{
    const char *uri = vte_terminal_get_termprop_string(t->shell, VTE_TERMPROP_CURRENT_DIRECTORY_URI, NULL);
    char *dir = uri ? g_filename_from_uri(uri, NULL, NULL) : NULL;

    return dir ? dir : g_strdup(t->root);
}

static void open_location(App *t, const char *location)
{
    const char *colon = strchr(location, ':');
    char *name = g_strndup(location, colon - location);
    int line = 0, column = 0;
    char *dir = shell_directory(t);
    const char *const bases[] = { dir, t->root };
    char *path = NULL;

    sscanf(colon + 1, "%d:%d", &line, &column);
    if (g_path_is_absolute(name)) {
        path = g_strdup(name);
    } else {
        for (size_t i = 0; i < G_N_ELEMENTS(bases) && !path; i++) {
            char *candidate = g_build_filename(bases[i], name, NULL);
            if (g_file_test(candidate, G_FILE_TEST_IS_REGULAR))
                path = g_canonicalize_filename(candidate, NULL);
            g_free(candidate);
        }
    }
    if (path && g_file_test(path, G_FILE_TEST_IS_REGULAR))
        open_file_at(t, path, line, column);
    g_free(path);
    g_free(dir);
    g_free(name);
}

static void on_shell_click(GtkGestureClick *gesture, int n_press G_GNUC_UNUSED, double x, double y, App *t)
{
    GdkModifierType state = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture));
    char *match;

    if (!(state & GDK_CONTROL_MASK))
        return;
    match = vte_terminal_check_match_at(t->shell, x, y, NULL);
    if (match) {
        gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
        open_location(t, match);
        g_free(match);
    }
}

static void add_location_links(App *t)
{
    GError *error = NULL;
    VteRegex *regex = vte_regex_new_for_match(location_pattern, -1, PCRE2_MULTILINE, &error);
    GtkGesture *click = gtk_gesture_click_new();

    if (!regex) {
        g_warning("t-code: %s", error->message);
        g_error_free(error);
        return;
    }
    int tag = vte_terminal_match_add_regex(t->shell, regex, 0);
    vte_terminal_match_set_cursor_name(t->shell, tag, "pointer");
    vte_regex_unref(regex);

    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click), GTK_PHASE_CAPTURE);
    g_signal_connect(click, "pressed", G_CALLBACK(on_shell_click), t);
    gtk_widget_add_controller(GTK_WIDGET(t->shell), GTK_EVENT_CONTROLLER(click));
}

void create_fixed_terminals(App *t)
{
    t->fm = new_terminal(t);
    t->shell = new_terminal(t);
    set_terminal_font(t, t->fm, t->settings.fm_font);
    g_signal_connect(t->fm, "child-exited", G_CALLBACK(on_fixed_exited), t);
    g_signal_connect(t->shell, "child-exited", G_CALLBACK(on_fixed_exited), t);
    add_location_links(t);
}
