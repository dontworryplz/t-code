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

/* An empty vertical space needed to align two versions side by side, in the unit of the heights
 * given to line_diff_gaps() (lines when none: t-code gives pixels). */
typedef struct {
  int line;       /* 0-based line in the buffer; the space is below it, or above line 0 when above is TRUE */
  int count;      /* for the other side's missing lines (drawn as a block) */
  int pad;        /* right below the line: the line it faces wraps to more rows (left blank) */
  gboolean above; /* TRUE when lines are missing before line 0 */
} DiffGap;

/* The marks (guint8 LineMark, one per line of new) for new compared with old. Lines removed at the
 * very end mark the last line. */
GArray *line_diff(const char *old, const char *new);

/* The DiffGap list (in line order, at most one per line and side) for new (for_old_side == FALSE:
 * gaps where lines were deleted from old) or for old (for_old_side == TRUE: gaps where lines were
 * added in new). old_heights / new_heights: each line's height (pixels, with word wrap; NULL: 1 each);
 * a line both have that is taller on the other side gets the difference as a pad. */
GArray *line_diff_gaps(const char *old, const char *new, const int *old_heights, const int *new_heights,
                       gboolean for_old_side);

/* How many lines text has, counted like the functions above (a last line break ends a line). */
guint line_count(const char *text);

/* What changed in a line, side by side: a whole changed line (end == -1), or the bytes start..end
 * of it that the other version does not have (its changed words). */
typedef struct {
  int line;
  int start, end;
} DiffSpan;

/* For new (for_old_side == FALSE) or old: each changed line (end -1) followed by its changed words.
 * In a stretch of changes the lines are paired in order and compared word by word (words, runs of
 * spaces, other characters); a line without a pair is all changed. */
GArray *line_diff_spans(const char *old, const char *new, gboolean for_old_side);
