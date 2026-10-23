/* Tests for version comparisons in src/update/update.c */
#include "update/update.h"

#include <glib.h>

static void test_newer_version(void)
{
  /* Standard semver increments */
  g_assert_true(newer_version("0.3.0", "0.4.0"));
  g_assert_false(newer_version("0.4.0", "0.3.0"));
  g_assert_true(newer_version("0.3.0", "0.3.1"));
  g_assert_false(newer_version("0.3.1", "0.3.0"));
  g_assert_true(newer_version("0.9.9", "1.0.0"));
  g_assert_false(newer_version("1.0.0", "0.9.9"));

  /* Identical versions */
  g_assert_false(newer_version("0.3.0", "0.3.0"));
  g_assert_false(newer_version("1.0", "1.0"));

  /* Leading 'v' prefixes */
  g_assert_true(newer_version("0.3.0", "v0.4.0"));
  g_assert_false(newer_version("v0.4.0", "0.3.0"));
  g_assert_true(newer_version("v0.3.0", "v0.4.0"));
  g_assert_false(newer_version("v0.4.0", "v0.3.0"));

  /* Pre-release suffixes */
  g_assert_false(newer_version("0.3.0-rc1", "0.3.0"));
  g_assert_false(newer_version("0.3.0", "0.3.0-rc1"));
  g_assert_true(newer_version("0.3.0-rc1", "0.3.1"));

  /* Varying component lengths */
  g_assert_true(newer_version("0.3", "0.4.0"));
  g_assert_true(newer_version("0.3.0", "0.3.0.1"));
  g_assert_false(newer_version("0.3.0.1", "0.3.0"));
}

int main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/update/newer-version", test_newer_version);
  return g_test_run();
}
