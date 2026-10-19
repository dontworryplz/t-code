/*
 * Tests for src/editor/detect.c: a file's own indentation and whether a header is C++.
 */
#include "editor/detect.h"

/* Runs detect_indent on text starting from tabs = FALSE, width = 4 (the settings). */
static gboolean indent(const char *text, gboolean *tabs, int *width)
{
  *tabs = FALSE;
  *width = 4;
  return detect_indent(text, -1, tabs, width);
}

static void test_spaces(void)
{
  gboolean tabs;
  int width;

  g_assert_true(indent("int f(void)\n{\n  if (x) {\n    y();\n  }\n}\n", &tabs, &width));
  g_assert_false(tabs);
  g_assert_cmpint(width, ==, 2);

  g_assert_true(indent("void f()\n{\n        a();\n        if (b)\n                c();\n}\n", &tabs, &width));
  g_assert_false(tabs);
  g_assert_cmpint(width, ==, 8);
}

static void test_tabs(void)
{
  gboolean tabs;
  int width;

  g_assert_true(indent("void f()\n{\n\ta();\n\tif (b)\n\t\tc();\n}\n", &tabs, &width));
  g_assert_true(tabs);
}

/* Block comments (" * ") and blank lines say nothing; without indented lines the settings stay. */
static void test_nothing_to_go_by(void)
{
  gboolean tabs;
  int width;

  g_assert_false(indent("/*\n * A comment.\n */\nint x;\n\n\nint y;\n", &tabs, &width));
  g_assert_false(tabs);
  g_assert_cmpint(width, ==, 4);
  g_assert_false(indent("", &tabs, &width));
}

/* Windows line ends: a line of "\r" alone is blank, not indented. */
static void test_crlf(void)
{
  gboolean tabs;
  int width;

  g_assert_true(indent("a\r\n  b\r\n    c\r\n\r\n  d\r\n", &tabs, &width));
  g_assert_cmpint(width, ==, 2);
}

static void test_cxx_headers(void)
{
  g_assert_true(is_cxx_text("#pragma once\n\nclass Widget {\npublic:\n    int x;\n};\n", -1));
  g_assert_true(is_cxx_text("#include <vector>\nstd::vector<int> v;\n", -1));
  g_assert_true(is_cxx_text("namespace app {\n}\n", -1));
  g_assert_true(is_cxx_text("template <typename T>\nT max(T a, T b);\n", -1));
  g_assert_false(is_cxx_text("#pragma once\n#include <stdio.h>\nstruct point { int x, y; };\n", -1));
  /* C23 has constexpr; "class" inside a C name or comment is no class. */
  g_assert_false(is_cxx_text("constexpr int size = 4;\nint subclass;\n/* a class of errors */\n", -1));
}

int main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/detect/spaces", test_spaces);
  g_test_add_func("/detect/tabs", test_tabs);
  g_test_add_func("/detect/nothing-to-go-by", test_nothing_to_go_by);
  g_test_add_func("/detect/crlf", test_crlf);
  g_test_add_func("/detect/cxx-headers", test_cxx_headers);
  return g_test_run();
}
