/*
 * VS Code's indent guides over an editor's terminal: a line at each indentation level, and the
 * one of the block around the cursor in its bracket pair color.
 */
#pragma once

#include "app.h"

/* The colors; once, before the first guides are drawn. */
void guides_init_colors(void);

/* Lays the guides over an editor's terminal (overlay holds it); they redraw when its text changes. */
void guides_attach(EditorView *v, GtkOverlay *overlay);
