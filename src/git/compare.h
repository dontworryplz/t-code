/*
 * Comparing a file with an older version from git, side by side: the working file on the left,
 * the old version (read-only, from a temporary file) on the right, each gutter marking what
 * differs from the other. Also File History, which lists a file's commits to compare with.
 */
#pragma once

#include "app.h"

/* compare_revisions(): the staged version (the index) instead of a commit's. */
#define INDEX_REV ":0"

/* The file the comparison commands work on: the current editor's, or for an old version, the
 * working file it is compared with. NULL without an editor. */
const char *comparable_path(App *t);

/* path next to its version in the last commit (Ctrl+Alt+D). */
void compare_with_head(App *t, const char *path);

/* path next to its staged version: what is not staged yet. */
void compare_with_index(App *t, const char *path);

/* Lists path's commits in the picker; the chosen one is compared with the working file (Ctrl+Alt+H). */
void file_history(App *t, const char *path);

/* Shows a file in two versions side by side: new_rev's on the left, old_rev's on the right. Paths
 * are relative to the work tree top (they differ after a rename); a NULL rev or path is a missing
 * side (the file was added or deleted). */
void compare_revisions(App *t, const char *old_rev, const char *old_rel, const char *new_rev, const char *new_rel);

/* Closes the old versions of the current file. */
void stop_comparing(App *t);

/* An editor closes: it and the tab it scrolled together with (a comparison's other side) stop
 * moving each other. Its adjustments may outlive it, and the other side's would reach a freed tab. */
void unsync_scroll(Tab *tab);

/* An old version's tab closed: the working file's gutter goes back to following HEAD, unless
 * another old version still compares with it. */
void forget_comparison(Tab *old_version);

/* Deletes the temporary folder of old versions (t->temp_dir), when t-code quits. */
void remove_temp_dir(App *t);
