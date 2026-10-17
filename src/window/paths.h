/*
 * Where t-code finds the files it needs besides itself: t-code-fm, t-code-lsp, micro's config
 * folder and the icon, all looked up next to the running executable, so the same binary works from
 * the build tree, from ~/.local and from a package.
 */
#pragma once

#include "app.h"

/* Fills t->fm_program, t->lsp_program, t->micro_config, t->icon_dir and t->temp_dir. */
void find_files(App *t);
