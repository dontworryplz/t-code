/*
 * Where clangd gets a file's compile flags (GLib only, so tests/ can run it without a display):
 * like clangd, from the file's folder up to "/", the first folder with compile_commands.json,
 * build/compile_commands.json or compile_flags.txt. Without one clangd guesses, and the project's
 * include paths (-I) are unknown: "'glib.h' file not found".
 */
#pragma once

#include <glib.h>

/* The compilation database clangd uses for file, or a .clangd config (which may give the flags
 * itself) found first on the way up; NULL when there is neither. */
char *compile_db_find(const char *file);

/* A file between file's folder and root (both included) named almost like a compilation database
 * ("compile-flags.txt", "Compile_Commands.json"), which clangd does not read; NULL when there is
 * none. */
char *compile_db_misnamed(const char *file, const char *root);

/* Whether the C library's headers (for cxx also the C++ library's, /usr/include/c++) are installed
 * under sysroot ("/" but in tests). Without them no compile flags make clangd find <stdio.h>: the
 * compiler's and the C library's development packages are missing. */
gboolean compile_db_system_headers(const char *sysroot, gboolean cxx);
