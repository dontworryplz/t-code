/*
 * Comparing a file with an older version from git, side by side in one tab:
 *   left   the working file; its gutter marks what changed since that version
 *   right  the old version, read-only; its gutter marks what changed since then
 * Ctrl+Alt+D compares with the last commit (HEAD); Ctrl+Alt+H lists the file's commits to pick
 * one. Closing the tab closes both; "Git: Stop Comparing" keeps the working file, its gutter
 * following HEAD again.
 */
#include "git/compare.h"

#include "editor/diffgutter.h"
#include "editor/editor.h"
#include "editor/tabs.h"
#include "popups/picker.h"

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

/* A message box over the window ("No git history", "Nothing to compare with"). */
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
  if (!g_spawn_sync(NULL, (char **)argv->pdata, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL,
                    &out, NULL, &status, NULL) ||
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

/* Writes content to path, making its folders first. */
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

static void on_vscroll(GtkAdjustment *src, GtkScrollable *dst)
{
  GtkAdjustment *target = gtk_scrollable_get_vadjustment(dst);
  double val = gtk_adjustment_get_value(src);
  double diff = target ? gtk_adjustment_get_value(target) - val : 0.0;

  if (target && (diff > 0.5 || diff < -0.5))
    gtk_adjustment_set_value(target, val);
}

static void on_hscroll(GtkAdjustment *src, GtkScrollable *dst)
{
  GtkAdjustment *target = gtk_scrollable_get_hadjustment(dst);
  double val = gtk_adjustment_get_value(src);
  double diff = target ? gtk_adjustment_get_value(target) - val : 0.0;

  if (target && (diff > 0.5 || diff < -0.5))
    gtk_adjustment_set_value(target, val);
}

/* Once laid out, the right side starts where the left one is (it opens at the top). */
static void on_right_mapped(GtkWidget *right, GtkScrollable *left)
{
  GtkAdjustment *from = gtk_scrollable_get_vadjustment(left);

  g_signal_handlers_disconnect_by_func(right, on_right_mapped, left);
  if (from)
    on_vscroll(from, GTK_SCROLLABLE(right));
}

/* Keeps a comparison's two sides scrolling together vertically and horizontally. */
static void sync_scroll(Tab *left, Tab *right)
{
  GtkScrollable *a = GTK_SCROLLABLE(left->source), *b = GTK_SCROLLABLE(right->source);

  g_signal_connect_object(right->source, "map", G_CALLBACK(on_right_mapped), a, G_CONNECT_AFTER);
  g_signal_connect_object(gtk_scrollable_get_vadjustment(a), "value-changed", G_CALLBACK(on_vscroll), b, 0);
  g_signal_connect_object(gtk_scrollable_get_vadjustment(b), "value-changed", G_CALLBACK(on_vscroll), a, 0);
  g_signal_connect_object(gtk_scrollable_get_hadjustment(a), "value-changed", G_CALLBACK(on_hscroll), b, 0);
  g_signal_connect_object(gtk_scrollable_get_hadjustment(b), "value-changed", G_CALLBACK(on_hscroll), a, 0);
}

void unsync_scroll(Tab *left)
{
  Tab *sides[] = { left, left->old_side };

  for (gsize i = 0; i < G_N_ELEMENTS(sides); i++) {
    if (!sides[i] || !sides[i]->source)
      continue;
    GtkAdjustment *adjustments[] = { gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(sides[i]->source)),
                                     gtk_scrollable_get_hadjustment(GTK_SCROLLABLE(sides[i]->source)) };
    for (gsize k = 0; k < G_N_ELEMENTS(adjustments); k++) {
      if (!adjustments[k])
        continue;
      g_signal_handlers_disconnect_matched(adjustments[k], G_SIGNAL_MATCH_FUNC, 0, 0, NULL, on_vscroll, NULL);
      g_signal_handlers_disconnect_matched(adjustments[k], G_SIGNAL_MATCH_FUNC, 0, 0, NULL, on_hscroll, NULL);
    }
  }
}

/* Opens path (an old version, read-only) in a tab of the active group titled "name @ label", compared
 * with working; a tab showing it already goes first (the file may have been written again). */
static Tab *open_version(App *t, const char *path, const char *label, const char *working)
{
  char *name = g_path_get_basename(working);
  Tab *tab;

  for (int i = tab_count(t) - 1; i >= 0; i--) {
    Tab *other = tab_nth(t, i);
    if (g_strcmp0(other->path, path) == 0)
      remove_tab(other); /* an old version: nothing to save */
    else if (other->old_side && g_strcmp0(other->old_side->path, path) == 0)
      stop_comparing_tab(other);
  }
  tab = open_editor(t, path, 0, 0, TRUE);
  tab->title = g_strdup_printf("%s @ %s", name, label);
  tab->compare_with = g_strdup(working);
  update_tab_label(tab);
  g_free(name);
  return tab;
}

/* left and right in one tab, each gutter marking what differs from the other, scrolling together. */
static void show_side_by_side(Tab *left, Tab *right)
{
  join_tabs(left, right);
  diff_gutter_pair(left, right);
  sync_scroll(left, right);
  select_tab(left);
}

/* The working file next to old_path (read-only, "name @ label") in one tab: the working file's
 * gutter compared with the commit base_rev (by name; it follows new commits when that is HEAD) or
 * else with old_path, the old version's with the working file. */
static void compare_working(App *t, const char *path, const char *old_path, const char *label, const char *base_rev)
{
  Tab *work = find_tab(t, TAB_EDITOR, path), *old;

  if (!work)
    work = open_editor(t, path, 0, 0, FALSE);
  stop_comparing_tab(work); /* one comparison per file at a time */
  keep_tab(work);           /* a click in Files must not replace it while it is compared */
  t->active = tab_group(work);
  old = open_version(t, old_path, label, path);
  if (base_rev && strcmp(base_rev, "HEAD") != 0)
    diff_gutter_compare_rev(work, base_rev);
  else if (base_rev)
    diff_gutter_follow_head(work);
  else
    diff_gutter_compare_file(work, old_path);
  diff_gutter_compare_file(old, path);
  show_side_by_side(work, old);
}

/* path next to its version in rev (a commit, "HEAD"): compare_with_head() and the File History
 * picker. */
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

/* Two versions from git ---------------------------------------------------------------------- */

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

/* Shows a file in two versions side by side in one tab: new_rev's on the left, old_rev's on the
 * right, each gutter marking what differs from the other. Paths are relative to the work tree top
 * (they differ after a rename); a NULL rev or path is a missing side (the file was added or
 * deleted): the other one shows alone. */
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
    if (new_path)
      left = open_version(t, new_path, new_label, working);
    if (old_path)
      right = open_version(t, old_path, old_label, working);
    if (left)
      diff_gutter_compare_file(left, old_path); /* NULL: the other side is missing, no marks */
    if (right)
      diff_gutter_compare_file(right, new_path);
    if (left && right)
      show_side_by_side(left, right);
    else
      select_tab(left ? left : right);
  }
  g_free(working);
  g_free(new_path);
  g_free(old_path);
  g_free(new_label);
  g_free(old_label);
}

void stop_comparing_tab(Tab *tab)
{
  if (!tab->old_side)
    return;
  part_tabs(tab);
  if (!tab->compare_with) { /* the working file: its gutter follows HEAD again */
    diff_gutter_pair(tab, NULL);
    diff_gutter_follow_head(tab);
  }
}

/* Ends the current tab's comparison: an old version closes, the working file stays. */
void stop_comparing(App *t)
{
  Tab *tab = current_tab(t);

  if (!tab || tab->kind != TAB_EDITOR)
    return;
  if (tab->compare_with) /* two old versions, or one: nothing to keep */
    remove_tab(tab);
  else
    stop_comparing_tab(tab);
}

/* File history ------------------------------------------------------------------------------- */

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

/* A commit was picked in File History: compare the file with it. */
static void history_choose(App *t, const char *commit)
{
  compare_with(t, history_path, commit);
}

void file_history(App *t, const char *path)
{
  char *dir = g_path_get_dirname(path);
  char *name = g_path_get_basename(path);
  const char *args[] = {
    "log", "--follow", "-n", "500", "--date=format:%Y-%m-%d %H:%M", "--format=%H%x1f%h%x1f%ad%x1f%an%x1f%s",
    "--",  name,       NULL
  };
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
