/*
 * Native Stage 1 Plugin System:
 * Out-of-process plugins discovered in user and system directories, configured via plugin.json.
 * Commands appear in the Command Palette and execute asynchronously with clean environment
 * ($TCODE_SOCKET, $TCODE_ACTIVE_FILE, etc.) without freezing the UI.
 */
#pragma once

#include "app.h"

typedef struct Plugin Plugin;

typedef struct PluginCommand {
  char *id;
  char *name;
  char *keys;    /* display text */
  char *trigger; /* GTK shortcut trigger, e.g. <Control><Alt>1 */
  Plugin *plugin;
} PluginCommand;

typedef struct Plugin {
  char *id;
  char *name;
  char *version;
  char *description;
  char *dir;
  char *executable;
  GPtrArray *commands; /* PluginCommand * */
} Plugin;

/* Discovers plugins from user and system directories and populates t->plugins. */
void plugins_init(App *t);

/* Reloads all plugins. */
void plugins_reload(App *t);

/* Frees all loaded plugins and commands. */
void plugins_stop(App *t);

/* Scans a directory for plugins and adds them to t->plugins. */
void plugins_scan_dir(App *t, const char *dir_path);

/* Queries plugins for matching commands in the command palette. Returns match count. */
int plugins_palette_query(App *t, const char *text, guint generation);

/* Handles selection of a plugin payload (format "plugin:<plugin_id>:<command_id>"). */
gboolean plugins_palette_choose(App *t, const char *payload);

/* Adds manifest shortcuts to a window's shared shortcut controller. */
void plugins_add_shortcuts(GtkShortcutController *controller, App *t);

/* Runs a plugin command asynchronously in a subprocess. */
void plugin_run_command(App *t, const char *plugin_id, const char *command_id);

/* Opens the user plugins directory in desktop file manager. */
void open_plugins_folder(App *t);
