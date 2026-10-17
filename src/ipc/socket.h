/*
 * The unix socket the file manager (t-code-fm) sends its requests to, one per line:
 *   open <path>, open-side <path>, preview <path>, project <dir>, compare <path>,
 *   history <path>, terminal <dir>, copy <text>
 * Its path is in $TCODE_SOCKET for every program t-code starts.
 */
#pragma once

#include "app.h"

/* Listens on $XDG_RUNTIME_DIR/t-code-<pid>.sock (t->socket_path). */
void start_socket(App *t);

/* Stops listening and removes the socket file. */
void stop_socket(App *t);
