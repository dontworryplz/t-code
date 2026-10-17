/*
 * Just enough JSON for ipc/lsp.c (see lspjson.h): finding fields by name and replacing their
 * values, without parsing the whole message.
 */
#include "ipc/lspjson.h"

#include <string.h>

/* Past blanks. */
static const char *skip_space(const char *p, const char *end)
{
    while (p < end && g_ascii_isspace(*p))
        p++;
    return p;
}

/* Past a string; p is at its opening quote. */
static const char *skip_string(const char *p, const char *end)
{
    for (p++; p < end; p++) {
        if (*p == '\\')
            p++;
        else if (*p == '"')
            return p + 1;
    }
    return end;
}

/* Past one value; stops at the ',' or the closing bracket after a number or literal. */
static const char *skip_value(const char *p, const char *end)
{
    int depth = 0;

    while (p < end) {
        char c = *p;
        if (c == '"') {
            p = skip_string(p, end);
        } else if (c == '{' || c == '[') {
            depth++;
            p++;
        } else if (c == '}' || c == ']') {
            if (depth == 0)
                return p;
            depth--;
            p++;
        } else if (c == ',' && depth == 0) {
            return p;
        } else {
            p++;
        }
        if (depth == 0 && (c == '"' || c == '}' || c == ']'))
            return p;
    }
    return end;
}

/* The value of key in object (which must be a JSON object), or a Span with start NULL. */
static Span field(Span object, const char *key)
{
    Span none = { NULL, NULL };
    const char *p = skip_space(object.start, object.end), *end = object.end;
    gsize key_len = strlen(key);

    if (!p || p >= end || *p != '{')
        return none;
    for (p++;;) {
        p = skip_space(p, end);
        if (p >= end || *p != '"')
            return none;
        const char *name = p + 1;
        p = skip_string(p, end);
        gboolean match = (gsize)(p - 1 - name) == key_len && memcmp(name, key, key_len) == 0;
        p = skip_space(p, end);
        if (p >= end || *p != ':')
            return none;
        const char *value = skip_space(p + 1, end);
        p = skip_value(value, end);
        if (match) {
            const char *value_end = p;
            while (value_end > value && g_ascii_isspace(value_end[-1]))
                value_end--;
            return (Span){ value, value_end };
        }
        p = skip_space(p, end);
        if (p >= end || *p != ',')
            return none;
        p++;
    }
}

Span json_lookup(const char *json, const char *path)
{
    Span span = { json, json + strlen(json) };
    char **keys = g_strsplit(path, ".", -1);

    for (int i = 0; keys[i] && span.start; i++)
        span = field(span, keys[i]);
    g_strfreev(keys);
    return span;
}

char *json_lookup_string(const char *json, const char *path)
{
    Span s = json_lookup(json, path);
    GString *out;

    if (!s.start || s.end - s.start < 2 || *s.start != '"')
        return NULL;
    out = g_string_new(NULL);
    for (const char *p = s.start + 1; p < s.end - 1; p++) {
        if (*p == '\\' && p + 1 < s.end - 1) {
            p++;
            switch (*p) {
            case 'n': g_string_append_c(out, '\n'); break;
            case 't': g_string_append_c(out, '\t'); break;
            case 'u': g_string_append(out, "\\u"); break;
            default: g_string_append_c(out, *p); break;
            }
        } else {
            g_string_append_c(out, *p);
        }
    }
    return g_string_free(out, FALSE);
}

char *json_text(Span s)
{
    return g_strndup(s.start, s.end - s.start);
}

char *json_replace(const char *json, const char *path, const char *value)
{
    Span s = json_lookup(json, path);

    if (!s.start)
        return g_strdup(json);
    return g_strdup_printf("%.*s%s%s", (int)(s.start - json), json, value, s.end);
}

char *json_quote(const char *text)
{
    GString *out = g_string_new("\"");

    for (const char *p = text; *p; p++) {
        if (*p == '"' || *p == '\\')
            g_string_append_c(out, '\\');
        g_string_append_c(out, *p);
    }
    g_string_append_c(out, '"');
    return g_string_free(out, FALSE);
}
