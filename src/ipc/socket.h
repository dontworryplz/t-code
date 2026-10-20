/*
 * The control socket scripts and tools send requests to, one per line (or ended by a NUL byte):
 *   open <path>, open-side <path>, open-window <path>, preview <path>, project <dir>, compare <path>,
 *   history <path>, terminal <dir>, copy <text>
 * Its path is in $TCODE_SOCKET for every program t-code starts.
 */
#pragma once

#include "app.h"

/* Listens on $XDG_RUNTIME_DIR/t-code-<pid>.sock (t->paths.socket). */
void start_socket(App *t);

/* Stops listening and removes the socket file. */
void stop_socket(App *t);
