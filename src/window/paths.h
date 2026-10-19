/*
 * Where t-code finds the files it needs besides itself: t-code-fm, the editor's style scheme and
 * the icon, all looked up next to the running executable, so the same binary works from
 * the build tree, from ~/.local and from a package.
 */
#pragma once

#include "app.h"

/* Fills t->fm_program, t->styles_dir, t->icon_dir and t->temp_dir. */
void find_files(App *t);
