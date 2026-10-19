/*
 * The diff gutter right of the line numbers, like VS Code's: green for added lines, blue for
 * changed ones, a red mark where lines were deleted. It compares with the last commit (HEAD,
 * followed as new commits come), or in a comparison with an older version (a commit, a file).
 */
#pragma once

#include "app.h"

typedef struct DiffGutter DiffGutter;

/* Adds the gutter to a new editor; it compares with HEAD once the file is loaded. */
void diff_gutter_attach(Tab *tab);

/* The file was loaded (again): its base is read again too. */
void diff_gutter_loaded(Tab *tab);

/* Compares with HEAD, following new commits (the default). */
void diff_gutter_follow_head(Tab *tab);

/* Compares with the file as it is in commit rev. */
void diff_gutter_compare_rev(Tab *tab, const char *rev);

/* Compares with another file (an old version's copy, the working file); NULL: no marks. */
void diff_gutter_compare_file(Tab *tab, const char *path);

/* HEAD moved (a commit, a checkout): the editors following it read their base again. */
void diff_gutter_head_moved(App *t);

/* Pairs two tabs in a side-by-side comparison (new_tab on the left, old_tab on the right) so
 * missing lines open synchronized gap rows (red for deleted lines on the left, blank for newly
 * added lines on the right). Pass NULL for old_tab to unpair new_tab. */
void diff_gutter_pair(Tab *new_tab, Tab *old_tab);

/* Gap pixels above (*above_px) and below (*below_px) `line` in a side-by-side comparison;
 * returns TRUE when `tab` is the new/working side (gaps are red deleted blocks). */
gboolean diff_gutter_line_gaps(Tab *tab, int line, int *above_px, int *below_px);

/* Frees an editor's gutter state. NULL is fine. */
void diff_gutter_free(DiffGutter *gutter);
