/*
 * Language servers (clangd) shared by all editors: each micro's lsp plugin connects through
 * t-code-lsp to a socket here instead of starting its own server, and one server per project
 * folder and command serves them all.
 */
#pragma once

#include "app.h"

/* Listens on $XDG_RUNTIME_DIR/t-code-<pid>-lsp.sock. */
void lsp_start(App *t);

/* Stops listening; the servers get EOF on their stdin and exit. */
void lsp_stop(void);

/* Adds TCODE_LSP and TCODE_LSP_CONNECT to an editor's environment (env holds "NAME=value"
 * strings), which make its lsp plugin use the shared servers. */
void lsp_editor_env(App *t, GPtrArray *env);
