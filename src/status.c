/*
 * The status bar at the bottom, like VS Code's:
 *   left   git branch ("*" when there are changes), clangd errors and warnings in the open files,
 *          Build and Run buttons and what they would build
 *   right  cursor line and column, and the Keybinds button
 */
#include "app.h"

#include <signal.h>
#include <string.h>

static const char *const css =
    /* A thin title bar, like VS Code's. */
    "headerbar { min-height: 30px; padding: 0 4px; background: #181818; box-shadow: none;"
    "  border-bottom: 1px solid #2b2b2b; }"
    "headerbar * { min-height: 0; margin-top: 0; margin-bottom: 0; padding-top: 0; padding-bottom: 0; }"
    "headerbar .title { font-size: 0.9em; font-weight: normal; }"
    "headerbar button { min-height: 22px; min-width: 22px; padding: 0 4px; margin: 4px 0; }"
    "headerbar windowcontrols button { min-height: 18px; min-width: 18px; padding: 1px; margin: 0 1px; }"
    ".tcode-status { background: #181818; border-top: 1px solid #2b2b2b; padding: 0 6px; }"
    ".tcode-status button { min-height: 0; padding: 1px 8px; border-radius: 0; color: #cccccc; }"
    ".tcode-status button:hover { background: rgba(255, 255, 255, .12); }"
    ".tcode-status label { color: #cccccc; padding: 0 8px; }"
    ".tcode-status label.dim { color: #9d9d9d; }";

void update_status(App *t)
{
    int n, errors = 0, warnings = 0;
    Tab *tab;
    char *text;

    if (!t->status.build || t->closing)
        return;

    text = project_summary(t);
    gtk_label_set_text(t->status.build, text);
    g_free(text);

    n = tab_count(t);
    for (int i = 0; i < n; i++) {
        Tab *each = tab_nth(t, i);
        if (!each->compare_with) { /* old versions in a comparison do not count */
            errors += each->errors;
            warnings += each->warnings;
        }
    }
    text = g_strdup_printf("⊗ %d  ⚠ %d", errors, warnings);
    gtk_label_set_text(t->status.problems, text);
    g_free(text);

    tab = current_tab(t);
    if (tab && tab->kind == TAB_EDITOR && tab->line > 0) {
        text = g_strdup_printf("Ln %d, Col %d", tab->line, tab->column);
        gtk_label_set_text(t->status.position, text);
        g_free(text);
        gtk_widget_set_visible(GTK_WIDGET(t->status.position), TRUE);
    } else {
        gtk_widget_set_visible(GTK_WIDGET(t->status.position), FALSE);
    }
}

/* Git branch ----------------------------------------------------------------------------------
 * git status runs when the repository changes (.git/HEAD or .git/index, watched), when an editor
 * saves, and every 10 seconds for changes made outside t-code (a build, a shell command). */

static gboolean branch_busy, branch_again;
static guint branch_soon_id;
static GFileMonitor *git_monitor;
static guint last_status; /* hash of git status's output */
static gboolean poll_branch(gpointer data);

/* The output (porcelain v2, -z) also fills the Source Control panel (scm.c). */
static void on_branch(GObject *source, GAsyncResult *result, gpointer data)
{
    App *t = data;
    GBytes *out = NULL;
    gboolean ok = g_subprocess_communicate_finish(G_SUBPROCESS(source), result, &out, NULL, NULL) &&
                  g_subprocess_get_if_exited(G_SUBPROCESS(source)) &&
                  g_subprocess_get_exit_status(G_SUBPROCESS(source)) == 0;
    gsize size = 0;
    const char *bytes = out ? g_bytes_get_data(out, &size) : NULL;
    char *branch = NULL;
    gboolean dirty = FALSE;

    branch_busy = FALSE;
    if (branch_again && !t->closing) {
        branch_again = FALSE;
        refresh_branch(t);
    }
    if (ok && bytes && !t->closing) {
        /* Records end with NUL; only the header lines matter here. */
        for (const char *p = bytes, *end = bytes + size; p < end;) {
            const char *nul = memchr(p, '\0', end - p);
            gsize len = (nul ? nul : end) - p;
            if (len > 14 && strncmp(p, "# branch.head ", 14) == 0)
                branch = g_strndup(p + 14, len - 14);
            else if (len && *p != '#')
                dirty = TRUE;
            p += len + 1;
        }
    }
    if (!t->closing)
        scm_update(t, ok ? bytes : NULL, size);
    /* The file manager watches only open folders: tell it when a file changed in a closed one. */
    guint hash = out ? g_bytes_hash(out) : 0;
    if (hash != last_status && last_status && !t->closing && t->fm_pid > 0)
        kill(t->fm_pid, SIGUSR1);
    last_status = hash;
    if (!t->closing && t->status.branch) {
        if (branch) {
            char *text = g_strdup_printf("⎇ %s%s", branch, dirty ? "*" : "");
            gtk_label_set_text(t->status.branch, text);
            g_free(text);
        }
        gtk_widget_set_visible(GTK_WIDGET(t->status.branch), branch != NULL);
    }
    g_free(branch);
    if (out)
        g_bytes_unref(out);
}

static gboolean poll_branch(gpointer data)
{
    App *t = data;
    GSubprocess *proc;

    if (t->closing)
        return G_SOURCE_REMOVE;
    if (branch_busy) {
        branch_again = TRUE; /* the running one may have missed the change */
        return G_SOURCE_CONTINUE;
    }
    proc = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL,
                            "git", "-C", t->root, "status", "--porcelain=v2", "--branch", "-z",
                            "--untracked-files=all", NULL);
    if (proc) {
        branch_busy = TRUE;
        g_subprocess_communicate_async(proc, NULL, NULL, on_branch, t);
        g_object_unref(proc);
    }
    return G_SOURCE_CONTINUE;
}

static gboolean poll_branch_once(gpointer data)
{
    branch_soon_id = 0;
    poll_branch(data);
    return G_SOURCE_REMOVE;
}

/* Runs git status shortly; changes that come together (a commit touches several files) share one. */
void refresh_branch(App *t)
{
    if (!branch_soon_id)
        branch_soon_id = g_timeout_add(300, poll_branch_once, t);
}

static gboolean is_git_state(GFile *file)
{
    char *name = file ? g_file_get_basename(file) : NULL;
    gboolean yes = name && (strcmp(name, "HEAD") == 0 || strcmp(name, "index") == 0);

    g_free(name);
    return yes;
}

static void on_git_changed(GFileMonitor *monitor G_GNUC_UNUSED, GFile *file, GFile *other,
                           GFileMonitorEvent event G_GNUC_UNUSED, App *t)
{
    /* git writes HEAD.lock and index.lock, then renames them into place (other is the new name). */
    if (is_git_state(file) || is_git_state(other))
        refresh_branch(t);
}

/* Watches the git directory of the project (t->root) and finds the top of its work tree
 * (t->git_top, NULL outside a repository); call again after the root changes. */
void watch_git(App *t)
{
    char *argv[] = { "git", "-C", t->root, "rev-parse", "--absolute-git-dir", "--show-toplevel", NULL };
    char *out = NULL;
    int status = 0;

    if (git_monitor) {
        g_file_monitor_cancel(git_monitor);
        g_clear_object(&git_monitor);
    }
    if (g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL,
                     &out, NULL, &status, NULL) &&
        g_spawn_check_wait_status(status, NULL)) {
        char **lines = g_strsplit(out, "\n", 3);
        GFile *dir = g_file_new_for_path(lines[0]);
        g_free(t->git_top);
        t->git_top = lines[1] && *lines[1] ? g_strdup(lines[1]) : NULL;
        g_strfreev(lines);
        git_monitor = g_file_monitor_directory(dir, G_FILE_MONITOR_WATCH_MOVES, NULL, NULL);
        if (git_monitor)
            g_signal_connect(git_monitor, "changed", G_CALLBACK(on_git_changed), t);
        g_object_unref(dir);
    } else {
        g_clear_pointer(&t->git_top, g_free);
    }
    g_free(out);
    refresh_branch(t);
}

void start_status_updates(App *t)
{
    watch_git(t);
    g_timeout_add_seconds(10, poll_branch, t);
    update_status(t);
}

/* Widgets ------------------------------------------------------------------------------------ */

static GtkLabel *status_label(GtkWidget *bar, gboolean dim)
{
    GtkWidget *label = gtk_label_new(NULL);

    if (dim)
        gtk_widget_add_css_class(label, "dim");
    gtk_box_append(GTK_BOX(bar), label);
    return GTK_LABEL(label);
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

static void on_build(App *t)
{
    build_project(t, FALSE);
}

static void on_run(App *t)
{
    build_project(t, TRUE);
}

GtkWidget *status_bar(App *t)
{
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkCssProvider *provider = gtk_css_provider_new();

    gtk_css_provider_load_from_string(provider, css);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
    gtk_widget_add_css_class(bar, "tcode-status");

    t->status.branch = status_label(bar, FALSE);
    gtk_widget_set_visible(GTK_WIDGET(t->status.branch), FALSE);
    t->status.problems = status_label(bar, FALSE);
    gtk_widget_set_tooltip_text(GTK_WIDGET(t->status.problems), "clangd errors and warnings in the open files");
    status_button(bar, "⚙ Build", "Build (Ctrl+Shift+B)", G_CALLBACK(on_build), t);
    status_button(bar, "▶ Run", "Build and run (F5)", G_CALLBACK(on_run), t);
    t->status.build = status_label(bar, TRUE);

    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(bar), spacer);
    t->status.position = status_label(bar, FALSE);
    status_button(bar, "⌨ Keybinds", "All keyboard shortcuts", G_CALLBACK(show_keybinds), t);
    return bar;
}
