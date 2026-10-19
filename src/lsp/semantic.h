/*
 * Colors by meaning, like VS Code with clangd: the language server's semantic tokens color
 * functions yellow, types teal, macros blue, enum members and constants bright blue, variables and
 * parameters light blue, over GtkSourceView's own coloring (which only knows the words' shapes).
 */
#pragma once

#include "app.h"

typedef struct Semantic Semantic;

/* Asks for the editor's colors again a moment from now (the text changed, the server opened the
 * file); with code coloring off (F7) they are taken off instead. */
void semantic_update(Tab *tab);

/* Frees an editor's semantic state. NULL is fine. */
void semantic_free(Semantic *semantic);
