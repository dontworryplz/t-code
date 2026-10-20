/*
 * Where clangd gets a file's compile flags (see compiledb.h).
 */
#include "lsp/compiledb.h"

#include <string.h>

/* What clangd looks for in each folder, in its order; .clangd last (it may add the flags). */
static const char *const databases[] = { "compile_commands.json", "build/compile_commands.json", "compile_flags.txt",
                                         ".clangd", NULL };

/* The names a misspelled database comes down to (lower case, "-" and " " as "_"). */
static const char *const near_names[] = { "compile_commands.json",
                                          "compile_command.json",
                                          "compile_flags.txt",
                                          "compile_flag.txt",
                                          "compile_flags",
                                          "compile_commands",
                                          NULL };

char *compile_db_find(const char *file)
{
  char *folder = g_path_get_dirname(file);

  for (;;) {
    for (int i = 0; databases[i]; i++) {
      char *path = g_build_filename(folder, databases[i], NULL);
      if (g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
        g_free(folder);
        return path;
      }
      g_free(path);
    }
    char *parent = g_path_get_dirname(folder);
    gboolean top = strcmp(parent, folder) == 0;
    g_free(folder);
    folder = parent;
    if (top)
      break;
  }
  g_free(folder);
  return NULL;
}

/* Whether name looks like a database but is not spelled as one. */
static gboolean near_miss(const char *name)
{
  char *normal = g_ascii_strdown(name, -1);
  gboolean near = FALSE;

  g_strdelimit(normal, "- ", '_');
  for (int i = 0; !near && near_names[i]; i++)
    near = strcmp(normal, near_names[i]) == 0;
  g_free(normal);
  return near && strcmp(name, "compile_commands.json") != 0 && strcmp(name, "compile_flags.txt") != 0;
}

char *compile_db_misnamed(const char *file, const char *root)
{
  char *folder = g_path_get_dirname(file), *found = NULL;

  while (!found) {
    GDir *dir = g_dir_open(folder, 0, NULL);
    const char *name;
    while (dir && !found && (name = g_dir_read_name(dir)))
      if (near_miss(name))
        found = g_build_filename(folder, name, NULL);
    if (dir)
      g_dir_close(dir);

    char *parent = g_path_get_dirname(folder);
    gboolean last =
        !root || strcmp(folder, root) == 0 || strcmp(parent, folder) == 0 || !g_str_has_prefix(folder, root);
    g_free(folder);
    folder = parent;
    if (last)
      break;
  }
  g_free(folder);
  return found;
}

gboolean compile_db_system_headers(const char *sysroot, gboolean cxx)
{
  char *c = g_build_filename(sysroot, "usr", "include", "stdio.h", NULL);
  char *cxx_dir = g_build_filename(sysroot, "usr", "include", "c++", NULL);
  gboolean there = g_file_test(c, G_FILE_TEST_EXISTS) && (!cxx || g_file_test(cxx_dir, G_FILE_TEST_IS_DIR));

  g_free(c);
  g_free(cxx_dir);
  return there;
}
