#define _DEFAULT_SOURCE /* kill() under -std=c23 with musl */
/*
 * Tests for how tabs open and close, in a whole t-code window (every module but main.c) on a
 * temporary project with a temporary config: temporary tabs (a click in the Files panel, VS Code's
 * preview editor), comparisons in one tab, the first run's layout and fonts, the files edited by
 * hand. Needs a display; skipped without one.
 */
#include "editor/editor.h"
#include "dialogs/settings.h"
#include "editor/editorview.h"
#include "editor/tabs.h"
#include "files/fileops.h"
#include "files/opentabs.h"
#include "git/compare.h"
#include "ipc/socket.h"
#include "project/session.h"
#include "project/userfiles.h"
#include "window/secondwindow.h"
#include "window/palette.h"
#include "window/theme.h"
#include "window/window.h"

#include <gio/gunixsocketaddress.h>
#include <glib/gstdio.h>
#include <signal.h>

static App app;

static char *file(const char *name)
{
  char *path = g_build_filename(app.project.root, name, NULL);

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

/* Runs the main loop until done() says so (git answers, a file loads), for at most 5 seconds. */
static void run_until(gboolean (*done)(gconstpointer), gconstpointer data)
{
  gint64 end = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;

  while (!done(data) && g_get_monotonic_time() < end)
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
  ok = g_spawn_sync(app.project.root, (char **)argv->pdata, NULL,
                    G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL,
                    NULL, &status, NULL) &&
       g_spawn_check_wait_status(status, NULL);
  g_ptr_array_free(argv, TRUE);
  return ok;
}

/* The old version shown next to path in its comparison tab, or NULL. */
static Tab *old_version_of(const char *path)
{
  for (int i = 0; i < tab_count(&app); i++) {
    Tab *tab = tab_nth(&app, i);
    if (g_strcmp0(tab->path, path) == 0 && tab->compare.old_side)
      return tab->compare.old_side;
  }
  return NULL;
}

static gboolean compared(gconstpointer path)
{
  return old_version_of(path) != NULL;
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
  app.project.git_top = g_canonicalize_filename(app.project.root, NULL);

  open_temporary("tracked.txt");
  compare_with_head(&app, path);
  run_until(compared, path);  /* git answers in the background */
  assert_tabs("tracked.txt"); /* one tab, kept (no longer temporary) */
  tab = tab_nth(&app, 0);
  g_assert_nonnull(tab->compare.old_side);
  g_assert_cmpstr(tab->compare.old_side->compare.with, ==, path);
  g_assert_true(g_str_has_prefix(gtk_label_get_text(tab->label), "tracked.txt ↔ "));
  g_assert_false(gtk_widget_get_visible(GTK_WIDGET(app.editors.groups[1]))); /* no second group */

  stop_comparing(&app); /* the file stays */
  run(50);
  assert_tabs("tracked.txt");
  g_assert_null(tab_nth(&app, 0)->compare.old_side);
  g_assert_cmpstr(gtk_label_get_text(tab_nth(&app, 0)->label), ==, "tracked.txt");

  compare_with_head(&app, path);
  run_until(compared, path);
  stop_comparing(&app);
  compare_with_head(&app, path);
  run_until(compared, path);
  compare_with_head(&app, path); /* again: still one comparison */
  run(300);
  assert_tabs("tracked.txt");
  quit_editor(tab_nth(&app, 0)); /* closes both */
  run(50);
  assert_tabs("");
  g_free(path);
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
  g_set_str(&app.project.git_top, NULL);
  app.project.git_top = g_canonicalize_filename(app.project.root, NULL);
  for (size_t i = 0; i < G_N_ELEMENTS(names); i++)
    g_assert_true(g_file_set_contents(paths[i], "one\nchanged\n", -1, NULL));

  for (int round = 0; round < 2; round++)
    for (size_t i = 0; i < G_N_ELEMENTS(names); i++) {
      compare_with_head(&app, paths[i]);
      run_until(compared, paths[i]);
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

static gboolean loaded(gconstpointer tab)
{
  return !((const Tab *)tab)->view->loading;
}

static gboolean in_conflict(gconstpointer tab)
{
  return editor_in_conflict((Tab *)tab);
}

static gboolean saved(gconstpointer tab)
{
  return !((const Tab *)tab)->modified;
}

/* A file changed on disk under unsaved changes is not written over by a save: the bar asks, and
 * Overwrite writes the editor's text; Reload takes the file's. */
static void test_save_conflict(void)
{
  char *path = g_build_filename(app.project.root, "conflict.txt", NULL), *text = NULL;
  Tab *tab;

  g_assert_true(g_file_set_contents(path, "mine\n", -1, NULL));
  open_file(&app, path);
  tab = find_tab(&app, TAB_EDITOR, path);
  run_until(loaded, tab);
  g_usleep(10 * 1000);                                                     /* another mtime */
  g_assert_true(g_file_set_contents(path, "theirs\n", -1, NULL));          /* another program */
  gtk_text_buffer_insert_at_cursor(GTK_TEXT_BUFFER(tab->buffer), "x", -1); /* unsaved */
  save_editor(tab);
  run_until(in_conflict, tab);
  g_assert_true(editor_in_conflict(tab));
  g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
  g_assert_cmpstr(text, ==, "theirs\n"); /* not written over */
  g_free(text);
  g_assert_true(tab->modified);

  tab->view->overwrite = TRUE; /* the bar's Overwrite */
  save_editor(tab);
  run_until(saved, tab);
  g_assert_false(editor_in_conflict(tab));
  g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
  g_assert_cmpstr(text, ==, "xmine\n");
  g_free(text);

  /* Changed on disk again while unsaved: the bar shows before any save; Reload takes the disk's. */
  gtk_text_buffer_insert_at_cursor(GTK_TEXT_BUFFER(tab->buffer), "y", -1);
  g_usleep(10 * 1000);
  g_assert_true(g_file_set_contents(path, "again\n", -1, NULL));
  run_until(in_conflict, tab);
  g_assert_true(editor_in_conflict(tab));
  editor_reload(tab);
  run_until(loaded, tab);
  g_assert_false(editor_in_conflict(tab));
  g_assert_false(tab->modified);
  close_everything();
  g_free(path);
}

static gboolean has_tab(gconstpointer path)
{
  return find_tab(&app, TAB_EDITOR, path) != NULL;
}

/* Requests on the control socket may end with NUL bytes instead of line breaks: a path may hold a
 * line break. */
static void test_socket_requests(void)
{
  char *odd = g_build_filename(app.project.root, "two\nlines.txt", NULL);
  char *plain = file("plain.txt");
  char *request = g_strdup_printf("open %s%copen %s", plain, '\0', odd);
  GSocketClient *client = g_socket_client_new();
  GSocketAddress *address = g_unix_socket_address_new(app.paths.socket);
  GSocketConnection *conn;

  g_assert_true(g_file_set_contents(odd, "x\n", -1, NULL));
  conn = g_socket_client_connect(client, G_SOCKET_CONNECTABLE(address), NULL, NULL);
  g_assert_nonnull(conn);
  /* "open <plain>\0open <odd>\0": the string's own NUL ends the second one */
  g_assert_true(g_output_stream_write_all(g_io_stream_get_output_stream(G_IO_STREAM(conn)), request,
                                          strlen(plain) + strlen(odd) + 12, NULL, NULL, NULL));
  run_until(has_tab, odd);
  g_assert_true(has_tab(plain));
  g_assert_true(has_tab(odd));
  g_io_stream_close(G_IO_STREAM(conn), NULL, NULL);
  g_object_unref(conn);

  char *lined = file("lined.txt"), *line = g_strdup_printf("open %s\n", lined); /* tools/headless.sh's way */
  conn = g_socket_client_connect(client, G_SOCKET_CONNECTABLE(address), NULL, NULL);
  g_assert_true(g_output_stream_write_all(g_io_stream_get_output_stream(G_IO_STREAM(conn)), line, strlen(line), NULL,
                                          NULL, NULL));
  run_until(has_tab, lined);
  g_assert_true(has_tab(lined));
  g_io_stream_close(G_IO_STREAM(conn), NULL, NULL);
  g_object_unref(conn);
  g_free(line);
  g_free(lined);
  g_object_unref(address);
  g_object_unref(client);
  close_everything();
  g_free(request);
  g_free(plain);
  g_free(odd);
}

static gboolean no_second_window(gconstpointer data G_GNUC_UNUSED)
{
  return app.editors.second == NULL;
}

/* "Open in New Window": the file's tab goes to a second window (one tab per file still); closing that
 * window brings its tabs back, and the session remembers them. */
static void test_second_window(void)
{
  char *a = file("w1.txt"), *b = file("w2.txt");
  Tab *tab;

  open_kept("w1.txt");
  open_in_new_window(&app, a); /* already open: its tab moves */
  run(50);
  g_assert_nonnull(app.editors.second);
  g_assert_cmpint(tab_count(&app), ==, 1);
  tab = find_tab(&app, TAB_EDITOR, a);
  g_assert_cmpint(tab_group(tab), ==, WINDOW_GROUP);
  g_assert_true(tab_window(tab) == app.editors.second);
  open_file(&app, a); /* still one tab */
  g_assert_cmpint(tab_count(&app), ==, 1);

  open_kept("w2.txt");
  move_to_new_window(&app); /* the current tab: w2.txt, opened in the active group (the window's) */
  g_assert_cmpint(tab_group(find_tab(&app, TAB_EDITOR, b)), ==, WINDOW_GROUP);

  session_save(&app);
  gtk_window_close(app.editors.second); /* the tabs come back */
  run_until(no_second_window, NULL);
  g_assert_null(app.editors.second);
  g_assert_cmpint(tab_count(&app), ==, 2);
  g_assert_cmpint(tab_group(find_tab(&app, TAB_EDITOR, a)), ==, 0);
  g_assert_cmpint(tab_group(find_tab(&app, TAB_EDITOR, b)), ==, 0);

  close_everything();
  session_restore_tabs(&app); /* as saved: both in the second window */
  run(100);
  g_assert_nonnull(app.editors.second);
  g_assert_cmpint(tab_group(find_tab(&app, TAB_EDITOR, a)), ==, WINDOW_GROUP);
  g_assert_cmpint(tab_group(find_tab(&app, TAB_EDITOR, b)), ==, WINDOW_GROUP);

  remove_tab(find_tab(&app, TAB_EDITOR, a));
  remove_tab(find_tab(&app, TAB_EDITOR, b)); /* the last one: the window goes */
  run_until(no_second_window, NULL);
  g_assert_null(app.editors.second);
  g_assert_cmpint(app.editors.active, ==, 0);
  close_everything();
  session_save(&app);
  g_free(a);
  g_free(b);
}

/* The tab names in a group's strip, in order: "a.txt b.txt". */
static char *strip_names(int group)
{
  GtkWidget *strip = gtk_widget_get_parent(tab_nth(&app, 0)->item);
  GString *text = g_string_new(NULL);

  for (int i = 0; i < tab_count(&app); i++)
    if (tab_group(tab_nth(&app, i)) == group)
      strip = gtk_widget_get_parent(tab_nth(&app, i)->item);
  for (GtkWidget *w = gtk_widget_get_first_child(strip); w; w = gtk_widget_get_next_sibling(w)) {
    Tab *tab = NULL;
    for (int i = 0; i < tab_count(&app); i++)
      if (tab_nth(&app, i)->item == w)
        tab = tab_nth(&app, i);
    char *name = g_path_get_basename(tab->path);
    g_string_append_printf(text, "%s%s%s", text->len ? " " : "", name,
                           gtk_widget_get_state_flags(w) & GTK_STATE_FLAG_CHECKED ? "!" : "");
    g_free(name);
  }
  return g_string_free(text, FALSE);
}

static void assert_strip(int group, const char *want)
{
  char *now = strip_names(group);

  g_assert_cmpstr(now, ==, want);
  g_free(now);
}

static gboolean laid_out(gconstpointer item)
{
  return gtk_widget_get_width((GtkWidget *)item) > 30;
}

/* Each group's tab strip follows its tabs: their order, the current one ("!"), a tab moved to the
 * other group or dropped among the tabs, a comparison (one tab). */
static void test_tab_strip(void)
{
  open_kept("s1.txt");
  open_kept("s2.txt");
  open_kept("s3.txt");
  assert_strip(0, "s1.txt s2.txt s3.txt!");
  select_tab(tab_nth(&app, 0));
  run(20);
  assert_strip(0, "s1.txt! s2.txt s3.txt");

  /* s3 dropped before s1 in the same strip */
  GtkWidget *strip = gtk_widget_get_parent(tab_nth(&app, 0)->item);
  GtkWidget *scrolled = gtk_widget_get_ancestor(strip, GTK_TYPE_SCROLLED_WINDOW);
  GValue value = G_VALUE_INIT;
  g_value_init(&value, G_TYPE_POINTER);
  g_value_set_pointer(&value, tab_nth(&app, 2));
  GtkDropTarget *target = NULL;
  GListModel *controllers = gtk_widget_observe_controllers(scrolled);
  for (guint i = 0; i < g_list_model_get_n_items(controllers) && !target; i++) {
    GObject *controller = g_list_model_get_item(controllers, i);
    if (GTK_IS_DROP_TARGET(controller))
      target = GTK_DROP_TARGET(controller);
    g_object_unref(controller);
  }
  g_object_unref(controllers);
  g_assert_nonnull(target);
  run_until(laid_out, gtk_widget_get_first_child(strip)); /* the drop is placed by the tabs' places */
  gboolean dropped = FALSE;
  g_signal_emit_by_name(target, "drop", &value, 0.0, 0.0, &dropped);
  g_assert_true(dropped);
  run(20);
  assert_strip(0, "s3.txt! s1.txt s2.txt");
  assert_tabs("s3.txt s1.txt s2.txt");

  set_split(&app, TRUE);
  move_tab(tab_nth(&app, 1), 1); /* s1 to the right */
  run(20);
  assert_strip(0, "s3.txt! s2.txt");
  assert_strip(1, "s1.txt!");
  g_value_unset(&value);
  close_everything();
}

/* The Open Tabs list at the top of the Files panel (its first child). */
static GtkListBox *open_tabs_list(void)
{
  GtkWidget *section = gtk_widget_get_first_child(app.files);
  GtkWidget *scrolled = gtk_widget_get_last_child(section);

  return GTK_LIST_BOX(gtk_widget_get_first_child(gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(scrolled))));
}

static int list_rows(GtkListBox *list)
{
  int n = 0;

  while (gtk_list_box_get_row_at_index(list, n))
    n++;
  return n;
}

/* Open Tabs lists every group's tabs (with group headings once two groups have tabs), the current
 * one selected; a click shows a tab; it folds, and the session keeps that. */
static void test_open_tabs(void)
{
  GtkListBox *list = open_tabs_list();
  GtkListBoxRow *row;

  open_kept("o1.txt");
  open_kept("o2.txt");
  run(50);
  g_assert_cmpint(list_rows(list), ==, 2);
  g_assert_true(gtk_list_box_get_selected_row(list) == gtk_list_box_get_row_at_index(list, 1)); /* o2 */

  row = gtk_list_box_get_row_at_index(list, 0);
  g_signal_emit_by_name(list, "row-activated", row);
  run(50);
  g_assert_true(current_tab(&app) == tab_nth(&app, 0)); /* o1 */

  set_split(&app, TRUE);
  move_tab(tab_nth(&app, 1), 1);
  run(50);
  g_assert_cmpint(list_rows(list), ==, 4); /* GROUP 1, o1, GROUP 2, o2 */

  open_tabs_show(FALSE);
  g_assert_false(open_tabs_shown());
  session_save(&app);
  open_tabs_show(TRUE);
  session_apply_window(&app); /* as saved: folded */
  g_assert_false(open_tabs_shown());
  open_tabs_show(TRUE);
  session_save(&app);
  close_everything();
  run(50);
  g_assert_cmpint(list_rows(list), ==, 0);
}

/* The picker's entry key controller (the arrows go through it). */
static GtkEventController *picker_keys(GtkWidget *entry)
{
  GListModel *controllers = gtk_widget_observe_controllers(entry);
  GtkEventController *keys = NULL;

  for (guint i = 0; i < g_list_model_get_n_items(controllers) && !keys; i++) {
    GObject *controller = g_list_model_get_item(controllers, i);
    if (GTK_IS_EVENT_CONTROLLER_KEY(controller))
      keys = GTK_EVENT_CONTROLLER(controller);
    g_object_unref(controller);
  }
  g_object_unref(controllers);
  return keys;
}

static GtkWidget *focused_entry(void)
{
  GtkWidget *focus = gtk_root_get_focus(GTK_ROOT(app.window));

  return focus ? gtk_widget_get_ancestor(focus, GTK_TYPE_SEARCH_ENTRY) : NULL;
}

/* "Preferences: Color Theme": the arrows show each theme at once, unsaved; Escape goes back to the
 * saved one, Enter keeps the one shown. */
static void test_theme_preview(void)
{
  char *saved = g_strdup(app.settings.color_theme);
  gboolean handled = FALSE;
  GtkWidget *entry;

  color_theme_pick(&app);
  run(50);
  entry = focused_entry();
  g_assert_nonnull(entry);
  g_signal_emit_by_name(picker_keys(entry), "key-pressed", GDK_KEY_Down, 0, 0, &handled);
  run(50);
  g_assert_true(handled);
  g_assert_cmpstr(palette_id(), !=, saved);             /* the second theme, shown */
  g_assert_cmpstr(app.settings.color_theme, ==, saved); /* not saved */
  g_signal_emit_by_name(entry, "stop-search");          /* Escape */
  run(50);
  g_assert_cmpstr(palette_id(), ==, saved);

  color_theme_pick(&app);
  run(50);
  entry = focused_entry();
  g_signal_emit_by_name(picker_keys(entry), "key-pressed", GDK_KEY_Down, 0, 0, &handled);
  g_signal_emit_by_name(entry, "activate"); /* Enter */
  run(50);
  g_assert_cmpstr(app.settings.color_theme, ==, palette_id());
  g_assert_cmpstr(palette_id(), !=, saved);

  g_set_str(&app.settings.color_theme, saved); /* back, for the other tests */
  color_theme_apply(&app, saved);
  g_free(saved);
}

/* A file opens at its left edge, also in a narrow view without word wrap (the cursor's reveal used to
 * scroll sideways by a few pixels, cutting off every line's first letters). */
static void test_opens_at_left_edge(void)
{
  char *path = g_build_filename(app.project.root, "long.txt", NULL);
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

/* The first run (no session.ini yet): Files shows on the left, the shell below the editors. */
static void test_first_run_layout(void)
{
  g_assert_cmpint(panel_dock(&app, "files"), ==, 0);
  g_assert_cmpstr(gtk_stack_get_visible_child_name(app.dock.stacks[0]), ==, "files");
  g_assert_cmpint(panel_dock(&app, "shell"), ==, 4);
}

/* A font that is not installed falls back to monospace, not to a proportional one. */
static void test_font_fallback(void)
{
  const PangoFontDescription *font = vte_terminal_get_font(app.shell.term);

  g_assert_cmpstr(pango_font_description_get_family(font), ==, "Cascadia Code,monospace");
}

/* The settings session.ini kept before 0.3 moved into settings.ini, each with its comment. */
static void test_settings_moved(void)
{
  char *path = settings_file(), *text = NULL;
  char *session = g_build_filename(g_get_user_config_dir(), "t-code", "session.ini", NULL), *old = NULL;

  g_assert_false(app.settings.minimap); /* main() wrote it to session.ini first */
  g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
  g_assert_nonnull(strstr(text, "minimap=false"));
  g_assert_nonnull(strstr(text, "# A small picture of the file next to the scrollbar\nminimap=false"));
  g_assert_nonnull(strstr(text, "theme=rounded"));
  g_assert_true(g_file_get_contents(session, &old, NULL, NULL));
  g_assert_null(strstr(old, "[settings]"));
  g_free(old);
  g_free(session);
  g_free(text);
  g_free(path);
}

/* settings.ini edited by another program applies at once; t-code's own saves change nothing. */
static void test_settings_edited(void)
{
  char *path = settings_file(), *text = NULL;

  g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
  char **parts = g_strsplit(text, "theme=rounded", -1);
  char *square = g_strjoinv("theme=square", parts);
  g_assert_true(g_file_set_contents(path, square, -1, NULL));
  for (int i = 0; i < 100 && !app.settings.square; i++)
    run(20);
  g_assert_true(app.settings.square);
  g_assert_true(app.settings.minimap == FALSE); /* the rest stays */

  app.settings.square = FALSE;
  session_save_settings(&app);
  run(400);
  g_assert_false(app.settings.square);
  g_strfreev(parts);
  g_free(square);
  g_free(text);
  g_free(path);
}

/* The files to edit by hand open in tabs: theme.css made with its examples, a copy of the colors. */
static void test_open_user_files(void)
{
  char *theme = g_build_filename(g_get_user_config_dir(), "t-code", "theme.css", NULL), *text = NULL;
  char *colors = g_build_filename(g_get_user_config_dir(), "t-code", "styles", "dark-classic.xml", NULL);

  open_settings_file(&app);
  open_user_theme(&app);
  open_editor_colors(&app);
  run(100);
  assert_tabs("settings.ini theme.css dark-classic.xml");
  g_assert_true(g_file_get_contents(theme, &text, NULL, NULL));
  g_assert_nonnull(strstr(text, "Saving this file applies it at once"));
  g_free(text);
  g_assert_true(g_file_get_contents(colors, &text, NULL, NULL));
  g_assert_nonnull(strstr(text, "Your copy of t-code's editor colors"));
  g_assert_nonnull(strstr(text, "id=\"t-code-dark-classic\""));
  g_free(text);
  close_everything();
  g_free(colors);
  g_free(theme);
}

/* The Settings window can be made smaller than its rows, which then scroll. */
static void test_settings_scrolls(void)
{
  GtkWindow *settings = NULL;

  show_settings(&app);
  run(50);
  for (guint i = 0; i < g_list_model_get_n_items(gtk_window_get_toplevels()); i++) {
    GtkWindow *window = g_list_model_get_item(gtk_window_get_toplevels(), i);
    if (g_strcmp0(gtk_window_get_title(window), "Settings") == 0)
      settings = window;
    g_object_unref(window);
  }
  g_assert_nonnull(settings);
  g_assert_true(gtk_window_get_resizable(settings));
  g_assert_true(GTK_IS_SCROLLED_WINDOW(gtk_window_get_child(settings)));
  gtk_window_destroy(settings);
  run(50);
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
  app.project.root = g_build_filename(home, "project", NULL);
  app.font_scale = 1;
  g_mkdir_with_parents(app.project.root, 0755);
  char *session = g_build_filename(config, "t-code", "session.ini", NULL), *dir = g_path_get_dirname(session);
  g_mkdir_with_parents(dir, 0700);
  g_assert_true(g_file_set_contents(session, "[settings]\nminimap=false\n", -1, NULL)); /* before 0.3 */
  g_free(dir);
  g_free(session);
  session_load_settings(&app);
  app.settings.autosave = FALSE; /* edits stay unsaved for the test to see */

  g_test_add_func("/window/first-run-layout", test_first_run_layout);
  g_test_add_func("/window/font-fallback", test_font_fallback);
  g_test_add_func("/window/settings-moved", test_settings_moved);
  g_test_add_func("/window/settings-edited", test_settings_edited);
  g_test_add_func("/window/open-user-files", test_open_user_files);
  g_test_add_func("/window/settings-scrolls", test_settings_scrolls);
  g_test_add_func("/window/temporary-tabs", test_temporary);
  g_test_add_func("/window/temporary-per-group", test_temporary_per_group);
  g_test_add_func("/window/opens-at-left-edge", test_opens_at_left_edge);
  g_test_add_func("/window/comparison-one-tab", test_comparison_one_tab);
  g_test_add_func("/window/comparisons-in-a-row", test_comparisons_in_a_row);
  g_test_add_func("/window/save-conflict", test_save_conflict);
  g_test_add_func("/window/socket-requests", test_socket_requests);
  g_test_add_func("/window/second-window", test_second_window);
  g_test_add_func("/window/tab-strip", test_tab_strip);
  g_test_add_func("/window/open-tabs", test_open_tabs);
  g_test_add_func("/window/theme-preview", test_theme_preview);

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
