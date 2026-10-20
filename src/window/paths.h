/*
 * Where t-code finds the files it needs besides itself: the editor's style scheme and the icon, all looked up next to the running executable, so the same binary works from
 * the build tree, from ~/.local and from a package.
 */
#pragma once

#include "app.h"

/* Fills t->paths.styles, t->paths.icons and t->paths.temp. */
void find_files(App *t);
