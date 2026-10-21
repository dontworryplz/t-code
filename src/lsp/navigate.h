/*
 * Moving around the code with the language server: Go to Definition (F12,
 * Ctrl+click) and back (Alt+Left), Find All References (Shift+F12, in the popup list), Format
 * Document (Shift+Alt+F) and Rename Symbol (F2, in every file it is used in).
 */
#pragma once

#include "app.h"

/* Adds Ctrl+click (Go to Definition) to a new editor. */
void navigate_attach(Tab *tab);

/* Jumps to the definition of the symbol at the cursor (in another tab when it is elsewhere). */
void go_to_definition(Tab *tab);

/* Back to where the last Go to Definition (or reference) was taken from. */
void go_back(App *t);

/* Lists the places the symbol at the cursor is used, to pick one. */
void find_references(Tab *tab);

/* Formats the file with the language server (clangd: the project's .clang-format). */
void format_document(Tab *tab);

/* Ctrl+S: saves the file, formatting it first when the "Format on save" setting is on. */
void save_with_format(Tab *tab);

/* Asks for a new name for the symbol at the cursor and renames it everywhere. */
void rename_symbol(Tab *tab);
