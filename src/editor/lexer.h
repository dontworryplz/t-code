/*
 * A small C/C++ lexer for the minimap: it splits a line into runs of one color, following the
 * rules of micro/syntax/c.yaml (functions, ALL_CAPS constants and macros, PascalCase and _t types,
 * punctuation, preprocessor directives), so the minimap looks like the code in the editor.
 */
#pragma once

#include <cairo.h>
#include <glib.h>

/* The colors, as micro's color scheme names them (minimap.c has their values). */
typedef enum {
    INK_TEXT, INK_COMMENT, INK_STRING, INK_NUMBER, INK_KEYWORD, INK_TYPE, INK_STATEMENT, INK_FUNCTION,
    INK_CONSTANT, INK_PUNCTUATION, INK_COUNT
} Ink;

/* Paints a run of length columns from column on. */
typedef void (*PaintFunc)(cairo_t *cr, int column, int length, Ink ink);

/* Colors one line (len bytes at s, comment: it starts inside a block comment), calling paint for
 * each run of visible characters; cr NULL only follows the comments. Returns whether the line ends
 * inside a block comment. */
gboolean lex_line(const char *s, int len, gboolean comment, cairo_t *cr, PaintFunc paint);
