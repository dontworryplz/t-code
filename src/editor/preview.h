/*
 * Markdown preview tabs (Ctrl+Shift+V, or "Open Preview" in the file manager): md4c turns the
 * file into HTML and WebKit shows it, styled like VS Code's preview. It follows the file as it is
 * saved.
 */
#pragma once

#include "app.h"

/* Whether path's extension is a Markdown one (.md, .markdown, ...). */
gboolean is_markdown(const char *path);

/* Opens a preview of path in a new tab, or switches to the one that shows it already. */
void open_preview(App *t, const char *path);

/* The preview's zoom for the window's text size. */
void preview_set_zoom(Tab *tab, double scale);

/* Frees a preview; its tab is going. */
void preview_free(Preview *preview);
