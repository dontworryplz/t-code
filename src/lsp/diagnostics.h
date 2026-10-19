/*
 * The language server's diagnostics in an editor: errors underlined with a red wave, warnings
 * yellow, notes blue, like VS Code; their counts in the status bar, their messages when the
 * pointer rests on them (lsp/hover.c).
 */
#pragma once

#include "app.h"

#include <json-glib/json-glib.h>

/* Shows a publishDiagnostics' list (replacing the last one). */
void diagnostics_show(Tab *tab, JsonNode *diagnostics);

/* The messages of the diagnostics at iter, one per line ("error: ..."), or NULL. markup gets Pango
 * markup with each severity in its color. */
char *diagnostics_at(Tab *tab, const GtkTextIter *iter, gboolean markup);

/* The lines with errors (and with warnings), 0-based, for the minimap's marks. */
void diagnostics_lines(Tab *tab, GArray *errors, GArray *warnings);

/* Forgets them all (the editor goes). */
void diagnostics_clear(Tab *tab);
