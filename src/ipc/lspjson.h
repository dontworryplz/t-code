/*
 * Just enough JSON for routing language server messages: finding a field by its path and
 * replacing its value, straight on the text. Nothing is parsed into a tree.
 */
#pragma once

#include <glib.h>

typedef struct {
    const char *start, *end; /* start == NULL: not there */
} Span;

/* The value at path, field names separated by dots, like "params.textDocument.uri". */
Span json_lookup(const char *json, const char *path);

/* A string value without its quotes and with simple escapes undone (\uXXXX stays); NULL when
 * there is no such string. */
char *json_lookup_string(const char *json, const char *path);

/* A copy of the span's text. */
char *json_text(Span s);

/* json with the value at path replaced by value; a copy of json when it has no such field. */
char *json_replace(const char *json, const char *path, const char *value);

/* text as a JSON string, in quotes. */
char *json_quote(const char *text);
