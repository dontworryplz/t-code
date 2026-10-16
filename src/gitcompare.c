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

/* Where an old version of path goes under the temp folder's subfolder: by its place in the work
 * tree (or the project, or else by its full path), so two files with one name never share it. */
static char *temp_path(App *t, const char *folder, const char *path)
{
    GFile *top = g_file_new_for_path(t->git_top ? t->git_top : t->root), *file = g_file_new_for_path(path);
    char *relative = g_file_get_relative_path(top, file);
    char *result = g_build_filename(t->temp_dir, folder, relative ? relative : path, NULL);

    g_free(relative);
    g_object_unref(file);
    g_object_unref(top);
    return result;
}

static void store(const char *path, const char *content)
{
    char *dir = g_path_get_dirname(path);

    g_mkdir_with_parents(dir, 0700);
    g_file_set_contents(path, content, -1, NULL);
    g_free(dir);
}

/* Writes what git has for spec ("<commit>:./<name>", ":./<name>" for the index), run in path's
 * folder, as the old version of path under the temp folder's subfolder; returns its path. */
static char *write_old_version(App *t, const char *path, const char *spec, const char *folder)
{
    char *dir = g_path_get_dirname(path);
    const char *args[] = { "show", spec, NULL };
    char *content = git(dir, args);
    char *old_path = NULL;

    if (content) {
        old_path = temp_path(t, folder, path);
        store(old_path, content);
        g_free(content);
    }
    g_free(dir);
    return old_path;
}

/* "tcodebase file <path>", quoted for micro's command line. */
static char *base_file_command(const char *path)
{
    char *quoted = g_shell_quote(path);
    char *command = g_strconcat("tcodebase file ", quoted, NULL);

    g_free(quoted);
    return command;
}

/* A tab that shows an old version already: its file may have been written again (the index
 * changes), and what its gutter compares with may be different now. */
static void refresh_version(Tab *tab, const char *base)
{
    editor_command(tab, "reopen");
    if (base) {
        char *command = base_file_command(base);
        editor_command(tab, command);
        g_free(command);
    }
}

/* Closes the old versions compared with path, except keep: one comparison per file at a time, or
 * a forgotten one would keep path's gutter from going back to HEAD when the newer one closes. */
static void close_comparisons(App *t, const char *path, const char *keep)
{
    int n = tab_count(t);

    for (int i = 0; i < n; i++) {
        Tab *tab = tab_nth(t, i);
        if (g_strcmp0(tab->compare_with, path) == 0 && g_strcmp0(tab->path, keep) != 0)
            editor_command(tab, "quit"); /* read-only: nothing to save */
    }
}

/* The working file on the left, its gutter compared with the commit base_rev (by name; it follows
 * new commits when that is HEAD) or else with old_path; on the right old_path read-only ("name @
 * label"), its gutter compared with the working file. */
static void compare_working(App *t, const char *path, const char *old_path, const char *label, const char *base_rev)
{
    char *name = g_path_get_basename(path);
    Tab *work = find_tab(t, TAB_EDITOR, path);
    Tab *old = find_tab(t, TAB_EDITOR, old_path);

    close_comparisons(t, path, old_path);
    if (work) {
        char *command = base_rev ? g_strdup_printf("tcodebase rev %s", base_rev) : base_file_command(old_path);
        if (tab_group(work) == 1)
            move_tab(work, 0);
        editor_command(work, command);
        g_free(command);
    } else {
        char *env_base = base_rev ? g_strconcat("TCODE_BASE_REV=", base_rev, NULL)
                                  : g_strconcat("TCODE_BASE_FILE=", old_path, NULL);
        char *env[] = { env_base, NULL };
        t->active = 0;
        work = open_editor(t, path, 0, 0, FALSE, env);
        g_free(env_base);
    }

    if (!old) {
        char *env_file = g_strconcat("TCODE_BASE_FILE=", path, NULL);
        /* No clangd for the copy: away from the project's build setup it would only report noise. */
        char *env[] = { env_file, "MICRO_LSP=none=none", NULL };
        set_split(t, TRUE);
        t->active = 1;
        old = open_editor(t, old_path, work->line, 1, TRUE, env);
        old->title = g_strdup_printf("%s @ %s", name, label);
        old->compare_with = g_strdup(path);
        update_tab_label(old);
        g_free(env_file);
    } else {
        if (tab_group(old) == 0) {
            set_split(t, TRUE);
            move_tab(old, 1);
        }
        refresh_version(old, path);
    }
    select_tab(old);
    select_tab(work);
    g_free(name);
}

static void compare_with(App *t, const char *path, const char *rev)
{
    char *dir = g_path_get_dirname(path);
    char *name = g_path_get_basename(path);
    const char *full_args[] = { "rev-parse", "--verify", "--quiet", rev, NULL };
    char *commit = git(dir, full_args);
    char *short_commit = NULL, *spec = NULL, *old_path = NULL;

    if (!commit) {
        show_message(t, "No git history", "This file is not in a git repository with commits.");
        goto out;
    }
    g_strstrip(commit);
    short_commit = g_strndup(commit, 7);
    spec = g_strconcat(commit, ":./", name, NULL);
    old_path = write_old_version(t, path, spec, short_commit);
    if (!old_path) {
        char *detail = g_strdup_printf("%s is not in commit %s (it may be new).", name, short_commit);
        show_message(t, "Nothing to compare with", detail);
        g_free(detail);
        goto out;
    }
    compare_working(t, path, old_path, short_commit, commit);

out:
    g_free(old_path);
    g_free(spec);
    g_free(short_commit);
    g_free(commit);
    g_free(name);
    g_free(dir);
}

void compare_with_head(App *t, const char *path)
{
    compare_with(t, path, "HEAD");
}

/* The working file next to its staged version (the index): what is not staged yet. */
void compare_with_index(App *t, const char *path)
{
    char *name = g_path_get_basename(path);
    char *spec = g_strconcat(":./", name, NULL);
    char *old_path = write_old_version(t, path, spec, "index");

    if (old_path)
        compare_working(t, path, old_path, "Index", NULL);
    else
        show_message(t, "Nothing to compare with", "This file is not staged.");
    g_free(old_path);
    g_free(spec);
    g_free(name);
}

/* Two versions from git -------------------------------------------------------------------------- */

/* Writes rel (relative to the work tree top) as it was in rev (INDEX_REV: as staged) under the
 * temp folder; returns its path, and the short commit (or "Index") in *label. */
static char *write_revision(App *t, const char *rev, const char *rel, char **label)
{
    gboolean index = strcmp(rev, INDEX_REV) == 0;
    const char *short_args[] = { "rev-parse", "--short", "--verify", "--quiet", rev, NULL };
    char *short_commit = index ? g_strdup("Index") : git(t->git_top, short_args);
    char *spec = g_strconcat(rev, ":", rel, NULL);
    const char *show_args[] = { "show", spec, NULL };
    char *content = short_commit ? git(t->git_top, show_args) : NULL;
    char *path = NULL;

    if (content) {
        g_strstrip(short_commit);
        path = g_build_filename(t->temp_dir, index ? "index" : short_commit, rel, NULL);
        store(path, content);
        *label = g_strdup(short_commit);
    }
    g_free(content);
    g_free(spec);
    g_free(short_commit);
    return path;
}

/* Opens a read-only old version in group (titled "name @ label"), its gutter compared with base
 * (NULL: no gutter marks); reuses a tab that shows it already. */
static Tab *open_version(App *t, const char *path, const char *label, const char *base, const char *working,
                         int group)
{
    Tab *tab = find_tab(t, TAB_EDITOR, path);

    if (!tab) {
        char *env_file = base ? g_strconcat("TCODE_BASE_FILE=", base, NULL) : NULL;
        char *env[] = { "MICRO_LSP=none=none", env_file, NULL };
        char *name = g_path_get_basename(working);
        t->active = group;
        tab = open_editor(t, path, 0, 0, TRUE, env);
        tab->title = g_strdup_printf("%s @ %s", name, label);
        tab->compare_with = g_strdup(working);
        update_tab_label(tab);
        g_free(name);
        g_free(env_file);
        return tab;
    }
    if (tab_group(tab) != group)
        move_tab(tab, group);
    refresh_version(tab, base);
    return tab;
}

/* Shows a file in two versions side by side: new_rev's on the left, old_rev's on the right, each
 * gutter marking what differs from the other. Paths are relative to the work tree top (they differ
 * after a rename); a NULL rev or path is a missing side (the file was added or deleted). */
void compare_revisions(App *t, const char *old_rev, const char *old_rel, const char *new_rev, const char *new_rel)
{
    char *old_label = NULL, *new_label = NULL;
    char *old_path = old_rev && old_rel && t->git_top ? write_revision(t, old_rev, old_rel, &old_label) : NULL;
    char *new_path = new_rev && new_rel && t->git_top ? write_revision(t, new_rev, new_rel, &new_label) : NULL;
    char *working = t->git_top ? g_build_filename(t->git_top, new_rel ? new_rel : old_rel, NULL) : NULL;
    Tab *left = NULL, *right = NULL;

    if (!old_path && !new_path) {
        show_message(t, "Nothing to compare", "git has no such version of this file.");
    } else {
        if (old_path && new_path)
            set_split(t, TRUE);
        if (new_path)
            left = open_version(t, new_path, new_label, old_path, working, 0);
        if (old_path)
            right = open_version(t, old_path, old_label, new_path, working, new_path ? 1 : 0);
        if (right)
            select_tab(right);
        if (left)
            select_tab(left);
    }
    g_free(working);
    g_free(new_path);
    g_free(old_path);
    g_free(new_label);
    g_free(old_label);
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
