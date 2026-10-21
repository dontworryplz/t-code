/*
 * The file tree's git colors, GLib-only: `git status --porcelain=v1 -z --untracked-files=all
 * --ignored=matching` read into a table of states, with their colors and letters. A state is
 * 0 or one of M(odified) A(dded) U(ntracked) D(eleted) C(onflict) I(gnored).
 */
#pragma once

#include <glib.h>

/* path -> state for the output of git status run in the work tree top; folders hold the strongest
 * state inside them (a deleted file makes its folder modified). Free with g_hash_table_unref. */
GHashTable *git_marks_parse(const char *top, const char *out, gsize len);

/* The state shown for a file or folder: its own, a folder's from what is inside, or ignored when it
 * is inside an ignored folder. marks may be NULL (outside a repository). */
char git_marks_state(GHashTable *marks, const char *path);

/* The color for a state ("#cccccc" for none). */
const char *git_marks_color(char state);

/* The letter at the end of a file's row: "M", "A", "U", "D" or "!" (a conflict); NULL for none. */
const char *git_marks_badge(char state);
