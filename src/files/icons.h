/*
 * The Files panel's file icons, GLib-only: a Nerd Font glyph (Cascadia Code has none; fontconfig
 * falls back to an installed Nerd Font) and a color, by file name or extension.
 */
#pragma once

#include <glib.h>

typedef struct {
  const char *glyph, *color;
} FileIcon;

/* The icon for a file called name, or for a folder (open or closed). */
const FileIcon *file_icon(const char *name, gboolean dir, gboolean open);
