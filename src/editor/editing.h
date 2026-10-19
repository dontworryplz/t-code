/*
 * Editing commands on the lines around the cursor, like VS Code's: comment, duplicate, move and
 * delete lines, redo; and typing help: closing brackets and quotes, Enter between braces.
 */
#pragma once

#include "app.h"

/* Adds the typing help to a new editor (its keys come here before the view's). */
void editing_attach(Tab *tab);

/* Comments the selected lines out with the language's line comment ("// "), or back in when they
 * all are. Without a line comment (CSS, HTML), the language's block comment goes around them. */
void editor_toggle_comment(Tab *tab);

/* Copies the selected lines (or the cursor's line) below themselves. */
void editor_duplicate_lines(Tab *tab);

/* Moves the selected lines (or the cursor's line) one line down or up. */
void editor_move_lines(Tab *tab, gboolean down);

/* Deletes the selected lines (or the cursor's line). */
void editor_delete_lines(Tab *tab);

/* Redoes what Ctrl+Z undid (Ctrl+Y; Ctrl+Shift+Z is GtkSourceView's own). */
void editor_redo(Tab *tab);
