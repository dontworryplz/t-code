/*
 * A tab's right-click menu, in the tab strip and in Open Tabs: closing it and the others, keeping
 * it, moving it to the other group or window, its path, and showing it in the Files panel.
 */
#pragma once

#include "app.h"

/* Shows tab's menu at x, y of widget. */
void tab_menu(Tab *tab, GtkWidget *widget, double x, double y);
