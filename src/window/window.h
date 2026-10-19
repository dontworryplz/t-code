/*
 * The main window and its layout:
 *
 *   +-------------+-------------------------------+
 *   |             | [main.c x] [util.c x]         |  editor tabs (GtkSourceView);
 *   |    files    |                               |  split into two groups side by side
 *   | (t-code-fm) +-------------------------------+
 *   |             | shell                         |
 *   +-------------+-------------------------------+
 *   | branch  problems  Build  Run      Ln, Col  Keybinds |  status bar
 *
 * The sidebar on the left is a stack of three pages: the file manager ("files"), Source Control
 * ("scm") and Search ("search").
 */
#pragma once

#include "app.h"

/* Creates the window with everything in it, starts the terminals and shows it. Called once, from
 * the application's "activate" or "open" signal. */
void build_window(App *t, GtkApplication *app);

/* Text size of every terminal and preview (Ctrl+= / Ctrl+-), 0.5 to 3. */
void set_font_scale(App *t, double scale);

/* Shows or hides a pane (the sidebar or the shell); a pane that had the keyboard gives it to the
 * current tab. */
void set_pane_visible(App *t, GtkWidget *pane, gboolean visible);

/* Hides a shown pane, or shows a hidden one and gives it the keyboard. */
void toggle_pane(App *t, GtkWidget *pane);

/* Shows the dock holding page ("files", "scm", "search" or "shell"), switches to it and gives it
 * the keyboard. */
void show_sidebar_page(App *t, const char *page);

/* Which dock holds page (0..3 left, 4..7 bottom, 8..11 right; see DOCKS in app.h), or -1. */
int panel_dock(App *t, const char *page);

/* Moves a panel ("files", "scm", "search" or "shell") to dock (0..DOCKS-1). */
void move_panel(App *t, const char *page, int dock);
