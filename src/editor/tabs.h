/*
 * Editor tabs, in two groups side by side (t->groups[0] and [1], each a GtkNotebook; the second
 * one is shown only while split). A tab is an editor (editor.h) or a Markdown preview
 * (preview.h). A file is never open in both groups at once: two editors would overwrite each
 * other's saves.
 */
#pragma once

#include "app.h"

/* Finding tabs ------------------------------------------------------------------------------- */

/* The tab a notebook page belongs to. */
Tab *tab_of(GtkWidget *page);

/* Tabs in both groups. */
int tab_count(App *t);

/* Tab 0 .. tab_count() - 1, the first group's first. */
Tab *tab_nth(App *t, int index);

/* The current tab of the active group (or else of the other one); NULL when none is open. */
Tab *current_tab(App *t);

/* The tab of that kind showing path, or NULL. */
Tab *find_tab(App *t, TabKind kind, const char *path);

/* The group a tab is in: 0 or 1. */
int tab_group(Tab *tab);

/* Whether an editor has unsaved changes. */
gboolean any_unsaved(App *t);

/* Adding, moving, closing -------------------------------------------------------------------- */

/* Adds page as a tab of the active group, right after its current tab, and switches to it. The
 * Tab is freed with the page. */
Tab *add_tab(App *t, TabKind kind, GtkWidget *page, const char *path);

/* The group's temporary tab (a click in the Files panel opened it), or NULL. */
Tab *temporary_tab(App *t, int group);

/* Keeps a temporary tab: the next click in the Files panel opens another tab instead of replacing
 * it. */
void keep_tab(Tab *tab);

/* Puts right (an old version) next to left in left's notebook page: one tab, both sides; it closes
 * with left. */
void join_tabs(Tab *left, Tab *right);

/* Takes a comparison apart: its right side closes, left is a tab of its own again. */
void part_tabs(Tab *left);

/* A tab's position in its group, and moving it to another one there. */
int tab_position(Tab *tab);
void set_tab_position(Tab *tab, int position);

/* Moves a tab (with its editor) to the end of the other group. */
void move_tab(Tab *tab, int group);

/* Takes the tab out of its group at once, unsaved changes or not; a comparison goes with both
 * sides. Closing an editor the normal way goes through
 * quit_editor(), which asks about unsaved changes. */
void remove_tab(Tab *tab);

/* Removes every tab without asking (a project switch, after asking). */
void close_all_tabs(App *t);

/* The label: the file name, "●" when unsaved, "Preview" for previews, in italics while temporary;
 * the path as tooltip. */
void update_tab_label(Tab *tab);

/* Focus -------------------------------------------------------------------------------------- */

/* Makes tab the current one of its group, that group the active one, and gives it the keyboard. */
void select_tab(Tab *tab);

/* The next (step 1) or previous (step -1) tab in the current group, going round. */
void step_tab(App *t, int step);

/* Gives the keyboard to the current tab, or to the file manager when there is none. */
void focus_current_tab(App *t);

/* Makes a group (0 or 1) the active one and focuses its tab; 1 only while split. */
void focus_group(App *t, int group);

/* The groups --------------------------------------------------------------------------------- */

/* Creates both groups and t->editor_area, which shows a hint while no tab is open. */
void create_groups(App *t);

/* Shows the tabs or the hint, whichever fits now. */
void update_editor_area(App *t);

/* Shows or hides the second group. Unsplitting moves its tabs to the first group. */
void set_split(App *t, gboolean split);
