/*
 * What changed between two versions of a file, line by line, for the editor's diff gutter (GLib
 * only, so tests/ can run it without a display).
 */
#pragma once

#include <glib.h>

/* A line's mark in the new version. */
typedef enum {
  LINE_SAME,
  LINE_ADDED,
  LINE_MODIFIED,
  LINE_DELETED_ABOVE, /* lines of the old version were removed right before it */
} LineMark;

/* An empty vertical space (in lines) needed to align two versions side by side. */
typedef struct {
  int line;       /* 0-based line in the buffer; gap is below this line, or above line 0 when above is TRUE */
  int count;      /* number of missing lines in this gap */
  gboolean above; /* TRUE when lines are missing before line 0 */
} DiffGap;

/* The marks (guint8 LineMark, one per line of new) for new compared with old. Lines removed at the
 * very end mark the last line. */
GArray *line_diff(const char *old, const char *new);

/* The DiffGap list for new (for_old_side == FALSE: gaps where lines were deleted from old) or for
 * old (for_old_side == TRUE: gaps where lines were added in new). */
GArray *line_diff_gaps(const char *old, const char *new, gboolean for_old_side);
