/*
 * Terminals (VteTerminal): their look, starting programs in them, and the two that are always
 * there, the file manager on the left and the shell at the bottom.
 */
#pragma once

#include "app.h"

#define DEFAULT_FONT "Cascadia Code 11"

/* A new terminal in the editor font with the Dark Modern colors. */
VteTerminal *new_terminal(App *t);

/* Sets a terminal's font (a Pango description like "Cascadia Code 11"; NULL: DEFAULT_FONT) at the
 * window's text size. */
void set_terminal_font(App *t, VteTerminal *term, const char *font);

/* Gives every terminal the font the settings choose for it. */
void apply_fonts(App *t);

/* Runs argv in term, in the project root, with $TCODE_SOCKET and extra_env ("NAME=value", may be
 * NULL) set; the child's pid goes to *pid. */
void spawn(App *t, VteTerminal *term, char **argv, char **extra_env, GPid *pid);

/* Creates t->fm and t->shell (they restart when their program exits) ... */
void create_fixed_terminals(App *t);
/* ... starts t-code-fm and the user's shell in them ... */
void start_fixed_terminals(App *t);
/* ... and makes both start again in the current project (after a project switch). */
void restart_fixed_terminals(App *t);

/* Types command into the shell pane and runs it. A program that runs in the shell gets Ctrl+C
 * first; when it does not give the prompt back within 2 seconds, a "terminal is busy" message
 * shows instead. */
void run_in_shell(App *t, const char *command);
