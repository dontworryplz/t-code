/*
 * Build (Ctrl+Shift+B), Run (Ctrl+F5) and the build before debugging (F5) in the shell pane, for the project found from the current
 * file upwards: CMake, a Makefile, or else the C/C++ file alone; a script (Python, JavaScript, shell, ...) runs with its
 * interpreter.
 */
#pragma once

#include "app.h"

/* Saves every changed file, then builds (and with run, runs) the project in the shell. */
void build_project(App *t, gboolean run);

/* The shell command build_project() types (free it). */
char *project_command(App *t, gboolean run);

/* Whether the current file is a script: there is nothing for the debugger, Ctrl+F5 runs it. */
gboolean project_is_script(App *t);

/* Saves every changed file, then builds the project in the shell for the debugger: once it built,
 * the program goes to debug_launch() (debug/debugger.h). Returns the folder the program is to run
 * in (free it). */
char *build_for_debug(App *t);

/* What Build would build, for the status bar: "CMake: name", "Make: name", "gcc: file.c", "python3: file.py". */
char *project_summary(App *t);
