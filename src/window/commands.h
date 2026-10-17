/*
 * Every command of the window in one table (commands.c): the keyboard shortcuts, the command
 * palette (Ctrl+Shift+P) and the Keybinds list are all made from it. A new command is one line
 * in that table.
 */
#pragma once

#include "app.h"

/* Adds the table's shortcuts to the window. */
void add_shortcuts(App *t);

/* Opens the command palette. */
void command_palette(App *t);

/* A command and its shortcut, for the Keybinds window. */
typedef struct {
    const char *name;
    const char *keys; /* as shown; NULL when there is no shortcut */
} KeyHelp;

/* The window's commands; *n is set to how many. */
const KeyHelp *window_keys(int *n);
