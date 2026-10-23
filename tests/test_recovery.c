/* Tests for crash recovery journaling in src/editor/recovery.c */
#include "editor/recovery.h"
#include "editor/editor.h"
#include "editor/editorview.h"

#include <glib.h>
#include <glib/gstdio.h>

void status_say(App *t, const char *message)
{
  (void)t;
  (void)message;
}

static void test_recovery_lifecycle(void)
{
  char *tmp = g_dir_make_tmp("t-code-test-rec-XXXXXX", NULL);
  char *filepath = g_build_filename(tmp, "test.c", NULL);
  g_file_set_contents(filepath, "int original = 1;\n", -1, NULL);

  /* Discard should be safe when no journal exists */
  recovery_discard_snapshot(filepath);

  /* Check hash/journal path */
  char *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA1, filepath, -1);
  char *journal = g_build_filename(g_get_user_cache_dir(), "t-code", "recovery", hash, NULL);
  g_assert_false(g_file_test(journal, G_FILE_TEST_EXISTS));

  /* Write a simulated snapshot directly */
  gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  char *content =
      g_strdup_printf("TCODE_RECOVERY_V1 %" G_GINT64_FORMAT " %s\nint recovered = 42;\n", now + 10, filepath);
  char *journal_dir = g_path_get_dirname(journal);
  g_mkdir_with_parents(journal_dir, 0700);
  g_free(journal_dir);
  g_file_set_contents(journal, content, -1, NULL);
  g_free(content);

  g_assert_true(g_file_test(journal, G_FILE_TEST_EXISTS));

  /* Discard snapshot */
  recovery_discard_snapshot(filepath);
  g_assert_false(g_file_test(journal, G_FILE_TEST_EXISTS));

  g_free(journal);
  g_free(hash);
  g_unlink(filepath);
  g_free(filepath);
  g_rmdir(tmp);
  g_free(tmp);
}

int main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/recovery/lifecycle", test_recovery_lifecycle);
  return g_test_run();
}
