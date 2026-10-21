/*
 * Open Tabs, the top of the Files panel: the open tabs of every group, the
 * current one highlighted; a click shows one, a middle click or its × closes it. It folds from its
 * heading, and is open by default.
 */
#pragma once

#include "app.h"

/* The section (files.c puts it over the folder tree). */
GtkWidget *open_tabs_new(App *t);

/* Tabs were opened, closed, moved, saved or switched (editor/tabs.c): the list follows shortly. */
void open_tabs_changed(App *t);

/* Whether the list is open (not folded), and opening or folding it (the session keeps it). */
gboolean open_tabs_shown(void);
void open_tabs_show(gboolean shown);
