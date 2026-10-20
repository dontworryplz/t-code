/*
 * Tests for the editor's editing commands and typing help (src/editor/editing.c) and its find bar
 * (src/editor/findbar.c), on a real GtkSourceView. The editor page's files are included to reach
 * their static functions: keys are fed to the typing help the way the view's key controller would.
 */
#include "editor_page.h"

#include <glib/gstdio.h>

static App app;

/* An editor tab (not in a window) holding text as C, the cursor at the start. */
static Tab *new_tab(const char *text)
{
  Tab *tab = g_new0(Tab, 1);
  GtkTextIter start;

  tab->app = &app;
  tab->kind = TAB_EDITOR;
  tab->path = g_strdup("/test/a.c");
  tab->page = g_object_ref_sink(editor_page());
  editor_view_init(tab, FALSE);
  gtk_source_buffer_set_language(
      tab->buffer, gtk_source_language_manager_get_language(gtk_source_language_manager_get_default(), "c"));
  gtk_text_buffer_set_text(GTK_TEXT_BUFFER(tab->buffer), text, -1);
  gtk_text_buffer_get_start_iter(GTK_TEXT_BUFFER(tab->buffer), &start);
  gtk_text_buffer_place_cursor(GTK_TEXT_BUFFER(tab->buffer), &start);
  return tab;
}

static void free_tab(Tab *tab)
{
  editor_view_free(tab->view);
  g_object_unref(tab->page);
  g_free(tab->path);
  g_free(tab);
}

/* The whole text; the caller frees it. */
static char *text_of(Tab *tab)
{
  GtkTextIter start, end;

  gtk_text_buffer_get_bounds(GTK_TEXT_BUFFER(tab->buffer), &start, &end);
  return gtk_text_buffer_get_text(GTK_TEXT_BUFFER(tab->buffer), &start, &end, TRUE);
}

static void assert_text(Tab *tab, const char *expected)
{
  char *text = text_of(tab);

  g_assert_cmpstr(text, ==, expected);
  g_free(text);
}

static void select_lines(Tab *tab, int line, int column, int to_line, int to_column)
{
  GtkTextIter a, b;

  gtk_text_buffer_get_iter_at_line_offset(GTK_TEXT_BUFFER(tab->buffer), &a, line, column);
  gtk_text_buffer_get_iter_at_line_offset(GTK_TEXT_BUFFER(tab->buffer), &b, to_line, to_column);
  gtk_text_buffer_select_range(GTK_TEXT_BUFFER(tab->buffer), &b, &a);
}

static void cursor_at(Tab *tab, int line, int column)
{
  select_lines(tab, line, column, line, column);
}

static void assert_cursor(Tab *tab, int line, int column)
{
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(tab->buffer);
  GtkTextIter iter;

  gtk_text_buffer_get_iter_at_mark(buffer, &iter, gtk_text_buffer_get_insert(buffer));
  g_assert_cmpint(gtk_text_iter_get_line(&iter), ==, line);
  g_assert_cmpint(gtk_text_iter_get_line_offset(&iter), ==, column);
}

/* A key as typed: the typing help first, then (unless it took the key) what the view would do. */
static void type_key(Tab *tab, guint key)
{
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(tab->buffer);

  if (on_typing_key(NULL, key, 0, 0, tab))
    return;
  if (key == GDK_KEY_BackSpace) {
    GtkTextIter iter;
    gtk_text_buffer_get_iter_at_mark(buffer, &iter, gtk_text_buffer_get_insert(buffer));
    gtk_text_buffer_backspace(buffer, &iter, TRUE, TRUE);
  } else if (key == GDK_KEY_Return) {
    gtk_text_buffer_insert_at_cursor(buffer, "\n", -1);
  } else {
    char c[8] = { 0 };
    g_unichar_to_utf8(gdk_keyval_to_unicode(key), c);
    gtk_text_buffer_delete_selection(buffer, TRUE, TRUE);
    gtk_text_buffer_insert_at_cursor(buffer, c, -1);
  }
}

static void type_text(Tab *tab, const char *text)
{
  for (const char *p = text; *p; p++)
    type_key(tab, gdk_unicode_to_keyval((guchar)*p));
}

/* The selected text; the caller frees it. */
static char *selection_of(Tab *tab)
{
  GtkTextIter start, end;

  gtk_text_buffer_get_selection_bounds(GTK_TEXT_BUFFER(tab->buffer), &start, &end);
  return gtk_text_buffer_get_text(GTK_TEXT_BUFFER(tab->buffer), &start, &end, TRUE);
}

/* Comments ----------------------------------------------------------------------------------- */

static void test_comment_lines(void)
{
  Tab *tab = new_tab("int a;\n  int b;\n\nint c;\n");

  select_lines(tab, 0, 0, 2, 0); /* ends at line 2's start: lines 0 and 1 */
  editor_toggle_comment(tab);
  assert_text(tab, "// int a;\n//   int b;\n\nint c;\n");
  editor_toggle_comment(tab);
  assert_text(tab, "int a;\n  int b;\n\nint c;\n");
  free_tab(tab);
}

/* Blank lines stay blank; the comment goes at the indentation. */
static void test_comment_indented(void)
{
  Tab *tab = new_tab("{\n    x();\n\n    y();\n}\n");

  select_lines(tab, 1, 0, 3, 3);
  editor_toggle_comment(tab);
  assert_text(tab, "{\n    // x();\n\n    // y();\n}\n");
  /* Partly commented: all of them get one. */
  cursor_at(tab, 1, 0);
  editor_toggle_comment(tab);
  select_lines(tab, 1, 0, 3, 3);
  editor_toggle_comment(tab);
  assert_text(tab, "{\n    // x();\n\n    // // y();\n}\n");
  free_tab(tab);
}

static void test_block_comment(void)
{
  Tab *tab = new_tab("a { color: red; }\n");

  gtk_source_buffer_set_language(
      tab->buffer, gtk_source_language_manager_get_language(gtk_source_language_manager_get_default(), "css"));
  editor_toggle_comment(tab);
  assert_text(tab, "/* a { color: red; } */\n");
  editor_toggle_comment(tab);
  assert_text(tab, "a { color: red; }\n");
  free_tab(tab);
}

/* Lines -------------------------------------------------------------------------------------- */

static void test_duplicate(void)
{
  Tab *tab = new_tab("a\nb\n");

  cursor_at(tab, 0, 1);
  editor_duplicate_lines(tab);
  assert_text(tab, "a\na\nb\n");
  assert_cursor(tab, 1, 1); /* on the copy */
  select_lines(tab, 1, 0, 2, 1);
  editor_duplicate_lines(tab);
  assert_text(tab, "a\na\nb\na\nb\n");
  free_tab(tab);
}

static void test_delete_line(void)
{
  Tab *tab = new_tab("a\nb\nc");

  cursor_at(tab, 1, 0);
  editor_delete_lines(tab);
  assert_text(tab, "a\nc");
  cursor_at(tab, 1, 1); /* the last line: the line break before it goes too */
  editor_delete_lines(tab);
  assert_text(tab, "a");
  free_tab(tab);
}

static void test_undo_redo(void)
{
  Tab *tab = new_tab("a\n");

  gtk_text_buffer_set_enable_undo(GTK_TEXT_BUFFER(tab->buffer), TRUE);
  editor_duplicate_lines(tab);
  assert_text(tab, "a\na\n");
  gtk_text_buffer_undo(GTK_TEXT_BUFFER(tab->buffer));
  assert_text(tab, "a\n");
  editor_redo(tab);
  assert_text(tab, "a\na\n");
  free_tab(tab);
}

/* Typing help -------------------------------------------------------------------------------- */

static void test_brackets(void)
{
  Tab *tab = new_tab("");

  type_text(tab, "f(");
  assert_text(tab, "f()");
  type_text(tab, "x[1");
  assert_text(tab, "f(x[1])");
  type_text(tab, "])"); /* steps over both */
  assert_text(tab, "f(x[1])");
  assert_cursor(tab, 0, 7);
  type_text(tab, ";");
  assert_text(tab, "f(x[1]);");
  free_tab(tab);
}

/* A ")" that was there already is typed, not stepped over. */
static void test_closer_not_ours(void)
{
  Tab *tab = new_tab("g(a)");

  cursor_at(tab, 0, 3);
  type_text(tab, ")");
  assert_text(tab, "g(a))");
  free_tab(tab);
}

/* Before a word, "(" is just "(". After a letter, "'" is an apostrophe. */
static void test_no_pair(void)
{
  Tab *tab = new_tab("abc");

  type_text(tab, "(");
  assert_text(tab, "(abc");
  cursor_at(tab, 0, 4);
  type_text(tab, " don't");
  assert_text(tab, "(abc don't");
  free_tab(tab);
}

static void test_backspace_pair(void)
{
  Tab *tab = new_tab("");

  type_text(tab, "x = \"");
  assert_text(tab, "x = \"\"");
  type_key(tab, GDK_KEY_BackSpace);
  assert_text(tab, "x = ");
  free_tab(tab);
}

static void test_wrap_selection(void)
{
  Tab *tab = new_tab("a + b");
  GtkTextIter start, end;
  char *selected;

  select_lines(tab, 0, 0, 0, 5);
  type_text(tab, "(");
  assert_text(tab, "(a + b)");
  g_assert_true(gtk_text_buffer_get_selection_bounds(GTK_TEXT_BUFFER(tab->buffer), &start, &end));
  selected = gtk_text_buffer_get_text(GTK_TEXT_BUFFER(tab->buffer), &start, &end, TRUE);
  g_assert_cmpstr(selected, ==, "a + b");
  g_free(selected);
  free_tab(tab);
}

static void test_enter_between_braces(void)
{
  Tab *tab = new_tab("");

  app.settings.tab_size = 4;
  app.settings.tabs_to_spaces = TRUE;
  editor_view_apply_settings(tab);
  type_text(tab, "  if (x) {");
  type_key(tab, GDK_KEY_Return);
  assert_text(tab, "  if (x) {\n      \n  }");
  assert_cursor(tab, 1, 6);
  free_tab(tab);
}

static void test_outdent_brace(void)
{
  Tab *tab = new_tab("{\n    x;\n    ");

  app.settings.tab_size = 4;
  app.settings.tabs_to_spaces = TRUE;
  editor_view_apply_settings(tab);
  cursor_at(tab, 2, 4);
  type_text(tab, "}");
  assert_text(tab, "{\n    x;\n}");
  free_tab(tab);
}

/* No brackets closed in a comment. */
static void test_no_pair_in_comment(void)
{
  Tab *tab = new_tab("// a\n");
  GtkTextIter start, end;

  gtk_text_buffer_get_bounds(GTK_TEXT_BUFFER(tab->buffer), &start, &end);
  gtk_source_buffer_ensure_highlight(tab->buffer, &start, &end);
  cursor_at(tab, 0, 4);
  type_text(tab, "(");
  assert_text(tab, "// a(\n");
  free_tab(tab);
}

/* Snippets ----------------------------------------------------------------------------------- */

/* A completed function's snippet: its first parameter selected and typed over. Tab then moves to
 * the next one only with enable-snippets on (GtkSourceView's key handler checks it even for pushed
 * snippets); the key itself cannot be fed here, the handler wants a real GdkEvent. */
static void test_snippet_tab(void)
{
  Tab *tab = new_tab("");
  GtkSourceSnippet *snippet = gtk_source_snippet_new_parsed("f(${1:int a}, ${2:int b})$0", NULL);
  GtkTextIter start;
  char *selected;

  g_assert_true(gtk_source_view_get_enable_snippets(tab->source));
  gtk_text_buffer_get_start_iter(GTK_TEXT_BUFFER(tab->buffer), &start);
  gtk_source_view_push_snippet(tab->source, snippet, &start);
  selected = selection_of(tab);
  g_assert_cmpstr(selected, ==, "int a");
  g_free(selected);
  type_text(tab, "x");
  assert_text(tab, "f(x, int b)");
  g_assert_cmpint(gtk_source_snippet_get_focus_position(snippet), ==, 1); /* still in the snippet */
  g_object_unref(snippet);
  free_tab(tab);
}

/* Find bar ----------------------------------------------------------------------------------- */

/* Runs the main loop until the find bar has counted the matches. */
static void wait_for_count(FindBar *f)
{
  gint64 deadline = g_get_monotonic_time() + 2 * G_USEC_PER_SEC;

  while (gtk_source_search_context_get_occurrences_count(f->context) < 0 && g_get_monotonic_time() < deadline)
    g_main_context_iteration(NULL, FALSE);
  update_count(f);
}

static void test_find(void)
{
  Tab *tab = new_tab("foo bar foo baz foo\n");
  FindBar *f;

  findbar_show(tab, FALSE);
  f = tab->view->find;
  gtk_editable_set_text(GTK_EDITABLE(f->find), "foo");
  wait_for_count(f);
  g_assert_cmpstr(gtk_label_get_text(f->count), ==, "1 of 3");
  findbar_next(tab, FALSE);
  g_assert_cmpstr(gtk_label_get_text(f->count), ==, "2 of 3");
  findbar_next(tab, TRUE);
  g_assert_cmpstr(gtk_label_get_text(f->count), ==, "1 of 3");
  findbar_next(tab, TRUE); /* wraps around */
  g_assert_cmpstr(gtk_label_get_text(f->count), ==, "3 of 3");

  gtk_editable_set_text(GTK_EDITABLE(f->find), "nothing");
  wait_for_count(f);
  g_assert_cmpstr(gtk_label_get_text(f->count), ==, "No results");
  free_tab(tab);
}

static void test_replace(void)
{
  Tab *tab = new_tab("foo bar foo baz foo\n");
  FindBar *f;

  findbar_show(tab, TRUE);
  f = tab->view->find;
  g_assert_true(gtk_widget_get_visible(f->replace_row));
  gtk_editable_set_text(GTK_EDITABLE(f->find), "foo");
  gtk_editable_set_text(GTK_EDITABLE(f->replace), "x");
  wait_for_count(f);
  on_replace(f); /* the selected first match, then on to the next */
  assert_text(tab, "x bar foo baz foo\n");
  on_replace_all(f);
  assert_text(tab, "x bar x baz x\n");
  free_tab(tab);
}

/* A big file is counted in the background: "x of N" once it is. */
static void test_find_big_file(void)
{
  GString *text = g_string_new(NULL);
  Tab *tab;
  FindBar *f;

  for (int i = 0; i < 5000; i++)
    g_string_append_printf(text, "line %d with a tab\n", i);
  tab = new_tab(text->str);
  cursor_at(tab, 2500, 0);
  findbar_show(tab, FALSE);
  f = tab->view->find;
  gtk_editable_set_text(GTK_EDITABLE(f->find), "tab");
  wait_for_count(f);
  g_assert_cmpstr(gtk_label_get_text(f->count), ==, "2501 of 5000");
  g_string_free(text, TRUE);
  free_tab(tab);
}

/* Ctrl+H on the match the bar selected keeps it: "1 of 3", and Replace takes that one. */
static void test_find_again(void)
{
  Tab *tab = new_tab("foo bar foo baz foo\n");
  FindBar *f;

  findbar_show(tab, FALSE);
  f = tab->view->find;
  gtk_editable_set_text(GTK_EDITABLE(f->find), "foo");
  wait_for_count(f);
  findbar_show(tab, TRUE);
  wait_for_count(f);
  g_assert_cmpstr(gtk_label_get_text(f->count), ==, "1 of 3");
  gtk_editable_set_text(GTK_EDITABLE(f->replace), "x");
  on_replace(f);
  assert_text(tab, "x bar foo baz foo\n");
  free_tab(tab);
}

/* Match Case on: the selection moves to a match that still is one. */
static void test_find_option(void)
{
  Tab *tab = new_tab("FOO foo\n");
  FindBar *f;
  GtkTextIter start, end;
  char *selected;

  findbar_show(tab, FALSE);
  f = tab->view->find;
  gtk_editable_set_text(GTK_EDITABLE(f->find), "foo");
  wait_for_count(f);
  gtk_source_search_settings_set_case_sensitive(f->settings, TRUE);
  on_option(NULL, f);
  g_assert_true(gtk_text_buffer_get_selection_bounds(GTK_TEXT_BUFFER(tab->buffer), &start, &end));
  selected = gtk_text_buffer_get_text(GTK_TEXT_BUFFER(tab->buffer), &start, &end, TRUE);
  g_assert_cmpstr(selected, ==, "foo");
  g_free(selected);
  free_tab(tab);
}

/* The selection on one line is what is looked for. */
static void test_find_selection(void)
{
  Tab *tab = new_tab("alpha beta alpha\n");

  select_lines(tab, 0, 6, 0, 10);
  findbar_show(tab, FALSE);
  g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(tab->view->find->find)), ==, "beta");
  wait_for_count(tab->view->find);
  g_assert_cmpstr(gtk_label_get_text(tab->view->find->count), ==, "1 of 1");
  free_tab(tab);
}

/* The buffer keeps a file's last line break (an empty last line): an unchanged file that ends in a
 * blank line has no marks, and a line break added at the end is an added line. */
static void test_trailing_newline(void)
{
  char *base = g_build_filename(g_get_tmp_dir(), "t-code-test-trailing.c", NULL);
  Tab *tab = new_tab("a\nb\n\n");
  gint64 until;

  g_assert_false(gtk_source_buffer_get_implicit_trailing_newline(tab->buffer));
  g_file_set_contents(base, "a\nb\n\n", -1, NULL);
  diff_gutter_compare_file(tab, base);
  for (until = g_get_monotonic_time() + G_USEC_PER_SEC; g_get_monotonic_time() < until;)
    g_main_context_iteration(NULL, FALSE);
  for (guint i = 0; i < tab->view->diff->marks->len; i++)
    g_assert_cmpuint(g_array_index(tab->view->diff->marks, guint8, i), ==, LINE_SAME);
  g_remove(base);
  g_free(base);
  free_tab(tab);
}

/* Indent guides ------------------------------------------------------------------------------ */

/* Brackets in strings, characters and comments do not count. */
static void test_guide_brackets(void)
{
  GArray *found = brackets_of("f(\"(\", ')'); // {\n/* [ */ g[1]\n");
  GString *text = g_string_new(NULL);

  for (guint i = 0; i < found->len; i++)
    g_string_append_printf(text, "%c%d", g_array_index(found, Bracket, i).c, g_array_index(found, Bracket, i).line);
  g_assert_cmpstr(text->str, ==, "(0)0[1]1");
  g_string_free(text, TRUE);
  g_array_free(found, TRUE);
}

/* The block around offset in text (see enclosing_pair): "open-close", or "-" for none. */
static char *pair_at(const char *text, int offset)
{
  GArray *brackets = brackets_of(text);
  int open, close;
  char *pair = enclosing_pair(brackets, offset, &open, &close) ? g_strdup_printf("%d-%d", open, close) : g_strdup("-");

  g_array_free(brackets, TRUE);
  return pair;
}

static void check_pair(const char *text, const char *before_cursor, const char *expected)
{
  const char *at = strstr(text, before_cursor);
  char *pair;

  g_assert_nonnull(at);
  pair = pair_at(text, (int)g_utf8_pointer_to_offset(text, at + strlen(before_cursor)));
  g_assert_cmpstr(pair, ==, expected);
  g_free(pair);
}

/* The block around the cursor: the innermost one with lines between its brackets. */
static void test_guide_pair(void)
{
  const char *text = "void f(void)\n{\n  if (x) {\n    a();\n  }\n  b();\n}\n";

  check_pair(text, "  b", "1-6");
  check_pair(text, "    a", "2-4"); /* inside the if */
  check_pair(text, "vo", "-");      /* outside everything */
}

/* The cursor inside a comment or a string: the brackets in them still do not count, after it too. */
static void test_guide_pair_in_comment(void)
{
  const char *text = "int f()\n{\n  /* ( { */\n  s = \"(\";\n  x;\n}\n";

  check_pair(text, "/* (", "1-5");
  check_pair(text, "s = \"", "1-5");
  check_pair(text, "  x", "1-5");
}

/* Offsets count characters: a "ğ" before the cursor is one. */
static void test_guide_pair_utf8(void)
{
  check_pair("{ // ğğğ\n  a;\n\n}\n", "  a", "0-3");
}

/* A blank line gets the guides of the lines around it. */
static void test_guide_blank_lines(void)
{
  Tab *tab = new_tab("{\n    a;\n\n    b;\n        c;\n\n}\n");
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(tab->buffer);

  g_assert_cmpint(guide_count(buffer, 1, 4), ==, 1);
  g_assert_cmpint(guide_count(buffer, 2, 4), ==, 1); /* between two lines of one level */
  g_assert_cmpint(guide_count(buffer, 4, 4), ==, 2);
  g_assert_cmpint(guide_count(buffer, 5, 4), ==, 1); /* where a block closes: the smaller */
  g_assert_cmpint(guide_count(buffer, 0, 4), ==, 0);
  free_tab(tab);
}

/* Diff gutter -------------------------------------------------------------------------------- */

/* The gutter compares with a file; typing changes the marks a moment later. */
static void test_diff_gutter(void)
{
  char *base = g_build_filename(g_get_tmp_dir(), "t-code-test-base.c", NULL);
  Tab *tab = new_tab("a\nb\nc\n");
  gint64 until;
  GtkTextIter start, end;

  g_file_set_contents(base, "a\nb\nc\n", -1, NULL);
  diff_gutter_compare_file(tab, base);
  gtk_text_buffer_get_iter_at_line(GTK_TEXT_BUFFER(tab->buffer), &start, 1);
  end = start;
  gtk_text_iter_forward_char(&end);
  gtk_text_buffer_delete(GTK_TEXT_BUFFER(tab->buffer), &start, &end);
  gtk_text_buffer_insert(GTK_TEXT_BUFFER(tab->buffer), &start, "B", -1);       /* b -> B */
  gtk_text_buffer_insert_at_cursor(GTK_TEXT_BUFFER(tab->buffer), "new\n", -1); /* a line at the start */
  for (until = g_get_monotonic_time() + G_USEC_PER_SEC; g_get_monotonic_time() < until;)
    g_main_context_iteration(NULL, FALSE);
  g_assert_nonnull(tab->view->diff->marks);
  g_assert_cmpuint(g_array_index(tab->view->diff->marks, guint8, 0), ==, LINE_ADDED);
  g_assert_cmpuint(g_array_index(tab->view->diff->marks, guint8, 1), ==, LINE_SAME);
  g_assert_cmpuint(g_array_index(tab->view->diff->marks, guint8, 2), ==, LINE_MODIFIED);
  g_remove(base);
  g_free(base);
  free_tab(tab);
}

/* Parameter hint ----------------------------------------------------------------------------- */

/* The view's adjustments can outlive the editor (a comparison's other side scrolls them): once
 * the editor goes, scrolling them must not reach its freed hint. */
static void test_signature_outlived(void)
{
  Tab *tab = new_tab("int a;\n");
  GtkAdjustment *vertical = g_object_ref(gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(tab->source)));
  GtkAdjustment *horizontal = g_object_ref(gtk_scrollable_get_hadjustment(GTK_SCROLLABLE(tab->source)));

  g_assert_cmpuint(g_signal_handler_find(vertical, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, on_scrolled, NULL), !=, 0);
  free_tab(tab);
  g_assert_cmpuint(g_signal_handler_find(vertical, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, on_scrolled, NULL), ==, 0);
  g_assert_cmpuint(g_signal_handler_find(horizontal, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, on_scrolled, NULL), ==, 0);
  gtk_adjustment_set_value(vertical, 10);
  g_object_unref(vertical);
  g_object_unref(horizontal);
}

/* Side by side, the sides line up with gap rows: the new side's where lines were deleted (drawn
 * red), the old side's where lines were added (drawn black); changed lines and words are colored. */
static void test_comparison_gaps(void)
{
  Tab *new_side = new_tab("a\nadded\nb\n"), *old_side = new_tab("a\nb\nremoved\n");
  int new_px = 0, old_px = 0, above, below;
  gboolean new_red = FALSE, old_red = FALSE;

  diff_gutter_pair(new_side, old_side);
  for (int line = 0; line < 4; line++) {
    if (diff_gutter_line_gaps(new_side, line, &above, &below) && above + below > 0)
      new_red = TRUE;
    new_px += above + below;
    if (diff_gutter_line_gaps(old_side, line, &above, &below) && above + below > 0)
      old_red = TRUE;
    old_px += above + below;
  }
  g_assert_cmpint(new_px, >, 0); /* for "removed" */
  g_assert_cmpint(old_px, >, 0); /* for "added" */
  g_assert_cmpint(new_px, ==, old_px);
  g_assert_true(new_red);
  g_assert_false(old_red);

  /* Changed lines are tinted and their words colored (a tag); the same lines are not. */
  GtkTextIter at;
  GtkTextTag *words = gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table(GTK_TEXT_BUFFER(new_side->buffer)),
                                                "tcode-changed-words");
  g_assert_false(diff_gutter_line_changed(new_side, 0));
  g_assert_true(diff_gutter_line_changed(new_side, 1)); /* "added" */
  g_assert_true(diff_gutter_line_changed(old_side, 2)); /* "removed" */
  g_assert_nonnull(words);
  gtk_text_buffer_get_iter_at_line_offset(GTK_TEXT_BUFFER(new_side->buffer), &at, 1, 2);
  g_assert_true(gtk_text_iter_has_tag(&at, words));
  gtk_text_buffer_get_iter_at_line(GTK_TEXT_BUFFER(new_side->buffer), &at, 0);
  g_assert_false(gtk_text_iter_has_tag(&at, words));

  diff_gutter_pair(new_side, NULL); /* no comparison: no colors */
  g_assert_false(diff_gutter_line_changed(new_side, 1));
  gtk_text_buffer_get_iter_at_line_offset(GTK_TEXT_BUFFER(new_side->buffer), &at, 1, 2);
  g_assert_false(gtk_text_iter_has_tag(&at, words));
  free_tab(old_side);
  free_tab(new_side);
}

int main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  if (!gtk_init_check()) {
    g_test_skip("no display");
    return g_test_run();
  }
  gtk_source_init();
  app.settings.tab_size = 4;
  app.settings.tabs_to_spaces = TRUE;
  app.font_scale = 1.0;
  app.styles_dir = g_canonicalize_filename("styles", NULL); /* run from the repository (make test) */
  g_test_add_func("/editing/comment-lines", test_comment_lines);
  g_test_add_func("/editing/comment-indented", test_comment_indented);
  g_test_add_func("/editing/block-comment", test_block_comment);
  g_test_add_func("/editing/duplicate", test_duplicate);
  g_test_add_func("/editing/delete-line", test_delete_line);
  g_test_add_func("/editing/undo-redo", test_undo_redo);
  g_test_add_func("/editing/brackets", test_brackets);
  g_test_add_func("/editing/closer-not-ours", test_closer_not_ours);
  g_test_add_func("/editing/no-pair", test_no_pair);
  g_test_add_func("/editing/backspace-pair", test_backspace_pair);
  g_test_add_func("/editing/wrap-selection", test_wrap_selection);
  g_test_add_func("/editing/enter-between-braces", test_enter_between_braces);
  g_test_add_func("/editing/outdent-brace", test_outdent_brace);
  g_test_add_func("/editing/no-pair-in-comment", test_no_pair_in_comment);
  g_test_add_func("/editing/snippet-tab", test_snippet_tab);
  g_test_add_func("/find/next-previous", test_find);
  g_test_add_func("/find/replace", test_replace);
  g_test_add_func("/find/selection", test_find_selection);
  g_test_add_func("/find/again", test_find_again);
  g_test_add_func("/find/option", test_find_option);
  g_test_add_func("/find/big-file", test_find_big_file);
  g_test_add_func("/diff-gutter/marks", test_diff_gutter);
  g_test_add_func("/diff-gutter/trailing-newline", test_trailing_newline);
  g_test_add_func("/diff-gutter/comparison-gaps", test_comparison_gaps);
  g_test_add_func("/guides/brackets", test_guide_brackets);
  g_test_add_func("/guides/pair", test_guide_pair);
  g_test_add_func("/guides/pair-in-comment", test_guide_pair_in_comment);
  g_test_add_func("/guides/pair-utf8", test_guide_pair_utf8);
  g_test_add_func("/guides/blank-lines", test_guide_blank_lines);
  g_test_add_func("/signature/outlived", test_signature_outlived);
  return g_test_run();
}
