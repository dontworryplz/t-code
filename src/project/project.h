/*
 * The project: the folder t-code works in (t->root). Open Folder, the recent folders menu and
 * "Open Folder as Project" in the file manager all switch it.
 */
#pragma once

#include "app.h"

typedef void (*ConfirmedFunc)(App *t, const char *arg);

/* Asks before throwing away unsaved changes; runs then(t, arg) on "yes", or right away when
 * nothing is unsaved. */
void unless_unsaved(App *t, const char *question, const char *button, ConfirmedFunc then, const char *arg);

/* Switches to another project folder (after asking about unsaved changes). */
void open_project(App *t, const char *root);

/* The Open Folder dialog (Ctrl+Shift+O). */
void choose_folder(App *t);

/* The window title: the folder's name, then "t-code". */
void update_title(App *t);

/* The window actions the title bar's buttons and menus use (win.open-folder, win.open-recent,
 * win.keybinds, win.about), and the recent folders menu. */
void add_project_actions(App *t);

/* Fills the recent folders menu from the session. */
void rebuild_recent_menu(App *t);
