/*
 * Build (Ctrl+Shift+B), Run (Ctrl+F5) and the build before debugging (F5) in the shell pane, for the project found from the current
 * file upwards: CMake, a Makefile, or else the C/C++ file alone; other languages' files run in their own project
 * (Cargo, Go, Node, Maven, Gradle, a Python root) or alone with their interpreter.
 */
#pragma once

#include "app.h"

/* Saves every changed file, then builds (and with run, runs) the project in the shell. */
void build_project(App *t, gboolean run);

/* The shell command build_project() types (free it). */
char *project_command(App *t, gboolean run);

/* Whether F5 can debug what Build builds: C / C++ (CMake, Make, a file alone; with nothing to build
 * it says so), not another language's file or project, which Ctrl+F5 runs. */
gboolean project_can_debug(App *t);

/* Saves every changed file, then builds the project in the shell for the debugger: once it built,
 * the program goes to debug_launch() (debug/debugger.h). Returns the folder the program is to run
 * in (free it). */
char *build_for_debug(App *t);

/* What Build would build, for the status bar: "CMake: name", "Make: name", "gcc: file.c", "Cargo: name", "python3: file.py". */
char *project_summary(App *t);
