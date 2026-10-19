/*
 * What shows when the pointer rests on code, like VS Code's hover: the problems there (in their
 * colors) and what the language server says about the symbol (its type, signature, comment).
 */
#pragma once

#include "app.h"

/* Adds the hover to a new editor. */
void hover_attach(Tab *tab);
