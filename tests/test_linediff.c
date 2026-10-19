/*
 * Tests for src/git/linediff.c: the diff gutter's marks.
 */
#include "git/linediff.h"

/* The marks as a string, one letter per line: . same, A added, M modified, D deleted above. */
static char *marks_of(const char *old, const char *new)
{
  GArray *marks = line_diff(old, new);
  GString *text = g_string_new(NULL);

  for (guint i = 0; i < marks->len; i++)
    g_string_append_c(text, ".AMD"[g_array_index(marks, guint8, i)]);
  g_array_free(marks, TRUE);
  return g_string_free(text, FALSE);
}

static void check(const char *old, const char *new, const char *expected)
{
  char *marks = marks_of(old, new);

  g_assert_cmpstr(marks, ==, expected);
  g_free(marks);
}

static void test_same(void)
{
  check("a\nb\nc\n", "a\nb\nc", "...");
  check("", "", ".");
}

static void test_added(void)
{
  check("a\nc\n", "a\nb\nc", ".A.");
  check("a\n", "a\nb\nc", ".AA");
  check("b\n", "a\nb", "A.");
}

static void test_modified(void)
{
  check("a\nb\nc\n", "a\nB\nc", ".M.");
  /* Two lines for one: the first is modified, the second added. */
  check("a\nb\nc\n", "a\nB1\nB2\nc", ".MA.");
}

static void test_deleted(void)
{
  check("a\nb\nc\n", "a\nc", ".D");
  check("a\nb\nc\n", "b\nc", "D.");
  check("a\nb\nc\n", "a\nb", ".D"); /* at the end: the last line carries it */
}

/* Several hunks apart from each other, lined up by what they share. */
static void test_hunks(void)
{
  check("1\n2\n3\n4\n5\n6\n7\n", "1\nX\n3\n4\n6\n7\nY", ".M..D.A");
}

/* A file rewritten past the table's size: every line in the middle counts as modified. */
static void test_big(void)
{
  GString *old = g_string_new(NULL), *new = g_string_new(NULL);
  GArray *marks;

  for (int i = 0; i < 3000; i++) {
    g_string_append_printf(old, "old %d\n", i);
    g_string_append_printf(new, "new %d\n", i);
  }
  marks = line_diff(old->str, new->str);
  g_assert_cmpuint(marks->len, ==, 3000);
  g_assert_cmpuint(g_array_index(marks, guint8, 0), ==, LINE_MODIFIED);
  g_assert_cmpuint(g_array_index(marks, guint8, 2999), ==, LINE_MODIFIED);
  g_array_free(marks, TRUE);
  g_string_free(new, TRUE);
  g_string_free(old, TRUE);
}

/* The gaps as a string: "<line>:<count>" (below that line) or "^:<count>" (above line 0). */
static char *gaps_of(const char *old, const char *new, gboolean old_side)
{
  GArray *gaps = line_diff_gaps(old, new, old_side);
  GString *text = g_string_new(NULL);

  for (guint i = 0; i < gaps->len; i++) {
    DiffGap *gap = &g_array_index(gaps, DiffGap, i);
    if (gap->above)
      g_string_append_printf(text, "%s^:%d", text->len ? " " : "", gap->count);
    else
      g_string_append_printf(text, "%s%d:%d", text->len ? " " : "", gap->line, gap->count);
  }
  g_array_free(gaps, TRUE);
  return g_string_free(text, FALSE);
}

static void check_gaps(const char *old, const char *new, const char *new_side, const char *old_side)
{
  char *a = gaps_of(old, new, FALSE), *b = gaps_of(old, new, TRUE);

  g_assert_cmpstr(a, ==, new_side);
  g_assert_cmpstr(b, ==, old_side);
  g_free(b);
  g_free(a);
}

/* Side by side, the side with fewer lines in a stretch gets the difference as a gap. */
static void test_gaps(void)
{
  check_gaps("a\nb\nc\n", "a\nc", "0:1", "");       /* b deleted: a gap under a on the new side */
  check_gaps("a\nc\n", "a\nb1\nb2\nc", "", "0:2");  /* two added: a gap under a on the old side */
  check_gaps("x\ny\na\n", "a", "^:2", "");          /* deleted at the start: above line 0 */
  check_gaps("a\nb\nc\n", "a\nB\nc", "", "");       /* modified: same size, no gap */
  check_gaps("a\nb\n", "a\nB1\nB2\nB3", "", "1:2"); /* one for three: two rows */
}

int main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/linediff/same", test_same);
  g_test_add_func("/linediff/added", test_added);
  g_test_add_func("/linediff/modified", test_modified);
  g_test_add_func("/linediff/deleted", test_deleted);
  g_test_add_func("/linediff/hunks", test_hunks);
  g_test_add_func("/linediff/big", test_big);
  g_test_add_func("/linediff/gaps", test_gaps);
  return g_test_run();
}
