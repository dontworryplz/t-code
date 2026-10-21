/*
 * The Search panel's text work, apart from GTK so tests can run it without a display: the
 * ripgrep / grep command line for a query, and finding the matches in the lines they print.
 */
#pragma once

#include <glib.h>

typedef struct {
  const char *text;
  gboolean match_case, whole_word, regex;
  const char *include, *exclude; /* comma separated globs */
} SearchQuery;

typedef struct {
  const char *path; /* as printed, relative to the folder searched ("./" removed) */
  int line;         /* 1-based */
  int column;       /* 1-based, in characters */
  const char *text; /* the whole line */
  int start, end;   /* byte offsets of the match in text; equal when it was not found there */
} SearchHit;

typedef void (*SearchHitFunc)(const SearchHit *hit, gpointer data);

/* The command to run in the folder (NULL-terminated, g_strfreev). Each line it prints is
 * "path\0line:text". ripgrep skips what .gitignore ignores; grep only skips .git and build. */
char **search_argv(const SearchQuery *q, gboolean ripgrep);

/* Finds the query in a line, to mark the matches. NULL with error for a bad regular expression. */
GRegex *search_regex(const SearchQuery *q, GError **error);

/* Calls found for each match in one printed line (without its '\n'); a line where the regex finds
 * nothing (a shortened long line) still gives one hit at its start. Returns the hits. */
int search_parse_line(const char *line, gsize len, GRegex *regex, SearchHitFunc found, gpointer data);

/* The line as the panel shows it: leading blanks dropped and, when the match is far to the right,
 * the start cut to "…" and some text before it. Returns the part before the match; *match and
 * *after get the rest (all g_free). */
char *search_snippet(const char *text, int start, int end, char **match, char **after);
