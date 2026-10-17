/*
 * A small C/C++ lexer for the minimap (see lexer.h). It follows micro/syntax/c.yaml, so the
 * minimap shows the same colors as the editor.
 */
#include "editor/lexer.h"

#include <string.h>

static const char *const statements[] = {
    "if", "else", "for", "while", "do", "switch", "case", "default", "break", "continue", "return",
    "goto", "try", "catch", "throw", "co_return", "co_await", "co_yield", NULL,
};
static const char *const keywords[] = {
    "int", "char", "void", "short", "long", "float", "double", "signed", "unsigned", "bool", "auto",
    "const", "static", "extern", "inline", "volatile", "register", "struct", "union", "enum",
    "typedef", "sizeof", "class", "namespace", "template", "typename", "using", "public", "private",
    "protected", "virtual", "override", "constexpr", "consteval", "constinit", "noexcept", "new",
    "delete", "this", "true", "false", "nullptr", "NULL", "operator", "friend", "explicit", "mutable",
    "static_cast", "dynamic_cast", "reinterpret_cast", "const_cast", "decltype", "concept", "requires",
    "TRUE", "FALSE", NULL,
};
/* Lowercase type names micro/syntax/c.yaml colors as types (besides foo_t). */
static const char *const types[] = {
    "FILE", "DIR", "va_list", "jmp_buf", "fd_set", "gboolean", "gchar", "guchar", "gint", "guint", "gshort",
    "gushort", "glong", "gulong", "gint8", "guint8", "gint16", "guint16", "gint32", "guint32", "gint64",
    "guint64", "gfloat", "gdouble", "gsize", "gssize", "goffset", "gintptr", "guintptr", "gpointer",
    "gconstpointer", "gunichar", "gquark", NULL,
};

/* Whether the len characters at word are one of list's words. */
static gboolean in_list(const char *const *list, const char *word, int len)
{
    for (int i = 0; list[i]; i++)
        if ((int)strlen(list[i]) == len && strncmp(list[i], word, len) == 0)
            return TRUE;
    return FALSE;
}

/* A character of a name or number. */
static gboolean is_word_char(char c)
{
    return g_ascii_isalnum(c) || c == '_';
}

/* Skips a block comment from i to just after its end, or to the end of the line (*open = TRUE). */
static int comment_end(const char *s, int len, int i, gboolean *open)
{
    while (i + 1 < len && !(s[i] == '*' && s[i + 1] == '/'))
        i++;
    *open = i + 1 >= len;
    return *open ? len : i + 2;
}

/* A word's color, following micro/syntax/c.yaml: ALL_CAPS names are constants (macros when
 * called), PascalCase names and foo_t are types, and a name before "(" is a function. */
static Ink word_ink(const char *s, int len, int start, int end)
{
    const char *word = s + start;
    int n = end - start, next = end;
    gboolean upper = FALSE, lower = FALSE;

    if (in_list(statements, word, n))
        return INK_STATEMENT;
    if (in_list(keywords, word, n))
        return INK_KEYWORD;
    if (in_list(types, word, n) || (n > 2 && word[n - 2] == '_' && word[n - 1] == 't'))
        return INK_TYPE;
    while (next < len && (s[next] == ' ' || s[next] == '\t'))
        next++;
    for (int i = 0; i < n; i++) {
        upper |= g_ascii_isupper(word[i]);
        lower |= g_ascii_islower(word[i]);
    }
    if (upper && !lower && n > 1)
        return next < len && s[next] == '(' ? INK_KEYWORD : INK_CONSTANT;
    if (next < len && s[next] == '(')
        return INK_FUNCTION;
    return g_ascii_isupper(word[0]) && lower ? INK_TYPE : INK_TEXT;
}

gboolean lex_line(const char *s, int len, gboolean comment, cairo_t *cr, PaintFunc paint)
{
    int i = 0, column = 0, k = 0;
    gboolean include = FALSE, name_next = FALSE; /* after #include; the name after #define */

    while (k < len && (s[k] == ' ' || s[k] == '\t'))
        k++;
    while (i < len) {
        int start = i;
        Ink ink = INK_TEXT;
        char c = s[i];

        if (comment) {
            i = comment_end(s, len, i, &comment);
            ink = INK_COMMENT;
        } else if (c == '/' && i + 1 < len && s[i + 1] == '/') {
            i = len;
            ink = INK_COMMENT;
        } else if (c == '/' && i + 1 < len && s[i + 1] == '*') {
            i = comment_end(s, len, i + 2, &comment);
            ink = INK_COMMENT;
        } else if (c == '"' || c == '\'') {
            for (i++; i < len && s[i] != c; i++)
                if (s[i] == '\\')
                    i++;
            i = MIN(i + 1, len);
            ink = INK_STRING;
        } else if (g_ascii_isdigit(c)) {
            while (i < len && (is_word_char(s[i]) || s[i] == '.'))
                i++;
            ink = INK_NUMBER;
        } else if (c == '#' && i == k && !comment) {
            /* A directive: "#include <a.h>" (the header like a string), "#define NAME" (the name
             * blue), "#if COND" (the condition blue). */
            int word;
            for (i++; i < len && (s[i] == ' ' || s[i] == '\t'); i++)
                ;
            for (word = i; i < len && is_word_char(s[i]); i++)
                ;
            include = i - word == 7 && strncmp(s + word, "include", 7) == 0;
            name_next = in_list((const char *const[]){ "define", "undef", "ifdef", "ifndef", NULL }, s + word, i - word);
            if (cr)
                paint(cr, column, i - start, INK_STATEMENT);
            column += i - start;
            if (in_list((const char *const[]){ "if", "elif", NULL }, s + word, i - word)) {
                while (i < len && (s[i] == ' ' || s[i] == '\t'))
                    i++, column++;
                start = i;
                while (i < len && !(s[i] == '/' && i + 1 < len && (s[i + 1] == '/' || s[i + 1] == '*')))
                    i++;
                if (cr && i > start)
                    paint(cr, column, i - start, INK_KEYWORD);
                column += i - start;
            }
            continue;
        } else if (c == '<' && include) {
            while (i < len && s[i] != '>')
                i++;
            i = MIN(i + 1, len);
            ink = INK_STRING;
        } else if (is_word_char(c)) {
            while (i < len && is_word_char(s[i]))
                i++;
            ink = name_next ? INK_KEYWORD : word_ink(s, len, start, i);
            name_next = FALSE;
        } else if (c == ' ') {
            i++;
            column++;
            continue;
        } else if (c == '\t') {
            i++;
            column += 4 - column % 4;
            continue;
        } else {
            i++;
            ink = INK_PUNCTUATION;
        }
        /* A run with spaces inside (a comment, a string) is still one bar, like the text. */
        if (cr)
            paint(cr, column, i - start, ink);
        column += i - start;
    }
    return comment;
}
