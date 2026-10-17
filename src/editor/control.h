/*
 * The control pipe: a named pipe ($XDG_RUNTIME_DIR/t-code-<pid>-editor-<n>, given to micro as
 * TCODE_CONTROL) that micro's tcode plugin reads line by line:
 *   scroll <line>       show the file from that line (0-based) on
 *   command <command>   run a micro command
 *   prompt <text>       open the command bar with text typed
 *   visible 0|1         the tab was hidden or shown
 */
#pragma once

#include "app.h"

/* Creates the pipe for editor number n. Returns its file descriptor (and its path in *path), or
 * -1 (and NULL) when it cannot be made. */
int control_open(int number, char **path);

/* Closes and removes a pipe from control_open(); the plugin's reader then ends. */
void control_close(int fd, char *path);

/* Sends one line to the editor's plugin; FALSE when there is no pipe (or it is full). */
gboolean editor_control(Tab *tab, const char *line);
