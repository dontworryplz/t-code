/*
 * The Settings window (Ctrl+, or the gear in the title bar). Changes apply at once and are saved
 * in settings.ini.
 */
#pragma once

#include "app.h"

/* Opens the window, or brings it to the front. */
void show_settings(App *t);

/* The settings changed outside the window (settings.ini edited by hand): applies them to the open
 * editors, the fonts and the theme, restarts clangd when restart_lsp, and closes the window, whose
 * controls show the old values. */
void settings_changed(App *t, gboolean restart_lsp);
