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

/* The marks (guint8 LineMark, one per line of new) for new compared with old. Lines removed at the
 * very end mark the last line. */
GArray *line_diff(const char *old, const char *new);
