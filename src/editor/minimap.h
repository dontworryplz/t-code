/*
 * The minimap next to each editor: the saved file as thin colored bars, clangd's problem lines
 * across it, and the lines the editor shows shaded. Clicking or dragging it scrolls the editor.
 */
#pragma once

#include "app.h"

/* The colors; once, before the first minimap is drawn. */
void minimap_init_colors(void);

/* A new minimap for an editor's view. */
GtkWidget *minimap_new(EditorView *v);

/* micro's problem lines ("e12,w30": an error on line 12, a warning on line 30; "-" for none). */
void minimap_set_problems(Tab *tab, const char *spec);
