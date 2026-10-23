/*
 * Splitting a Git diff into independently applicable hunks.
 */
#include "git/patch.h"

#include <string.h>

GPtrArray *git_patch_hunks(const char *diff)
{
  GPtrArray *patches = g_ptr_array_new_with_free_func(g_free);
  const char *prefix_end = NULL, *hunk_start = NULL;

  for (const char *line = diff; line && *line;) {
    const char *next = strchr(line, '\n');
    next = next ? next + 1 : line + strlen(line);
    if (g_str_has_prefix(line, "@@ ")) {
      if (!prefix_end)
        prefix_end = line;
      if (hunk_start) {
        GString *patch = g_string_new_len(diff, prefix_end - diff);
        g_string_append_len(patch, hunk_start, line - hunk_start);
        g_ptr_array_add(patches, g_string_free(patch, FALSE));
      }
      hunk_start = line;
    }
    line = next;
  }
  if (hunk_start) {
    GString *patch = g_string_new_len(diff, prefix_end - diff);
    g_string_append(patch, hunk_start);
    g_ptr_array_add(patches, g_string_free(patch, FALSE));
  }
  return patches;
}
