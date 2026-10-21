/*
 * Small helpers for JSON messages, the language server's (LSP) and the debugger's (DAP): reading a
 * value at a dotted path, and quoting a string to build a message with g_strdup_printf().
 */
#pragma once

#include <json-glib/json-glib.h>

/* The node at a dotted path in node ("range.start.line", "items.0.label"), or NULL. */
JsonNode *json_at(JsonNode *node, const char *path);

/* The string / number at path, or NULL / fallback. */
const char *json_string_at(JsonNode *node, const char *path);
gint64 json_int_at(JsonNode *node, const char *path, gint64 fallback);

/* text as a JSON string, quotes included. */
char *json_quote(const char *text);
