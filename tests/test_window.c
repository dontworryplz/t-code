#define _DEFAULT_SOURCE /* kill() under -std=c23 with musl */
/*
 * Tests for how tabs open and close, in a whole t-code window (every module but main.c) on a
 * temporary project with a temporary config: temporary tabs (a click in the Files panel, VS Code's
 * preview editor) and comparisons in one tab. Needs a display; skipped without one.
 */
#include "editor/editor.h"
#include "editor/tabs.h"
#include "files/fileops.h"
#include "git/compare.h"
#include "ipc/socket.h"
#include "project/session.h"
#include "window/window.h"

#include <glib/gstdio.h>
#include <signal.h>

static App app;

static char *file(const char *name)
{
  char *path = g_build_filename(app.root, name, NULL);

  if (!g_file_test(path, G_FILE_TEST_EXISTS))
    g_assert_true(g_file_set_contents(path, "one\ntwo\n", -1, NULL));
  return path;
}

/* Lets the main loop run for ms milliseconds (files load, idles run). */
static void run(int ms)
{
  gint64 end = g_get_monotonic_time() + ms * 1000;

  while (g_get_monotonic_time() < end)
    g_main_context_iteration(NULL, FALSE);
}

/* The open tabs' file names in order, "*" after temporary ones: "a.txt* b.txt". */
static char *tabs(void)
{
  GString *text = g_string_new(NULL);

  for (int i = 0; i < tab_count(&app); i++) {
    Tab *tab = tab_nth(&app, i);
    char *name = g_path_get_basename(tab->path);
    g_string_append_printf(text, "%s%s%s", i ? " " : "", name, tab->temporary ? "*" : "");
    g_free(name);
  }
  return g_string_free(text, FALSE);
}

static void assert_tabs(const char *want)
{
  char *now = tabs();

  g_assert_cmpstr(now, ==, want);
  g_free(now);
}

static void close_everything(void)
{
  close_all_tabs(&app);
  set_split(&app, FALSE);
  run(50);
}

static void open_temporary(const char *name)
{
  char *path = file(name);

  open_file_temporary(&app, path);
  g_free(path);
  run(20);
}

static void open_kept(const char *name)
{
  char *path = file(name);

  open_file(&app, path);
  g_free(path);
  run(20);
}

/* A click replaces the last click's tab, in its place; opening a file for good, or editing it, keeps
 * its tab. */
static void test_temporary(void)
{
  open_temporary("a.txt");
  assert_tabs("a.txt*");
  open_temporary("b.txt");
  assert_tabs("b.txt*");
  open_kept("b.txt"); /* a double click */
  assert_tabs("b.txt");
  open_temporary("c.txt");
  assert_tabs("b.txt c.txt*");

  open_kept("d.txt");
  select_tab(tab_nth(&app, 1)); /* c.txt; d.txt after it */
  open_temporary("e.txt");
  assert_tabs("b.txt e.txt* d.txt"); /* where c.txt was */
  open_temporary("b.txt");           /* already open: shown, nothing replaced */
  assert_tabs("b.txt e.txt* d.txt");

  gtk_text_buffer_insert_at_cursor(GTK_TEXT_BUFFER(tab_nth(&app, 1)->buffer), "x", -1); /* edited */
  assert_tabs("b.txt e.txt d.txt");
  gtk_text_buffer_set_modified(GTK_TEXT_BUFFER(tab_nth(&app, 1)->buffer), FALSE); /* nothing to ask */
  open_temporary("f.txt"); /* after the current tab, b.txt (shown just before) */
  assert_tabs("b.txt f.txt* e.txt d.txt");
  keep_tab(tab_nth(&app, 1)); /* a double click on its tab */
  assert_tabs("b.txt f.txt e.txt d.txt");
  close_everything();
}

/* Each group has its own temporary tab. */
static void test_temporary_per_group(void)
{
  open_temporary("a.txt");
  set_split(&app, TRUE); /* the right group is active now */
  open_temporary("b.txt");
  assert_tabs("a.txt* b.txt*");
  open_temporary("c.txt");
  assert_tabs("a.txt* c.txt*");
  g_assert_cmpint(tab_group(tab_nth(&app, 1)), ==, 1);
  close_everything();
}

static gboolean git(const char *const *args)
{
  GPtrArray *argv = g_ptr_array_new();
  gboolean ok;
  int status = 0;

  g_ptr_array_add(argv, "git");
  for (int i = 0; args[i]; i++)
    g_ptr_array_add(argv, (gpointer)args[i]);
  g_ptr_array_add(argv, NULL);
  ok = g_spawn_sync(app.root, (char **)argv->pdata, NULL,
                    G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL,
                    NULL, &status, NULL) &&
       g_spawn_check_wait_status(status, NULL);
  g_ptr_array_free(argv, TRUE);
  return ok;
}

/* A comparison is one tab with both sides ("tracked.txt ↔ <commit>"), on the left the working file:
 * Stop Comparing closes the old version and keeps the file; closing the tab closes both. */
static void test_comparison_one_tab(void)
{
  char *path = file("tracked.txt");
  Tab *tab;

  if (!git((const char *[]){ "init", "-q", NULL }) || !git((const char *[]){ "add", "tracked.txt", NULL }) ||
      !git((const char *[]){ "-c", "user.name=t", "-c", "user.email=t@t", "commit", "-qm", "one", NULL })) {
    g_test_skip("no git");
    g_free(path);
    return;
  }
  g_assert_true(g_file_set_contents(path, "one\nTWO\n", -1, NULL));
  app.git_top = g_canonicalize_filename(app.root, NULL);

  open_temporary("tracked.txt");
  compare_with_head(&app, path);
  run(50);
  assert_tabs("tracked.txt"); /* one tab, kept (no longer temporary) */
  tab = tab_nth(&app, 0);
  g_assert_nonnull(tab->old_side);
  g_assert_cmpstr(tab->old_side->compare_with, ==, path);
  g_assert_true(g_str_has_prefix(gtk_label_get_text(tab->label), "tracked.txt ↔ "));
  g_assert_false(gtk_widget_get_visible(GTK_WIDGET(app.groups[1]))); /* no second group */

  stop_comparing(&app); /* the file stays */
  run(50);
  assert_tabs("tracked.txt");
  g_assert_null(tab_nth(&app, 0)->old_side);
  g_assert_cmpstr(gtk_label_get_text(tab_nth(&app, 0)->label), ==, "tracked.txt");

  compare_with_head(&app, path);
  run(50);
  compare_with_head(&app, path); /* again: still one comparison */
  run(50);
  assert_tabs("tracked.txt");
  quit_editor(tab_nth(&app, 0)); /* closes both */
  run(50);
  assert_tabs("");
  g_free(path);
}

/* The old version shown next to path in its comparison tab, or NULL. */
static Tab *old_version_of(const char *path)
{
  for (int i = 0; i < tab_count(&app); i++) {
    Tab *tab = tab_nth(&app, i);
    if (g_strcmp0(tab->path, path) == 0 && tab->old_side)
      return tab->old_side;
  }
  return NULL;
}

/* Clicking changed files one after the other in Source Control: each one opens with its old version
 * in its tab, every time. */
static void test_comparisons_in_a_row(void)
{
  const char *names[] = { "r1.txt", "r2.txt", "r3.txt", "r4.txt", "r5.txt", "r6.txt" };
  char *paths[G_N_ELEMENTS(names)];

  for (size_t i = 0; i < G_N_ELEMENTS(names); i++) {
    paths[i] = file(names[i]);
    if (!git((const char *[]){ "add", names[i], NULL })) {
      g_test_skip("no git");
      return;
    }
  }
  if (!git((const char *[]){ "-c", "user.name=t", "-c", "user.email=t@t", "commit", "-qm", "rows", NULL })) {
    g_test_skip("no git");
    return;
  }
  g_set_str(&app.git_top, NULL);
  app.git_top = g_canonicalize_filename(app.root, NULL);
  for (size_t i = 0; i < G_N_ELEMENTS(names); i++)
    g_assert_true(g_file_set_contents(paths[i], "one\nchanged\n", -1, NULL));

  for (int round = 0; round < 2; round++)
    for (size_t i = 0; i < G_N_ELEMENTS(names); i++) {
      compare_with_head(&app, paths[i]);
      run(50);
      if (!old_version_of(paths[i])) {
        char *now = tabs();
        g_test_message("round %d, %s: tabs %s", round, names[i], now);
        g_free(now);
      }
      g_assert_nonnull(old_version_of(paths[i]));
    }
  close_everything();
  for (size_t i = 0; i < G_N_ELEMENTS(names); i++)
    g_free(paths[i]);
}

/* A file opens at its left edge, also in a narrow view without word wrap (the cursor's reveal used to
 * scroll sideways by a few pixels, cutting off every line's first letters). */
static void test_opens_at_left_edge(void)
{
  char *path = g_build_filename(app.root, "long.txt", NULL);
  GString *text = g_string_new(NULL);
  gboolean wrap = app.settings.word_wrap;
  GtkAdjustment *sideways;

  for (int i = 0; i < 40; i++)
    g_string_append(text, "a line much longer than the view is wide, so that it could scroll sideways\n");
  g_assert_true(g_file_set_contents(path, text->str, -1, NULL));
  app.settings.word_wrap = FALSE;
  open_file(&app, path);
  run(300);
  sideways = gtk_scrollable_get_hadjustment(GTK_SCROLLABLE(tab_nth(&app, 0)->source));
  g_assert_cmpfloat(gtk_adjustment_get_value(sideways), ==, 0);
  close_everything();
  app.settings.word_wrap = wrap;
  g_string_free(text, TRUE);
  g_free(path);
}

static GPid broadway;

/* The window is shown (build_window presents it): on GTK's Broadway backend, nothing appears on the
 * screen. Without gtk4-broadwayd it opens on the desktop for a moment. */
static void start_broadway(void)
{
  char *display = g_strdup_printf(":%d", 50 + g_random_int_range(0, 50));
  char *argv[] = { "gtk4-broadwayd", display, NULL };

  if (g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                    NULL, NULL, &broadway, NULL)) {
    g_setenv("GDK_BACKEND", "broadway", TRUE);
    g_setenv("BROADWAY_DISPLAY", display, TRUE);
    g_usleep(300 * 1000); /* until it listens */
  }
  g_free(display);
}

static void on_activate(GApplication *gapp, gpointer data G_GNUC_UNUSED)
{
  int *status = g_object_get_data(G_OBJECT(gapp), "status");

  build_window(&app, GTK_APPLICATION(gapp));
  run(100);
  *status = g_test_run();
  app.closing = TRUE;
  gtk_window_destroy(app.window);
}

int main(int argc, char **argv)
{
  char *home = g_dir_make_tmp("t-code-window-XXXXXX", NULL);
  char *config = g_build_filename(home, "config", NULL);
  GtkApplication *gapp;
  int status = 0;

  g_setenv("XDG_CONFIG_HOME", config, TRUE); /* not the real session.ini */
  start_broadway();
  gtk_test_init(&argc, &argv, NULL);
  app.root = g_build_filename(home, "project", NULL);
  app.font_scale = 1;
  g_mkdir_with_parents(app.root, 0755);
  session_load_settings(&app);
  app.settings.autosave = FALSE; /* edits stay unsaved for the test to see */

  g_test_add_func("/window/temporary-tabs", test_temporary);
  g_test_add_func("/window/temporary-per-group", test_temporary_per_group);
  g_test_add_func("/window/opens-at-left-edge", test_opens_at_left_edge);
  g_test_add_func("/window/comparison-one-tab", test_comparison_one_tab);
  g_test_add_func("/window/comparisons-in-a-row", test_comparisons_in_a_row);

  gapp = gtk_application_new("io.github.tcode.test", G_APPLICATION_NON_UNIQUE);
  g_object_set_data(G_OBJECT(gapp), "status", &status);
  g_signal_connect(gapp, "activate", G_CALLBACK(on_activate), NULL);
  g_application_run(G_APPLICATION(gapp), 1, argv);
  g_object_unref(gapp);
  if (broadway)
    kill(broadway, SIGTERM);
  stop_socket(&app);
  remove_temp_dir(&app);
  files_delete(home, NULL);
  g_free(config);
  g_free(home);
  return status;
}
