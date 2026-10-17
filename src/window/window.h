/*
 * The main window and its layout:
 *
 *   +-------------+-------------------------------+
 *   |             | [main.c x] [util.c x]         |  editor tabs, one terminal (micro) each;
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

/* Shows the sidebar on one of its pages ("files", "scm" or "search") and gives that page the
 * keyboard. */
void show_sidebar_page(App *t, const char *page);
