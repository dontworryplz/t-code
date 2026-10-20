/*
 * The files in ~/.config/t-code made to be edited by hand: settings.ini (the settings), theme.css
 * (the user's CSS over t-code's look) and styles/dark-modern.xml (the user's copy of the editor's
 * colors). Each opens in an editor tab, made first when it is not there, and a change saved to it
 * (from t-code or any other program) applies at once.
 */
#pragma once

#include "app.h"

/* Starts watching the three files (from build_window). */
void watch_user_files(App *t);

/* Opens settings.ini in an editor tab, written first with every setting and its comment. */
void open_settings_file(App *t);

/* Opens theme.css in an editor tab; the first time it is made with examples in a comment. */
void open_user_theme(App *t);

/* Opens the user's copy of the editor's colors in an editor tab; the first time it is copied from
 * t-code's. */
void open_editor_colors(App *t);
