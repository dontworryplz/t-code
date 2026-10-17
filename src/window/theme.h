/*
 * t-code's look: VS Code Dark Modern colors on rounded panels ("cards") over GTK's built-in
 * Adwaita theme.
 */
#pragma once

#include "app.h"

/* Before any window: forces Adwaita (dark) and adds the global CSS. */
void load_theme(void);

/* Gives widget rounded corners on the window's darker frame; its children are cut to them. */
void make_card(GtkWidget *widget);
