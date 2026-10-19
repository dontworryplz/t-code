/*
 * The language server's TextEdits ({"range": ..., "newText": ...}): applied to an open editor (a
 * completion's extra "#include", formatting, renaming) or to the text of a file that is not open.
 */
#pragma once

#include "app.h"

#include <json-glib/json-glib.h>

/* Applies a list of TextEdits to an editor as one undo step. Their ranges are all taken first, so
 * each edit lands where the server meant it, whatever the others do to the text. */
void edits_apply(Tab *tab, JsonNode *edits);

/* The same on a file's text (positions counted in UTF-16 units when utf16, else in bytes); returns
 * the new text. */
char *edits_apply_text(const char *text, JsonNode *edits, gboolean utf16);
