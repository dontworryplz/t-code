/*
 * Splitting a Git diff into independently applicable hunks.
 */
#pragma once

#include <glib.h>

/* One full-file patch per hunk, preserving the Git file header and hunk context. */
GPtrArray *git_patch_hunks(const char *diff);
