/*
 * t-code's look: VS Code Dark Modern colors on rounded panels ("cards") over GTK's built-in
 * Adwaita theme, or square ones (the settings' theme), and the user's own CSS over it.
 */
#pragma once

#include "app.h"

/* Before any window: forces Adwaita (dark) and adds the global CSS, the square theme when the
 * settings ask for it, and the user's theme.css. */
void load_theme(App *t);

/* Turns the square theme on or off, as t->settings.square says. */
void apply_theme(App *t);

/* ~/.config/t-code/theme.css (free it): the user's own CSS, over t-code's. */
char *user_theme_file(void);

/* Reads theme.css again (none: nothing over t-code's own look). */
void load_user_theme(void);

/* Gives widget rounded corners on the window's darker frame; its children are cut to them. */
void make_card(GtkWidget *widget);
