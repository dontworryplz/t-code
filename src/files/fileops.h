/*
 * The file tree's file operations and rules, GLib-only (tested without a display, also under ASan):
 * which names show, their order, creating, renaming, deleting, and copying or moving into a folder
 * like VS Code ("name copy.ext" when the name is taken). Errors say what went wrong in *error.
 */
#pragma once

#include <glib.h>

/* Whether a folder entry shows: dotfiles only with show_hidden, version control folders never. */
gboolean files_shown(const char *name, gboolean show_hidden);

/* The tree's order: folders first, then names without regard to case (ties: byte order). */
int files_compare(const char *a, gboolean a_dir, const char *b, gboolean b_dir);

/* Whether a file is source code (C, C++ and other programming languages, by extension): a click in
 * the Files panel opens those in a temporary tab; READMEs, Makefiles, configs open for good. */
gboolean files_is_source(const char *name);

/* Whether path is folder or inside it. */
gboolean files_inside(const char *path, const char *folder);

/* path relative to root ("" for root itself; path when it is outside). Points into path. */
const char *files_relative(const char *path, const char *root);

/* dir/name, or "name copy.ext", "name copy 2.ext", ... in dir when that is taken. */
char *files_free_name(const char *dir, const char *name, gboolean is_dir);

/* Creates name in dir: the folders on the way when it has "/" in it, a folder when it ends in "/" or
 * folder is set, else an empty file. The new path goes to *made. */
gboolean files_create(const char *dir, const char *name, gboolean folder, char **made, GError **error);

/* Renames path to name in the same folder; refuses a "/" and a name that is taken. */
gboolean files_rename(const char *path, const char *name, char **dest, GError **error);

/* Deletes path, a folder with everything in it (symlinks themselves, not what they point to). */
gboolean files_delete(const char *path, GError **error);

/* Copies or moves (cut) src into dir under a free name (*dest). FALSE without an error when there is
 * nothing to do (moving into the folder it is in). A move across file systems copies, then removes. */
gboolean files_put(const char *src, const char *dir, gboolean cut, char **dest, GError **error);
