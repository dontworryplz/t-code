/*
 * Comparing a file with an older version from git, side by side:
 *   left   the working file; its gutter marks what changed since that version
 *   right  the old version, read-only; its gutter marks what changed since then
 * Ctrl+Alt+D compares with the last commit (HEAD); Ctrl+Alt+H lists the file's commits to pick
 * one. When the old version's tab closes, the left gutter follows HEAD again.
 * The gutters come from micro (micro/plug/tcode: the tcodebase command, TCODE_BASE_* variables).
 */
#include "app.h"

#include <glib/gstdio.h>
#include <string.h>

/* The file the comparison commands work on: the current editor's, or for an old version, the
 * working file it is compared with. */
const char *comparable_path(App *t)
{
    Tab *tab = current_tab(t);

    if (!tab || tab->kind != TAB_EDITOR)
        return NULL;
    return tab->compare_with ? tab->compare_with : tab->path;
}

static void show_message(App *t, const char *message, const char *detail)
{
    GtkAlertDialog *dialog = gtk_alert_dialog_new("%s", message);

    gtk_alert_dialog_set_detail(dialog, detail);
    gtk_alert_dialog_show(dialog, t->window);
    g_object_unref(dialog);
}

/* Runs git in dir; returns its output, or NULL when it fails. */
static char *git(const char *dir, const char *const *args)
{
    GPtrArray *argv = g_ptr_array_new();
    char *out = NULL;
    int status;

    g_ptr_array_add(argv, "git");
    g_ptr_array_add(argv, "-C");
    g_ptr_array_add(argv, (char *)dir);
    for (int i = 0; args[i]; i++)
        g_ptr_array_add(argv, (char *)args[i]);
    g_ptr_array_add(argv, NULL);
    if (!g_spawn_sync(NULL, (char **)argv->pdata, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL,
                      NULL, NULL, &out, NULL, &status, NULL) ||
        !g_spawn_check_wait_status(status, NULL))
        g_clear_pointer(&out, g_free);
    g_ptr_array_free(argv, TRUE);
    return out;
}

/* Writes the file as it was in commit (a full hash) under the temp folder; returns its path. */
static char *write_old_version(App *t, const char *path, const char *commit, const char *short_commit)
{
    char *dir = g_path_get_dirname(path);
    char *name = g_path_get_basename(path);
    char *spec = g_strconcat(commit, ":./", name, NULL);
    const char *args[] = { "show", spec, NULL };
    char *content = git(dir, args);
    char *old_path = NULL;

    if (content) {
        GFile *root = g_file_new_for_path(t->root), *file = g_file_new_for_path(path);
        char *relative = g_file_get_relative_path(root, file);
        old_path = g_build_filename(t->temp_dir, short_commit, relative ? relative : name, NULL);
        char *old_dir = g_path_get_dirname(old_path);
        g_mkdir_with_parents(old_dir, 0700);
        g_file_set_contents(old_path, content, -1, NULL);
        g_free(old_dir);
        g_free(relative);
        g_object_unref(file);
        g_object_unref(root);
        g_free(content);
    }
    g_free(spec);
    g_free(name);
    g_free(dir);
    return old_path;
}

static void compare_with(App *t, const char *path, const char *rev)
{
    char *dir = g_path_get_dirname(path);
    char *name = g_path_get_basename(path);
    const char *full_args[] = { "rev-parse", "--verify", "--quiet", rev, NULL };
    char *commit = git(dir, full_args);
    char *short_commit = NULL, *old_path = NULL;

    if (!commit) {
        show_message(t, "No git history", "This file is not in a git repository with commits.");
        goto out;
    }
    g_strstrip(commit);
    short_commit = g_strndup(commit, 7);
    old_path = write_old_version(t, path, commit, short_commit);
    if (!old_path) {
        char *detail = g_strdup_printf("%s is not in commit %s (it may be new).", name, short_commit);
        show_message(t, "Nothing to compare with", detail);
        g_free(detail);
        goto out;
    }

    /* Left: the working file, compared with the commit. */
    Tab *work = find_tab(t, TAB_EDITOR, path);
    if (work) {
        char *command = g_strdup_printf("tcodebase rev %s", commit);
        if (tab_group(work) == 1)
            move_tab(work, 0);
        editor_command(work, command);
        g_free(command);
    } else {
        char *env_rev = g_strconcat("TCODE_BASE_REV=", commit, NULL);
        char *env[] = { env_rev, NULL };
        t->active = 0;
        work = open_editor(t, path, 0, 0, FALSE, env);
        g_free(env_rev);
    }

    /* Right: the old version, compared with the working file. */
    Tab *old = find_tab(t, TAB_EDITOR, old_path);
    if (!old) {
        char *env_file = g_strconcat("TCODE_BASE_FILE=", path, NULL);
        /* No clangd for the copy: away from the project's build setup it would only report noise. */
        char *env[] = { env_file, "MICRO_LSP=none=none", NULL };
        set_split(t, TRUE);
        t->active = 1;
        old = open_editor(t, old_path, work->line, 1, TRUE, env);
        old->title = g_strdup_printf("%s @ %s", name, short_commit);
        old->compare_with = g_strdup(path);
        update_tab_label(old);
        g_free(env_file);
    } else if (tab_group(old) == 0) {
        set_split(t, TRUE);
        move_tab(old, 1);
    }
    select_tab(old);
    select_tab(work);

out:
    g_free(old_path);
    g_free(short_commit);
    g_free(commit);
    g_free(name);
    g_free(dir);
}

void compare_with_head(App *t, const char *path)
{
    compare_with(t, path, "HEAD");
}

/* An old version's tab closed: unless another old version still compares with the same file,
 * its gutter goes back to following HEAD. */
void forget_comparison(Tab *old_version)
{
    App *t = old_version->app;
    Tab *work = find_tab(t, TAB_EDITOR, old_version->compare_with);
    int n = tab_count(t);

    for (int i = 0; i < n; i++) {
        Tab *other = tab_nth(t, i);
        if (other != old_version && g_strcmp0(other->compare_with, old_version->compare_with) == 0)
            return;
    }
    if (work)
        editor_command(work, "tcodebase");
}

/* Closes the old versions of the current file (their editors are read-only, so nothing is lost). */
void stop_comparing(App *t)
{
    const char *path = comparable_path(t);
    int n = tab_count(t);
    gboolean closed = FALSE;

    if (!path)
        return;
    char *working = g_strdup(path);
    for (int i = 0; i < n; i++) {
        Tab *tab = tab_nth(t, i);
        if (g_strcmp0(tab->compare_with, working) == 0) {
            vte_terminal_feed_child(tab->term, "\x11", 1); /* Ctrl+Q */
            closed = TRUE;
        }
    }
    if (!closed) {
        Tab *work = find_tab(t, TAB_EDITOR, working);
        if (work)
            editor_command(work, "tcodebase");
    }
    g_free(working);
}

/* File history --------------------------------------------------------------------------------- */

static char *history_path;
static GPtrArray *history; /* "hash \x1f short \x1f date \x1f author \x1f subject" */

static void history_query(App *t, const char *text, guint generation)
{
    int shown = 0;

    for (guint i = 0; i < history->len; i++) {
        char **f = g_strsplit(g_ptr_array_index(history, i), "\x1f", 5);
        if (g_strv_length(f) == 5) {
            char *haystack = g_strjoin(" ", f[4], f[1], f[3], f[2], NULL);
            if (!*text || fuzzy_score(text, haystack) >= 0) {
                char *detail = g_strdup_printf("%s · %s · %s", f[1], f[2], f[3]);
                picker_add(t, generation, f[4], detail, f[0]);
                g_free(detail);
                shown++;
            }
            g_free(haystack);
        }
        g_strfreev(f);
    }
    if (!shown)
        picker_message(t, generation, history->len ? "No matching commits" : "This file has no commits yet");
}

static void history_choose(App *t, const char *commit)
{
    compare_with(t, history_path, commit);
}

void file_history(App *t, const char *path)
{
    char *dir = g_path_get_dirname(path);
    char *name = g_path_get_basename(path);
    const char *args[] = { "log", "--follow", "-n", "500", "--date=format:%Y-%m-%d %H:%M",
                           "--format=%H%x1f%h%x1f%ad%x1f%an%x1f%s", "--", name, NULL };
    char *out = git(dir, args);
    char *title;

    if (!out) {
        show_message(t, "No git history", "This file is not in a git repository with commits.");
        g_free(name);
        g_free(dir);
        return;
    }
    g_free(history_path);
    history_path = g_strdup(path);
    if (!history)
        history = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_set_size(history, 0);
    char **lines = g_strsplit(out, "\n", -1);
    for (int i = 0; lines[i]; i++)
        if (*lines[i])
            g_ptr_array_add(history, g_strdup(lines[i]));
    g_strfreev(lines);

    title = g_strdup_printf("History of %s: pick a commit to compare with", name);
    picker_open(t, title, history_query, history_choose);
    g_free(title);
    g_free(out);
    g_free(name);
    g_free(dir);
}

/* Temp files --------------------------------------------------------------------------------- */

static void delete_tree(const char *path)
{
    GDir *dir = g_dir_open(path, 0, NULL);
    const char *name;

    while (dir && (name = g_dir_read_name(dir))) {
        char *child = g_build_filename(path, name, NULL);
        if (g_file_test(child, G_FILE_TEST_IS_DIR) && !g_file_test(child, G_FILE_TEST_IS_SYMLINK))
            delete_tree(child);
        else
            g_remove(child);
        g_free(child);
    }
    if (dir)
        g_dir_close(dir);
    g_rmdir(path);
}

void remove_temp_dir(App *t)
{
    if (t->temp_dir)
        delete_tree(t->temp_dir);
}
