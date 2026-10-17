/* The Search panel's command lines and output parsing; see searchmatch.h. GLib only. */
#include "search/searchmatch.h"

#include <stdlib.h>
#include <string.h>

#define SNIPPET_BEFORE 30 /* characters before a match shown in full; more are cut to... */
#define SNIPPET_KEEP 24   /* ...this many after a "…" */
#define SNIPPET_AFTER 200 /* characters after the match */

/* "a, b/,./c" -> { "a", "b", "c" }: blanks around and empty ones dropped, "./" and a trailing
 * "/" removed. */
static char **split_globs(const char *list)
{
    GPtrArray *globs = g_ptr_array_new();
    char **parts = g_strsplit(list ? list : "", ",", -1);

    for (int i = 0; parts[i]; i++) {
        char *glob = g_strstrip(parts[i]);
        gsize len;
        while (g_str_has_prefix(glob, "./"))
            glob += 2;
        len = strlen(glob);
        while (len > 1 && glob[len - 1] == '/')
            glob[--len] = '\0';
        if (*glob && strcmp(glob, "/") != 0)
            g_ptr_array_add(globs, g_strdup(glob));
    }
    g_strfreev(parts);
    g_ptr_array_add(globs, NULL);
    return (char **)g_ptr_array_free(globs, FALSE);
}

/* Appends a copy of arg to argv. */
static void add(GPtrArray *argv, const char *arg)
{
    g_ptr_array_add(argv, g_strdup(arg));
}

char **search_argv(const SearchQuery *q, gboolean ripgrep)
{
    GPtrArray *argv = g_ptr_array_new();
    char **include = split_globs(q->include), **exclude = split_globs(q->exclude);

    if (ripgrep) {
        /* Sorted, so files come in a steady order and each one's lines together. */
        static const char *const fixed[] = { "rg", "--line-number", "--no-heading", "--color", "never", "--null",
                                             "--sort", "path", "--max-columns", "1000", "--max-columns-preview" };
        for (gsize i = 0; i < G_N_ELEMENTS(fixed); i++)
            add(argv, fixed[i]);
        add(argv, q->match_case ? "--case-sensitive" : "--ignore-case");
        if (!q->regex)
            add(argv, "--fixed-strings");
        if (q->whole_word)
            add(argv, "--word-regexp");
        /* A folder name (no wildcards in its last part) includes what is in it, as in VS Code. */
        for (int i = 0; include[i]; i++) {
            const char *last = strrchr(include[i], '/');
            add(argv, "--glob");
            add(argv, include[i]);
            if (!strpbrk(last ? last + 1 : include[i], "*?[{")) {
                add(argv, "--glob");
                g_ptr_array_add(argv, g_strconcat(include[i], "/**", NULL));
            }
        }
        for (int i = 0; exclude[i]; i++) {
            add(argv, "--glob");
            g_ptr_array_add(argv, g_strconcat("!", exclude[i], NULL));
        }
    } else {
        static const char *const fixed[] = { "grep", "-rnIZ", "--color=never", "--exclude-dir=.git",
                                             "--exclude-dir=build" };
        for (gsize i = 0; i < G_N_ELEMENTS(fixed); i++)
            add(argv, fixed[i]);
        if (!q->match_case)
            add(argv, "-i");
        add(argv, q->regex ? "-E" : "-F");
        if (q->whole_word)
            add(argv, "-w");
        /* grep matches file and folder names only, not paths. */
        for (int i = 0; include[i]; i++)
            if (!strchr(include[i], '/'))
                g_ptr_array_add(argv, g_strconcat("--include=", include[i], NULL));
        for (int i = 0; exclude[i]; i++)
            if (!strchr(exclude[i], '/')) {
                g_ptr_array_add(argv, g_strconcat("--exclude=", exclude[i], NULL));
                g_ptr_array_add(argv, g_strconcat("--exclude-dir=", exclude[i], NULL));
            }
    }
    add(argv, "--");
    add(argv, q->text);
    add(argv, ".");
    g_ptr_array_add(argv, NULL);
    g_strfreev(include);
    g_strfreev(exclude);
    return (char **)g_ptr_array_free(argv, FALSE);
}

GRegex *search_regex(const SearchQuery *q, GError **error)
{
    char *pattern = q->regex ? g_strdup(q->text) : g_regex_escape_string(q->text, -1);
    GRegex *regex;

    if (q->whole_word) {
        char *word = g_strdup_printf("\\b(?:%s)\\b", pattern);
        g_free(pattern);
        pattern = word;
    }
    regex = g_regex_new(pattern, q->match_case ? 0 : G_REGEX_CASELESS, 0, error);
    g_free(pattern);
    return regex;
}

int search_parse_line(const char *line, gsize len, GRegex *regex, SearchHitFunc found, gpointer data)
{
    const char *nul = memchr(line, '\0', len), *rest, *colon, *end = line + len;
    GMatchInfo *info = NULL;
    SearchHit hit = { 0 };
    char *path, *text;
    int count = 0;

    if (!nul)
        return 0;
    rest = nul + 1;
    colon = memchr(rest, ':', end - rest);
    if (!colon || colon == rest)
        return 0;
    for (const char *p = rest; p < colon; p++)
        if (!g_ascii_isdigit(*p))
            return 0;
    hit.line = atoi(rest);
    if (hit.line <= 0)
        return 0;
    if (end > colon + 1 && end[-1] == '\r')
        end--;
    path = g_utf8_make_valid(line, nul - line);
    text = g_utf8_make_valid(colon + 1, end - colon - 1);
    hit.path = g_str_has_prefix(path, "./") ? path + 2 : path;
    hit.text = text;

    if (regex && g_regex_match(regex, text, 0, &info)) {
        while (g_match_info_matches(info)) {
            g_match_info_fetch_pos(info, 0, &hit.start, &hit.end);
            hit.column = (int)g_utf8_pointer_to_offset(text, text + hit.start) + 1;
            found(&hit, data);
            count++;
            g_match_info_next(info, NULL);
        }
    }
    g_match_info_free(info);
    if (count == 0) {
        hit.start = hit.end = 0;
        hit.column = 1;
        found(&hit, data);
        count = 1;
    }
    g_free(text);
    g_free(path);
    return count;
}

/* A copy with tabs as spaces, so a label shows them one column wide. */
static char *flat(const char *start, const char *end)
{
    return g_strdelimit(g_strndup(start, end - start), "\t", ' ');
}

char *search_snippet(const char *text, int start, int end, char **match, char **after)
{
    const char *from = text, *stop;
    gboolean cut = FALSE;
    char *before, *shown;

    while ((*from == ' ' || *from == '\t') && from < text + start)
        from++;
    if (g_utf8_pointer_to_offset(from, text + start) > SNIPPET_BEFORE) {
        from = g_utf8_offset_to_pointer(text + start, -SNIPPET_KEEP);
        cut = TRUE;
    }
    stop = text + end;
    for (int i = 0; *stop && i < SNIPPET_AFTER; i++)
        stop = g_utf8_next_char(stop);
    shown = flat(from, text + start);
    before = cut ? g_strconcat("…", shown, NULL) : g_strdup(shown);
    g_free(shown);
    *match = flat(text + start, text + end);
    *after = flat(text + end, stop);
    return before;
}
