/*
 * Breakpoints in the editor: a red dot left of the line numbers, put and taken away by a click there
 * (a faint one shows under the pointer) or F9. They move with the text as it is edited, and stay
 * for files that are not open (t->debug.breakpoints, kept in the session per project). While the
 * program is stopped, the line it stopped at has a yellow arrow and a yellow background.
 */
#pragma once

#include "app.h"

typedef struct Breakpoints Breakpoints;

/* Puts the dots' column into tab's gutter (editorview.c). */
void breakpoints_attach(Tab *tab);

/* Frees it once the view is gone; NULL is fine. */
void breakpoints_free(Breakpoints *b);

/* The file was loaded (again): its breakpoints, and the stopped line, go into the text. */
void breakpoints_loaded(Tab *tab);

/* Puts a breakpoint on line (0-based), or takes it away. */
void breakpoint_toggle(Tab *tab, int line);

/* The same on the cursor's line (F9). */
void breakpoint_toggle_at_cursor(Tab *tab);

/* The breakpoints' lines in path (1-based, ascending), or NULL when it has none. Owned by t. */
GArray *breakpoints_in(App *t, const char *path);

/* Takes every breakpoint away. */
void breakpoints_remove_all(App *t);

/* Every breakpoint as "line:path", for the session (free with g_strfreev); NULL when there are none. */
char **breakpoints_save(App *t);

/* Puts back what breakpoints_save() gave (another project's go); NULL: none. */
void breakpoints_restore(App *t, char **saved);

/* The program stopped at line (1-based) of path: the editors showing it mark it. path NULL: it runs
 * again (or ended), the mark goes. */
void breakpoints_show_stop(App *t, const char *path, int line);

/* Draws the stopped line's background (guides.c, under the text). */
void breakpoints_draw(Tab *tab, GtkTextView *view, GtkSnapshot *snapshot);
