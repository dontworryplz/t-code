/*
 * Terminals (VteTerminal): their look, starting programs in them, and the shell pane, which is
 * always there.
 */
#pragma once

#include "app.h"

#define DEFAULT_FONT "Cascadia Code 11"

/* A new terminal in the editor font in the color theme's colors. */
VteTerminal *new_terminal(App *t);

/* Sets a terminal's font (a Pango description like "Cascadia Code 11"; NULL: DEFAULT_FONT) at the
 * window's text size. */
void set_terminal_font(App *t, VteTerminal *term, const char *font);

/* Gives the terminals, the editors and the Files panel the fonts the settings choose for them. */
void apply_fonts(App *t);

/* Runs argv in term, in the project root, with $TCODE_SOCKET and extra_env ("NAME=value", may be
 * NULL) set; the child's pid goes to *pid. */
void spawn(App *t, VteTerminal *term, char **argv, char **extra_env, GPid *pid);

/* Creates t->shell.term (it restarts when the shell exits) ... */
void create_shell_pane(App *t);
/* ... starts the user's shell in it ... */
void start_shell_pane(App *t);
/* ... and makes it start again in the current project (after a project switch). */
void restart_shell_pane(App *t);

/* Types command into the shell pane and runs it. A program that runs in the shell gets Ctrl+C
 * first; when it does not give the prompt back within 2 seconds, a "terminal is busy" message
 * shows instead. */
void run_in_shell(App *t, const char *command);

/* cd into dir in the shell pane (kept out of the shell's history). */
void cd_in_shell(App *t, const char *dir);

/* The color theme changed: the shell and the Debug panel's terminal take its colors. */
void terminal_apply_colors(App *t);
