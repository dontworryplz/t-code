/*
 * Build (Ctrl+Shift+B) and Run (F5) in the shell pane, for the project found from the current
 * file upwards: CMake, a Makefile, or else the C/C++ file alone.
 */
#pragma once

#include "app.h"

/* Saves every changed file, then builds (and with run, runs) the project in the shell. */
void build_project(App *t, gboolean run);

/* What Build would build, for the status bar: "CMake: name", "Make: name", "gcc: file.c". */
char *project_summary(App *t);
