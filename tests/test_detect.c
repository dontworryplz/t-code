/*
 * Tests for src/editor/detect.c (a file's own indentation, whether a header is C++) and
 * src/editor/indentconf.c (what .editorconfig and .clang-format say about it).
 */
#include "editor/detect.h"
#include "editor/indentconf.h"

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

/* A file with text under folder, its folders made. */
static void put(const char *folder, const char *name, const char *text)
{
  char *path = g_build_filename(folder, name, NULL), *parent = g_path_get_dirname(path);

  g_mkdir_with_parents(parent, 0700);
  g_assert_true(g_file_set_contents(path, text, -1, NULL));
  g_free(parent);
  g_free(path);
}

/* The indentation config says for root/name. */
static const char *config_of(const char *root, const char *name, IndentStyle *style)
{
  char *file = g_build_filename(root, name, NULL);
  const char *from = indent_from_config(file, style);

  g_free(file);
  return from;
}

/* .editorconfig: the sections matching the file, later ones and nearer files winning, up to root. */
static void test_editorconfig(void)
{
  char *root = g_dir_make_tmp("t-code-indent-XXXXXX", NULL);
  IndentStyle style;

  put(root, ".editorconfig",
      "root = true\n\n[*]\nindent_style = space\nindent_size = 4\n\n"
      "[Makefile]\nindent_style = tab\n\n[*.{c,h}]\nindent_size = 2\n\n"
      "[lib/**.py]\nindent_size = 8\n\n[vendor/*]\nindent_style = tab\ntab_width = 3\nindent_size = tab\n");
  g_assert_cmpstr(config_of(root, "notes.txt", &style), ==, ".editorconfig");
  g_assert_cmpint(style.tabs, ==, 0);
  g_assert_cmpint(style.width, ==, 4);
  config_of(root, "src/main.c", &style); /* no "/" in the glob: any folder */
  g_assert_cmpint(style.width, ==, 2);
  config_of(root, "Makefile", &style);
  g_assert_cmpint(style.tabs, ==, 1);
  g_assert_cmpint(style.width, ==, 4);
  config_of(root, "lib/a/b.py", &style);
  g_assert_cmpint(style.width, ==, 8);
  config_of(root, "vendor/x.txt", &style); /* indent_size = tab: tab_width */
  g_assert_cmpint(style.tabs, ==, 1);
  g_assert_cmpint(style.width, ==, 3);
  config_of(root, "vendor/deep/x.txt", &style); /* "*" stays in its folder */
  g_assert_cmpint(style.tabs, ==, 0);

  /* A nearer one wins; one above "root = true" is not read. */
  put(root, "sub/.editorconfig", "[*.c]\nindent_size = 3\n");
  config_of(root, "sub/x.c", &style);
  g_assert_cmpint(style.width, ==, 3);
  g_assert_cmpint(style.tabs, ==, 0);
  put(root, "other/.editorconfig", "# own rules\nroot = true\n[*.c]\nindent_style = tab\n");
  config_of(root, "other/x.c", &style);
  g_assert_cmpint(style.tabs, ==, 1);
  g_assert_cmpint(style.width, ==, 0); /* not said */
  g_free(root);
}

/* .clang-format: for C and C++ files only, its Cpp section, with the styles' defaults. */
static void test_clang_format(void)
{
  char *root = g_dir_make_tmp("t-code-indent-XXXXXX", NULL);
  IndentStyle style;

  put(root, ".clang-format",
      "---\nLanguage: JavaScript\nIndentWidth: 8\n---\nLanguage: Cpp\nBasedOnStyle: LLVM\nIndentWidth: 3 # ours\n"
      "UseTab: ForIndentation\n...\n");
  g_assert_cmpstr(config_of(root, "a.cpp", &style), ==, ".clang-format");
  g_assert_cmpint(style.tabs, ==, 1);
  g_assert_cmpint(style.width, ==, 3);
  g_assert_null(config_of(root, "a.py", &style));
  g_assert_cmpint(style.tabs, ==, -1);
  g_assert_cmpint(style.width, ==, 0);

  put(root, "webkit/.clang-format", "BasedOnStyle: WebKit\n");
  config_of(root, "webkit/a.h", &style);
  g_assert_cmpint(style.tabs, ==, 0);
  g_assert_cmpint(style.width, ==, 4);

  /* .editorconfig first, .clang-format for the rest. */
  put(root, "both/.editorconfig", "root = true\n[*]\nindent_style = space\n");
  g_assert_cmpstr(config_of(root, "both/a.c", &style), ==, ".editorconfig");
  g_assert_cmpint(style.tabs, ==, 0);
  g_assert_cmpint(style.width, ==, 3);
  g_free(root);
}

int main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/detect/spaces", test_spaces);
  g_test_add_func("/detect/tabs", test_tabs);
  g_test_add_func("/detect/nothing-to-go-by", test_nothing_to_go_by);
  g_test_add_func("/detect/crlf", test_crlf);
  g_test_add_func("/detect/cxx-headers", test_cxx_headers);
  g_test_add_func("/detect/editorconfig", test_editorconfig);
  g_test_add_func("/detect/clang-format", test_clang_format);
  return g_test_run();
}
