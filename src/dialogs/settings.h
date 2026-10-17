/*
 * The Settings window (Ctrl+, or the gear in the title bar). Changes apply at once and are saved
 * in session.ini.
 */
#pragma once

#include "app.h"

/* Opens the window, or brings it to the front. */
void show_settings(App *t);

/* Adds micro's command line options that follow the settings (tab size, word wrap, ...) to argv. */
void settings_editor_args(App *t, GPtrArray *argv);
