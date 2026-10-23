/*
 * The control socket scripts and tools send requests, one per line (or ended by a NUL byte):
 *   open <path>, open-side <path>, open-window <path>, preview <path>, project <dir>, compare <path>,
 *   history <path>, terminal <dir>, copy <text>, say <text>, insert <text>, append <text>,
 *   replace-all <text>, get-file, get-project, get-text, save, subscribe
 * "subscribe" keeps the connection open for did_open/did_save JSON-lines events.
 * Its path is in $TCODE_SOCKET for every program t-code starts.
 */
#pragma once

#include "app.h"

/* Listens on $XDG_RUNTIME_DIR/t-code-<pid>.sock (t->paths.socket). */
void start_socket(App *t);
/* Sends JSON-lines did_open and did_save events to connected "subscribe" clients. */
void socket_did_open(App *t, const char *path);
void socket_did_save(App *t, const char *path);

/* Stops listening and removes the socket file. */
void stop_socket(App *t);
