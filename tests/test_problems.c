/* Tests for the Problems panel in src/lsp/problems.c */
#include "lsp/problems.h"
#include "lsp/diagnostics.h"
#include "editor/editor.h"
#include "editor/tabs.h"

#include <glib.h>

static int open_file_at_called = 0;
static char *last_opened_path = NULL;
static int last_opened_line = 0;
static int last_opened_col = 0;

void open_file_at(App *t, const char *path, int line, int column)
{
  (void)t;
  open_file_at_called++;
  g_free(last_opened_path);
  last_opened_path = g_strdup(path);
  last_opened_line = line;
  last_opened_col = column;
}

void show_sidebar_page(App *t, const char *page)
{
  (void)t;
  (void)page;
}

static void test_problems_empty(void)
{
  App app = { 0 };
  problems_update(&app);
  /* Should not crash with null/empty */
}

int main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/problems/empty", test_problems_empty);
  return g_test_run();
}
