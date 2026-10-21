/*
 * The Run and Debug panel, VS Code's, while a program is paused:
 *   Variables   the paused function's arguments and local variables with their values; arrays,
 *               structs and pointers open like folders; a value that changed since the last pause is
 *               colored
 *   Call Stack  how the program got there: the paused function, the one that called it, and so on
 *               up to main; a click shows that function's line and its variables
 */
#pragma once

#include "app.h"

#include <json-glib/json-glib.h>

/* The panel (window.c docks it, on the left by default). */
GtkWidget *debug_view(App *t);

/* The program paused: frames is the stackTrace answer's stackFrames, shown the index of the frame
 * the editor shows (its variables are listed). */
void debug_view_paused(App *t, JsonNode *frames, int shown);

/* It runs again: the variables are not known now. */
void debug_view_running(App *t);

/* Debugging ended. */
void debug_view_ended(App *t);

/* What the lists show, a line each (for the tests): the variables as "name = value", "*" after a
 * changed one; the call stack's rows as "function file:line". */
char *debug_view_variables_text(void);
char *debug_view_stack_text(void);
