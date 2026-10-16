/*
 * The Source Control panel, right of the shell (Ctrl+Shift+G), like VS Code's:
 *   a commit message and Commit / Push buttons
 *   Staged Changes and Changes: click a file to see its changes side by side; + stages, − unstages,
 *     ↶ discards (after asking)
 *   Commits: click one to list its files, click a file to see what that commit changed
 * The lists come from the git status that status.c runs anyway (scm_update); git commands run in
 * the background, and the git directory monitor in status.c refreshes everything afterwards.
 */
#include "app.h"

#include <string.h>

typedef struct {
    char *path; /* relative to the work tree top */
    char *old_path; /* renames: where it was */
    char code;  /* M A D R C U(ntracked) !(conflict) */
} Change;

static struct {
    App *app;
    GtkWidget *message;     /* GtkTextView */
    GtkWidget *placeholder;
    GtkWidget *commit, *push;
    GtkLabel *note;         /* the last error or result */
    GtkListBox *staged_list, *changes_list, *commits_list;
    GtkLabel *staged_title, *changes_title;
    GtkWidget *staged_box, *changes_box;
    GPtrArray *staged, *changes; /* Change */
    char *head;             /* the commit HEAD is at: commits are listed again when it moves */
    char *upstream;
    int ahead, behind;
    gboolean initial;       /* no commit yet */
    gboolean busy;          /* a git command runs */
    char *open_commit;      /* the commit whose files are shown */
} scm;

static const char *const css =
    ".tcode-scm { background: #181818; border-left: 1px solid #2b2b2b; }"
    ".tcode-scm .title { font-size: 0.8em; font-weight: bold; color: #cccccc; margin: 6px 10px 4px; }"
    ".tcode-scm textview, .tcode-scm textview text { background: #313131; color: #cccccc; }"
    ".tcode-scm .message { border: 1px solid #3c3c3c; margin: 0 10px; }"
    ".tcode-scm .message:focus-within { border-color: #0078d4; }"
    ".tcode-scm .placeholder { color: #8b8b8b; margin: 4px 6px; }"
    ".tcode-scm .actions { margin: 6px 10px; }"
    ".tcode-scm .actions button { min-height: 24px; padding: 0 10px; }"
    ".tcode-scm .primary { background: #0078d4; color: #ffffff; }"
    ".tcode-scm .primary:hover { background: #026ec1; }"
    ".tcode-scm .note { color: #9d9d9d; font-size: 0.85em; margin: 0 10px 4px; }"
    ".tcode-scm .note.error { color: #f14c4c; }"
    ".tcode-scm .section { font-size: 0.8em; font-weight: bold; color: #cccccc; padding: 2px 10px; }"
    ".tcode-scm list { background: transparent; }"
    ".tcode-scm row { padding: 0 6px 0 18px; min-height: 22px; color: #cccccc; }"
    ".tcode-scm row:hover { background: #2a2d2e; }"
    ".tcode-scm row:selected { background: #04395e; }"
    ".tcode-scm row .dim { color: #8b8b8b; font-size: 0.9em; }"
    ".tcode-scm button.tool { min-height: 0; min-width: 0; padding: 0 5px; opacity: 0; }"
    ".tcode-scm row:hover button.tool, .tcode-scm .section:hover button.tool { opacity: 1; }"
    ".tcode-scm button.open { min-height: 0; padding: 0; background: none; box-shadow: none; }"
    ".tcode-scm .code { font-family: 'Cascadia Code', monospace; font-weight: bold; min-width: 14px; }"
    ".tcode-scm .code-M { color: #e2c08d; } .tcode-scm .code-A, .tcode-scm .code-U { color: #73c991; }"
    ".tcode-scm .code-D { color: #c74e39; } .tcode-scm .code-R, .tcode-scm .code-C { color: #73c991; }"
    ".tcode-scm .code-x { color: #e4676b; }"
    ".tcode-scm .file-row { padding-left: 34px; }";

static void change_free(Change *c)
{
    g_free(c->path);
    g_free(c->old_path);
    g_free(c);
}

static void set_note(const char *text, gboolean error)
{
    gtk_label_set_text(scm.note, text ? text : "");
    gtk_widget_set_visible(GTK_WIDGET(scm.note), text && *text);
    if (error)
        gtk_widget_add_css_class(GTK_WIDGET(scm.note), "error");
    else
        gtk_widget_remove_css_class(GTK_WIDGET(scm.note), "error");
}

/* Running git ------------------------------------------------------------------------------------ */

typedef void (*GitDone)(gboolean ok, const char *out, const char *err, gpointer data);

typedef struct {
    GitDone done;
    gpointer data;
    gboolean quiet; /* a background read: does not hold the buttons */
} GitCall;

static void on_git_done(GObject *source, GAsyncResult *result, gpointer user)
{
    GitCall *call = user;
    char *out = NULL, *err = NULL;
    gboolean ok = g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &out, &err, NULL) &&
                  g_subprocess_get_if_exited(G_SUBPROCESS(source)) &&
                  g_subprocess_get_exit_status(G_SUBPROCESS(source)) == 0;

    if (!call->quiet)
        scm.busy = FALSE;
    if (scm.app && !scm.app->closing) {
        if (call->done)
            call->done(ok, out ? out : "", err ? g_strstrip(err) : "", call->data);
        if (!call->quiet)
            refresh_branch(scm.app);
    }
    g_free(out);
    g_free(err);
    g_free(call);
}

/* Runs git in the work tree with args (NULL-terminated); input goes to its stdin. */
static void run_git(const char *const *args, const char *input, gboolean quiet, GitDone done, gpointer data)
{
    GSubprocessLauncher *launcher;
    GPtrArray *argv = g_ptr_array_new();
    GError *error = NULL;
    GSubprocess *proc;
    GitCall *call;

    if (!scm.app->git_top)
        return;
    g_ptr_array_add(argv, "git");
    for (int i = 0; args[i]; i++)
        g_ptr_array_add(argv, (char *)args[i]);
    g_ptr_array_add(argv, NULL);
    launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE |
                                         (input ? G_SUBPROCESS_FLAGS_STDIN_PIPE : 0));
    g_subprocess_launcher_set_cwd(launcher, scm.app->git_top);
    /* No terminal to answer a password prompt in: fail instead of waiting forever. */
    g_subprocess_launcher_setenv(launcher, "GIT_TERMINAL_PROMPT", "0", TRUE);
    proc = g_subprocess_launcher_spawnv(launcher, (const char *const *)argv->pdata, &error);
    g_object_unref(launcher);
    g_ptr_array_free(argv, TRUE);
    if (!proc) {
        set_note(error->message, TRUE);
        g_error_free(error);
        return;
    }
    call = g_new0(GitCall, 1);
    call->done = done;
    call->data = data;
    call->quiet = quiet;
    if (!quiet)
        scm.busy = TRUE;
    g_subprocess_communicate_utf8_async(proc, input, NULL, on_git_done, call);
    g_object_unref(proc);
}

/* Shows git's complaint when a command fails. */
static void report_failure(gboolean ok, const char *out G_GNUC_UNUSED, const char *err, gpointer data)
{
    if (!ok)
        set_note(*err ? err : data, TRUE);
}

static void run_simple(const char *const *args, const char *failure)
{
    set_note(NULL, FALSE);
    run_git(args, NULL, FALSE, report_failure, (gpointer)failure);
}

/* Staging ---------------------------------------------------------------------------------------- */

static void stage(const char *path)
{
    const char *args[] = { "add", "-A", "--", path, NULL };
    run_simple(args, "Could not stage the file");
}

static void unstage(const char *path)
{
    /* Before the first commit there is no HEAD to restore from. */
    const char *first[] = { "rm", "--cached", "-r", "-q", "--", path, NULL };
    const char *args[] = { "restore", "--staged", "--", path, NULL };
    run_simple(scm.initial ? first : args, "Could not unstage the file");
}

static void stage_all(void)
{
    const char *args[] = { "add", "-A", NULL };
    run_simple(args, "Could not stage the changes");
}

static void unstage_all(void)
{
    const char *first[] = { "rm", "--cached", "-r", "-q", ".", NULL };
    const char *args[] = { "reset", "-q", NULL };
    run_simple(scm.initial ? first : args, "Could not unstage the changes");
}

typedef struct {
    char *path;
    gboolean untracked;
} Discard;

static void on_discard_answer(GObject *dialog, GAsyncResult *result, gpointer data)
{
    Discard *d = data;

    if (gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(dialog), result, NULL) == 1) {
        if (d->untracked) {
            /* Never in git: to the trash, where it can still be found. */
            char *full = g_build_filename(scm.app->git_top, d->path, NULL);
            GFile *file = g_file_new_for_path(full);
            GError *error = NULL;
            if (!g_file_trash(file, NULL, &error)) {
                set_note(error->message, TRUE);
                g_error_free(error);
            }
            g_object_unref(file);
            g_free(full);
            refresh_branch(scm.app);
        } else {
            const char *args[] = { "restore", "--", d->path, NULL };
            run_simple(args, "Could not discard the changes");
        }
    }
    g_free(d->path);
    g_free(d);
}

static void discard(const Change *c)
{
    Discard *d = g_new0(Discard, 1);
    GtkAlertDialog *dialog = gtk_alert_dialog_new("Discard changes in %s?", c->path);
    const char *buttons[] = { "Cancel", "Discard", NULL };

    d->path = g_strdup(c->path);
    d->untracked = c->code == 'U';
    gtk_alert_dialog_set_detail(dialog, d->untracked ? "The file is not in git; it goes to the trash."
                                                     : "Its unstaged changes are lost.");
    gtk_alert_dialog_set_buttons(dialog, buttons);
    gtk_alert_dialog_set_cancel_button(dialog, 0);
    gtk_alert_dialog_set_default_button(dialog, 0);
    gtk_alert_dialog_choose(dialog, scm.app->window, NULL, on_discard_answer, d);
    g_object_unref(dialog);
}

/* Commit and push -------------------------------------------------------------------------------- */

static char *message_text(void)
{
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(scm.message));
    GtkTextIter start, end;

    gtk_text_buffer_get_bounds(buffer, &start, &end);
    return g_strstrip(gtk_text_buffer_get_text(buffer, &start, &end, FALSE));
}

static void on_committed(gboolean ok, const char *out G_GNUC_UNUSED, const char *err, gpointer data G_GNUC_UNUSED)
{
    if (!ok) {
        set_note(*err ? err : "The commit failed", TRUE);
        return;
    }
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(scm.message)), "", -1);
    set_note("Committed", FALSE);
}

/* With nothing staged, the commit takes every change (like VS Code's smart commit). */
static void on_staged_all(gboolean ok, const char *out G_GNUC_UNUSED, const char *err, gpointer data)
{
    const char *args[] = { "commit", "-q", "-F", "-", NULL };

    if (!ok) {
        set_note(*err ? err : "Could not stage the changes", TRUE);
        g_free(data);
        return;
    }
    run_git(args, data, FALSE, on_committed, NULL);
    g_free(data);
}

static void commit(void)
{
    char *message = message_text();

    if (scm.busy) {
        g_free(message);
        return;
    }
    if (!*message) {
        set_note("Type a commit message first", TRUE);
        gtk_widget_grab_focus(scm.message);
    } else if (scm.staged->len == 0 && scm.changes->len == 0) {
        set_note("Nothing to commit", TRUE);
    } else {
        set_note(NULL, FALSE);
        if (scm.staged->len == 0) {
            const char *add[] = { "add", "-A", NULL };
            run_git(add, NULL, FALSE, on_staged_all, g_strdup(message));
        } else {
            const char *args[] = { "commit", "-q", "-F", "-", NULL };
            run_git(args, message, FALSE, on_committed, NULL);
        }
    }
    g_free(message);
}

static void on_pushed(gboolean ok, const char *out G_GNUC_UNUSED, const char *err, gpointer data G_GNUC_UNUSED)
{
    if (ok) {
        set_note("Pushed", FALSE);
        return;
    }
    if (strstr(err, "terminal prompts disabled") || strstr(err, "could not read Username"))
        set_note("git needs a password or token, and there is no terminal to ask in. Push once from the "
                 "shell, or set up a credential helper (e.g. gh auth setup-git) or an SSH key.", TRUE);
    else
        set_note(*err ? err : "The push failed", TRUE);
}

static void push(void)
{
    /* A branch with no upstream yet goes to origin under its own name. */
    const char *args[] = { "push", NULL };
    const char *first[] = { "push", "-u", "origin", "HEAD", NULL };

    if (scm.busy)
        return;
    set_note("Pushing…", FALSE);
    run_git(scm.upstream ? args : first, NULL, FALSE, on_pushed, NULL);
}

/* Lists ------------------------------------------------------------------------------------------ */

static void clear_list(GtkListBox *list)
{
    GtkWidget *child;

    while ((child = gtk_widget_get_first_child(GTK_WIDGET(list))))
        gtk_list_box_remove(list, child);
}

static GtkWidget *icon_button(const char *text, const char *tooltip)
{
    GtkWidget *button = gtk_button_new_with_label(text);

    gtk_button_set_has_frame(GTK_BUTTON(button), FALSE);
    gtk_widget_set_focusable(button, FALSE);
    gtk_widget_set_tooltip_text(button, tooltip);
    gtk_widget_add_css_class(button, "tool");
    return button;
}

/* Puts a row's main content in a flat button that activates the row: a click, Enter or the
 * accessibility "click" action all open it. */
static GtkWidget *open_button(GtkWidget *row, GtkWidget *content)
{
    GtkWidget *button = gtk_button_new();

    gtk_button_set_has_frame(GTK_BUTTON(button), FALSE);
    gtk_widget_add_css_class(button, "open");
    gtk_widget_set_hexpand(button, TRUE);
    gtk_button_set_child(GTK_BUTTON(button), content);
    g_signal_connect_swapped(button, "clicked", G_CALLBACK(gtk_widget_activate), row);
    return button;
}

static GtkWidget *code_label(char code)
{
    char text[2] = { code, 0 };
    char *class = g_strdup_printf("code-%c", code == '!' ? 'x' : code);
    GtkWidget *label = gtk_label_new(text);

    gtk_widget_add_css_class(label, "code");
    gtk_widget_add_css_class(label, class);
    g_free(class);
    return label;
}

/* A row: name, its folder dimmed, then the buttons and the status letter on the right. */
static GtkWidget *file_row(const char *path, char code, GtkWidget **box_out)
{
    GtkWidget *row = gtk_list_box_row_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *names = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    char *name = g_path_get_basename(path), *dir = g_path_get_dirname(path);
    GtkWidget *label = gtk_label_new(name);
    GtkWidget *folder = gtk_label_new(strcmp(dir, ".") == 0 ? "" : dir);

    gtk_label_set_ellipsize(GTK_LABEL(folder), PANGO_ELLIPSIZE_START);
    gtk_widget_add_css_class(folder, "dim");
    gtk_widget_set_hexpand(folder, TRUE);
    gtk_label_set_xalign(GTK_LABEL(folder), 0);
    gtk_box_append(GTK_BOX(names), label);
    gtk_box_append(GTK_BOX(names), folder);
    gtk_box_append(GTK_BOX(box), open_button(row, names));
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
    gtk_widget_set_tooltip_text(row, path);
    *box_out = box;
    g_object_set_data(G_OBJECT(row), "code", GINT_TO_POINTER(code));
    g_free(dir);
    g_free(name);
    return row;
}

static void on_stage_clicked(GtkButton *button G_GNUC_UNUSED, Change *c) { stage(c->path); }
static void on_unstage_clicked(GtkButton *button G_GNUC_UNUSED, Change *c) { unstage(c->path); }
static void on_discard_clicked(GtkButton *button G_GNUC_UNUSED, Change *c) { discard(c); }

static void fill_changes(GtkListBox *list, GPtrArray *changes, gboolean staged)
{
    clear_list(list);
    for (guint i = 0; i < changes->len; i++) {
        Change *c = g_ptr_array_index(changes, i);
        GtkWidget *box, *row = file_row(c->path, c->code, &box);
        GtkWidget *button;

        g_object_set_data(G_OBJECT(row), "change", c);
        if (staged) {
            button = icon_button("−", "Unstage");
            g_signal_connect(button, "clicked", G_CALLBACK(on_unstage_clicked), c);
            gtk_box_append(GTK_BOX(box), button);
        } else {
            button = icon_button("↶", "Discard changes");
            g_signal_connect(button, "clicked", G_CALLBACK(on_discard_clicked), c);
            gtk_box_append(GTK_BOX(box), button);
            button = icon_button("+", "Stage");
            g_signal_connect(button, "clicked", G_CALLBACK(on_stage_clicked), c);
            gtk_box_append(GTK_BOX(box), button);
        }
        gtk_box_append(GTK_BOX(box), code_label(c->code));
        gtk_list_box_append(list, row);
    }
}

/* A changed file next to its last commit (HEAD), staged or not. */
static void on_change_activated(GtkListBox *list G_GNUC_UNUSED, GtkListBoxRow *row, gpointer data G_GNUC_UNUSED)
{
    Change *c = g_object_get_data(G_OBJECT(row), "change");
    App *t = scm.app;
    char *full;

    if (!c || !t->git_top)
        return;
    full = g_build_filename(t->git_top, c->path, NULL);
    if (c->code == 'D')
        compare_revisions(t, "HEAD", c->path, NULL, NULL); /* only the old version is left */
    else if (c->code == 'U' || c->code == 'A' || c->code == 'R' || c->code == 'C' || scm.initial)
        open_file(t, full); /* not in HEAD under this name: nothing to compare with */
    else
        compare_with_head(t, full);
    g_free(full);
}

/* Commits ---------------------------------------------------------------------------------------- */

static void on_commit_files(gboolean ok, const char *out, const char *err G_GNUC_UNUSED, gpointer data)
{
    char *commit = data;
    GtkListBoxRow *after = NULL;
    int n = 0;

    /* The commit's row: its files go right below it. */
    for (GtkWidget *w = gtk_widget_get_first_child(GTK_WIDGET(scm.commits_list)); w; w = gtk_widget_get_next_sibling(w))
        if (g_strcmp0(g_object_get_data(G_OBJECT(w), "commit"), commit) == 0)
            after = GTK_LIST_BOX_ROW(w);
    if (!ok || !after || g_strcmp0(scm.open_commit, commit) != 0) {
        g_free(commit);
        return;
    }
    int position = gtk_list_box_row_get_index(after) + 1;
    /* --name-status: "M<tab>path", renames and copies "R100<tab>old<tab>new". */
    char **lines = g_strsplit(out, "\n", -1);
    for (int i = 0; lines[i]; i++) {
        char **f = g_strsplit(lines[i], "\t", 3);
        if (g_strv_length(f) >= 2 && *f[0]) {
            char code = f[0][0];
            const char *path = g_strv_length(f) == 3 ? f[2] : f[1];
            const char *old_path = f[1];
            GtkWidget *box, *row = file_row(path, code, &box);
            gtk_widget_add_css_class(row, "file-row");
            gtk_box_append(GTK_BOX(box), code_label(code));
            g_object_set_data_full(G_OBJECT(row), "file-commit", g_strdup(commit), g_free);
            g_object_set_data_full(G_OBJECT(row), "file-new", code == 'D' ? NULL : g_strdup(path), g_free);
            g_object_set_data_full(G_OBJECT(row), "file-old", code == 'A' ? NULL : g_strdup(old_path), g_free);
            gtk_list_box_insert(scm.commits_list, row, position + n++);
        }
        g_strfreev(f);
    }
    g_strfreev(lines);
    g_free(commit);
}

static void remove_commit_files(void)
{
    GtkWidget *w = gtk_widget_get_first_child(GTK_WIDGET(scm.commits_list));

    while (w) {
        GtkWidget *next = gtk_widget_get_next_sibling(w);
        if (g_object_get_data(G_OBJECT(w), "file-commit"))
            gtk_list_box_remove(scm.commits_list, w);
        w = next;
    }
}

static void on_commit_activated(GtkListBox *list G_GNUC_UNUSED, GtkListBoxRow *row, gpointer data G_GNUC_UNUSED)
{
    const char *commit = g_object_get_data(G_OBJECT(row), "commit");
    const char *file_commit = g_object_get_data(G_OBJECT(row), "file-commit");

    if (file_commit) {
        /* What the commit did to the file: its own version next to its parent's (a first commit
         * has no parent, and that side stays empty). */
        char *parent = g_strconcat(file_commit, "^", NULL);
        compare_revisions(scm.app, parent, g_object_get_data(G_OBJECT(row), "file-old"), file_commit,
                          g_object_get_data(G_OBJECT(row), "file-new"));
        g_free(parent);
        return;
    }
    if (!commit)
        return;
    remove_commit_files();
    if (g_strcmp0(scm.open_commit, commit) == 0) { /* clicked again: fold it */
        g_clear_pointer(&scm.open_commit, g_free);
        return;
    }
    g_free(scm.open_commit);
    scm.open_commit = g_strdup(commit);
    const char *args[] = { "-c", "core.quotePath=false", "show", "--name-status", "-M", "--format=", commit, NULL };
    run_git(args, NULL, TRUE, on_commit_files, g_strdup(commit));
}

static void on_log(gboolean ok, const char *out, const char *err G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED)
{
    char **lines;

    clear_list(scm.commits_list);
    g_clear_pointer(&scm.open_commit, g_free);
    if (!ok)
        return;
    lines = g_strsplit(out, "\n", -1);
    for (int i = 0; lines[i]; i++) {
        char **f = g_strsplit(lines[i], "\x1f", 5); /* hash, short, subject, author, when */
        if (g_strv_length(f) == 5) {
            GtkWidget *row = gtk_list_box_row_new();
            GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
            GtkWidget *subject = gtk_label_new(f[2]);
            char *info = g_strdup_printf("%s · %s", f[1], f[4]);
            GtkWidget *detail = gtk_label_new(info);
            char *tooltip = g_strdup_printf("%s\n%s · %s · %s", f[2], f[1], f[3], f[4]);

            gtk_label_set_ellipsize(GTK_LABEL(subject), PANGO_ELLIPSIZE_END);
            gtk_label_set_xalign(GTK_LABEL(subject), 0);
            gtk_widget_set_hexpand(subject, TRUE);
            gtk_widget_add_css_class(detail, "dim");
            gtk_box_append(GTK_BOX(box), subject);
            gtk_box_append(GTK_BOX(box), detail);
            gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), open_button(row, box));
            gtk_widget_set_tooltip_text(row, tooltip);
            g_object_set_data_full(G_OBJECT(row), "commit", g_strdup(f[0]), g_free);
            gtk_list_box_append(scm.commits_list, row);
            g_free(tooltip);
            g_free(info);
        }
        g_strfreev(f);
    }
    g_strfreev(lines);
}

static void load_commits(void)
{
    const char *args[] = { "log", "-n", "100", "--format=%H%x1f%h%x1f%s%x1f%an%x1f%ar", NULL };

    run_git(args, NULL, TRUE, on_log, NULL);
}

/* Status ----------------------------------------------------------------------------------------- */

static void add_change(GPtrArray *list, const char *path, const char *old_path, char code)
{
    Change *c = g_new0(Change, 1);

    c->path = g_strdup(path);
    c->old_path = g_strdup(old_path);
    c->code = code;
    g_ptr_array_add(list, c);
}

static int compare_changes(gconstpointer a, gconstpointer b)
{
    return g_strcmp0((*(Change **)a)->path, (*(Change **)b)->path);
}

static void update_buttons(void)
{
    char *text;

    if (!scm.upstream)
        text = g_strdup("Publish Branch");
    else if (scm.behind)
        text = g_strdup_printf("Push ↑%d ↓%d", scm.ahead, scm.behind);
    else if (scm.ahead)
        text = g_strdup_printf("Push ↑%d", scm.ahead);
    else
        text = g_strdup("Push");
    gtk_button_set_label(GTK_BUTTON(scm.push), text);
    g_free(text);
    gtk_button_set_label(GTK_BUTTON(scm.commit), scm.staged->len == 0 && scm.changes->len > 0 ? "✓ Commit All"
                                                                                             : "✓ Commit");
}

/* Takes the output of "git status --porcelain=v2 --branch -z --untracked-files=all" (NULL: not a
 * repository, or git failed). */
void scm_update(App *t, const char *out, gsize size)
{
    char *head = NULL;
    char *title;

    if (!scm.app || t->closing)
        return;
    g_ptr_array_set_size(scm.staged, 0);
    g_ptr_array_set_size(scm.changes, 0);
    g_clear_pointer(&scm.upstream, g_free);
    scm.ahead = scm.behind = 0;
    scm.initial = FALSE;

    for (const char *p = out, *end = out + size; out && p < end;) {
        const char *nul = memchr(p, '\0', end - p);
        gsize len = (nul ? nul : end) - p;
        char *record = g_strndup(p, len);
        p += len + 1;

        if (g_str_has_prefix(record, "# branch.oid ")) {
            head = g_strdup(record + 13);
            scm.initial = strcmp(head, "(initial)") == 0;
        } else if (g_str_has_prefix(record, "# branch.upstream ")) {
            scm.upstream = g_strdup(record + 18);
        } else if (g_str_has_prefix(record, "# branch.ab ")) {
            sscanf(record + 12, "+%d -%d", &scm.ahead, &scm.behind);
        } else if (record[0] == '?' && len > 2) {
            add_change(scm.changes, record + 2, NULL, 'U');
        } else if ((record[0] == '1' || record[0] == '2' || record[0] == 'u') && len > 4) {
            /* "1 XY sub mH mI mW hH hI path", "2 ... Xscore path\0orig", "u XY sub m1 m2 m3 mW h1 h2 h3 path" */
            int skip = record[0] == '1' ? 8 : record[0] == '2' ? 9 : 10;
            char *path = record;
            for (int i = 0; i < skip && path; i++)
                path = strchr(path, ' ') ? strchr(path, ' ') + 1 : NULL;
            char *orig = NULL;
            if (record[0] == '2' && p < end) {
                const char *onul = memchr(p, '\0', end - p);
                gsize olen = (onul ? onul : end) - p;
                orig = g_strndup(p, olen);
                p += olen + 1;
            }
            if (path && record[0] == 'u') {
                add_change(scm.changes, path, NULL, '!');
            } else if (path) {
                char x = record[2], y = record[3];
                if (x != '.')
                    add_change(scm.staged, path, orig, x == 'T' ? 'M' : x);
                if (y != '.')
                    add_change(scm.changes, path, NULL, y == 'T' ? 'M' : y);
            }
            g_free(orig);
        }
        g_free(record);
    }
    g_ptr_array_sort(scm.staged, compare_changes);
    g_ptr_array_sort(scm.changes, compare_changes);
    fill_changes(scm.staged_list, scm.staged, TRUE);
    fill_changes(scm.changes_list, scm.changes, FALSE);
    gtk_widget_set_visible(scm.staged_box, scm.staged->len > 0);
    title = g_strdup_printf("CHANGES  %u", scm.changes->len);
    gtk_label_set_text(scm.changes_title, title);
    g_free(title);
    title = g_strdup_printf("STAGED CHANGES  %u", scm.staged->len);
    gtk_label_set_text(scm.staged_title, title);
    g_free(title);
    update_buttons();
    gtk_widget_set_sensitive(scm.commit, out != NULL);
    gtk_widget_set_sensitive(scm.push, out != NULL && !scm.initial);
    if (!out)
        set_note(t->git_top ? NULL : "This folder is not in a git repository.", FALSE);

    if (g_strcmp0(head, scm.head) != 0) { /* a commit, a checkout, another project */
        g_free(scm.head);
        scm.head = head;
        if (head && !scm.initial)
            load_commits();
        else
            clear_list(scm.commits_list);
    } else {
        g_free(head);
    }
}

/* Building the panel ------------------------------------------------------------------------------ */

static void on_message_changed(GtkTextBuffer *buffer, gpointer data G_GNUC_UNUSED)
{
    gtk_widget_set_visible(scm.placeholder, gtk_text_buffer_get_char_count(buffer) == 0);
}

static gboolean on_message_key(GtkEventControllerKey *controller G_GNUC_UNUSED, guint key, guint code G_GNUC_UNUSED,
                               GdkModifierType state, gpointer data G_GNUC_UNUSED)
{
    if ((key == GDK_KEY_Return || key == GDK_KEY_KP_Enter) && (state & GDK_CONTROL_MASK)) {
        commit();
        return TRUE;
    }
    return FALSE;
}

/* A section heading with buttons on its right; the list goes below it in the returned box. */
static GtkWidget *section(GtkLabel **title, const char *text, GtkListBox **list, GtkWidget *buttons[], int n)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *label = gtk_label_new(text);

    gtk_widget_add_css_class(head, "section");
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_box_append(GTK_BOX(head), label);
    for (int i = 0; i < n; i++)
        gtk_box_append(GTK_BOX(head), buttons[i]);
    gtk_box_append(GTK_BOX(box), head);
    *list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(*list, GTK_SELECTION_NONE);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(*list));
    if (title)
        *title = GTK_LABEL(label);
    return box;
}

GtkWidget *scm_panel(App *t)
{
    GtkCssProvider *provider = gtk_css_provider_new();
    GtkWidget *panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *title = gtk_label_new("SOURCE CONTROL");
    GtkWidget *overlay = gtk_overlay_new();
    GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkEventController *keys = gtk_event_controller_key_new();
    GtkWidget *unstage_all_button = icon_button("−", "Unstage All");
    GtkWidget *stage_all_button = icon_button("+", "Stage All");
    GtkWidget *refresh = icon_button("⟳", "Refresh");
    GtkWidget *staged_buttons[] = { unstage_all_button };
    GtkWidget *changes_buttons[] = { stage_all_button };
    GtkWidget *commits_buttons[] = { refresh };
    GtkTextBuffer *buffer;

    gtk_css_provider_load_from_string(provider, css);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);

    scm.app = t;
    scm.staged = g_ptr_array_new_with_free_func((GDestroyNotify)change_free);
    scm.changes = g_ptr_array_new_with_free_func((GDestroyNotify)change_free);
    gtk_widget_add_css_class(panel, "tcode-scm");

    gtk_widget_add_css_class(title, "title");
    gtk_label_set_xalign(GTK_LABEL(title), 0);
    gtk_box_append(GTK_BOX(content), title);

    /* The commit message, with a hint while it is empty. */
    scm.message = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(scm.message), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(scm.message), 6);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(scm.message), 4);
    gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(scm.message), 4);
    gtk_widget_set_size_request(scm.message, -1, 56);
    buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(scm.message));
    g_signal_connect(buffer, "changed", G_CALLBACK(on_message_changed), NULL);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_message_key), NULL);
    gtk_widget_add_controller(scm.message, keys);
    scm.placeholder = gtk_label_new("Message (Ctrl+Enter to commit)");
    gtk_widget_add_css_class(scm.placeholder, "placeholder");
    gtk_widget_set_halign(scm.placeholder, GTK_ALIGN_START);
    gtk_widget_set_valign(scm.placeholder, GTK_ALIGN_START);
    gtk_widget_set_can_target(scm.placeholder, FALSE);
    gtk_overlay_set_child(GTK_OVERLAY(overlay), scm.message);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), scm.placeholder);
    gtk_widget_add_css_class(overlay, "message");
    gtk_box_append(GTK_BOX(content), overlay);

    scm.commit = gtk_button_new_with_label("✓ Commit");
    gtk_widget_add_css_class(scm.commit, "primary");
    gtk_widget_set_hexpand(scm.commit, TRUE);
    gtk_widget_set_tooltip_text(scm.commit, "Commit the staged changes (all changes when none are staged)");
    g_signal_connect_swapped(scm.commit, "clicked", G_CALLBACK(commit), NULL);
    scm.push = gtk_button_new_with_label("Push");
    gtk_widget_set_tooltip_text(scm.push, "Push the commits to the remote");
    g_signal_connect_swapped(scm.push, "clicked", G_CALLBACK(push), NULL);
    gtk_widget_add_css_class(actions, "actions");
    gtk_box_append(GTK_BOX(actions), scm.commit);
    gtk_box_append(GTK_BOX(actions), scm.push);
    gtk_box_append(GTK_BOX(content), actions);

    scm.note = GTK_LABEL(gtk_label_new(NULL));
    gtk_widget_add_css_class(GTK_WIDGET(scm.note), "note");
    gtk_label_set_wrap(scm.note, TRUE);
    gtk_label_set_xalign(scm.note, 0);
    gtk_label_set_selectable(scm.note, TRUE);
    gtk_widget_set_visible(GTK_WIDGET(scm.note), FALSE);
    gtk_box_append(GTK_BOX(content), GTK_WIDGET(scm.note));

    g_signal_connect_swapped(unstage_all_button, "clicked", G_CALLBACK(unstage_all), NULL);
    g_signal_connect_swapped(stage_all_button, "clicked", G_CALLBACK(stage_all), NULL);
    g_signal_connect_swapped(refresh, "clicked", G_CALLBACK(refresh_branch), t);
    scm.staged_box = section(&scm.staged_title, "STAGED CHANGES", &scm.staged_list, staged_buttons, 1);
    scm.changes_box = section(&scm.changes_title, "CHANGES", &scm.changes_list, changes_buttons, 1);
    gtk_box_append(GTK_BOX(content), scm.staged_box);
    gtk_box_append(GTK_BOX(content), scm.changes_box);
    gtk_box_append(GTK_BOX(content), section(NULL, "COMMITS", &scm.commits_list, commits_buttons, 1));
    gtk_list_box_set_activate_on_single_click(scm.staged_list, TRUE);
    gtk_list_box_set_activate_on_single_click(scm.changes_list, TRUE);
    gtk_list_box_set_activate_on_single_click(scm.commits_list, TRUE);
    g_signal_connect(scm.staged_list, "row-activated", G_CALLBACK(on_change_activated), NULL);
    g_signal_connect(scm.changes_list, "row-activated", G_CALLBACK(on_change_activated), NULL);
    g_signal_connect(scm.commits_list, "row-activated", G_CALLBACK(on_commit_activated), NULL);
    gtk_widget_set_visible(scm.staged_box, FALSE);

    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), content);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(panel), scroll);
    return panel;
}

/* Another project: its commits are listed afresh. */
void scm_reset(void)
{
    g_clear_pointer(&scm.head, g_free);
    g_clear_pointer(&scm.open_commit, g_free);
    if (scm.message)
        gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(scm.message)), "", -1);
    if (scm.note)
        set_note(NULL, FALSE);
}
