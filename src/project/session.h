/*
 * What t-code remembers between runs, in ~/.config/t-code/session.ini:
 *   [settings]          the Settings window's choices
 *   [window]            size, pane sizes, which panes are shown, text size
 *   [recent]            the last opened project folders
 *   [project <sha1>]    per project folder: the open tabs of both groups and the active ones
 */
#pragma once

#include "app.h"

/* Reads [settings] into t->settings (and t->plain); missing values get their defaults. */
void session_load_settings(App *t);

/* Writes t->settings to [settings]. */
void session_save_settings(App *t);

/* Gives the window its saved size, pane sizes, visible panes and text size. */
void session_apply_window(App *t);

/* Opens the tabs saved for the current project; returns whether any opened. */
gboolean session_restore_tabs(App *t);

/* Saves the window and the current project's tabs. */
void session_save(App *t);

/* Puts the current project at the top of the recent folders. */
void session_add_recent(App *t);

/* The recent folders, most recent first (free with g_strfreev); NULL when there are none. */
char **session_recent(void);
