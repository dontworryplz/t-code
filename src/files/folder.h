/*
 * A folder's entries as a list model, GIO-only (tested without a display, also under ASan): GFileInfos
 * with their name, type and GFile ("standard::file"). The folder is read in a thread; when entries
 * come, go or are renamed (not when a file is only written) it is read again after a short delay and
 * only the differences are applied, so rows that stay are the same objects. GtkDirectoryList does
 * most of this but misses files that appear by a rename, which is how editors, git and t-code itself
 * save.
 */
#pragma once

#include <gio/gio.h>

#define FOLDER_DELAY_MS 250 /* changes come in bursts (a build, a checkout): the folder is read once */

/* An empty list for the folder at path; watched for changes once loaded, when watch is set. Freeing
 * the list stops the reading and the watching. */
GListModel *folder_new(const char *path, gboolean watch);

/* Reads the folder the first time it is called for it (a folder shown closed is never read). */
void folder_load(GListModel *list);

/* Whether the list has started reading its folder, and whether it watches it. */
gboolean folder_loaded(GListModel *list);
gboolean folder_watched(GListModel *list);

/* The folder's path. */
const char *folder_path(GListModel *list);
