/*
 * The editor page's files (src/editor and src/lsp), included by the tests that need their static
 * functions, with stand-ins for the rest of t-code they call (tabs, the status bar, the picker).
 */
#pragma once

#include "editor/detect.c"
#include "window/palette.c"
#include "editor/blocks.c"
#include "editor/diffgutter.c"
#include "editor/folding.c"
#include "editor/breakpoints.c"
#include "editor/sticky.c"
#include "git/linediff.c"
#include "editor/editing.c"
#include "editor/editorview.c"
#include "editor/findbar.c"
#include "editor/indentconf.c"
#include "editor/guides.c"
#include "editor/occurrences.c"
#include "lsp/channel.c"
#include "lsp/json.c"
#include "lsp/client.c"
#include "lsp/compiledb.c"
#include "lsp/completion.c"
#include "lsp/diagnostics.c"
#include "lsp/edits.c"
#include "lsp/hover.c"
#include "lsp/navigate.c"
#include "lsp/semantic.c"
#include "lsp/signature.c"

/* clang-format off */
void update_status(App *t G_GNUC_UNUSED) {}
void debug_breakpoints_changed(App *t G_GNUC_UNUSED, const char *path G_GNUC_UNUSED) {}
void debug_hover_attach(Tab *tab G_GNUC_UNUSED) {}
gboolean disassembly_is(App *t G_GNUC_UNUSED, Tab *tab G_GNUC_UNUSED) { return FALSE; }
gboolean disassembly_breakpoint_at(App *t G_GNUC_UNUSED, int line G_GNUC_UNUSED) { return FALSE; }
void disassembly_toggle_breakpoint(App *t G_GNUC_UNUSED, int line G_GNUC_UNUSED) {}
void status_show_indent(App *t G_GNUC_UNUSED) {}
int tab_count(App *t G_GNUC_UNUSED) { return 0; }
Tab *tab_nth(App *t G_GNUC_UNUSED, int index G_GNUC_UNUSED) { return NULL; }
Tab *find_tab(App *t G_GNUC_UNUSED, TabKind kind G_GNUC_UNUSED, const char *path G_GNUC_UNUSED) { return NULL; }
void select_tab(Tab *tab G_GNUC_UNUSED) {}
void open_file_at(App *t G_GNUC_UNUSED, const char *path G_GNUC_UNUSED, int line G_GNUC_UNUSED,
                  int column G_GNUC_UNUSED) {}
void editor_goto(Tab *tab G_GNUC_UNUSED, int line G_GNUC_UNUSED, int column G_GNUC_UNUSED) {}
void save_editor(Tab *tab G_GNUC_UNUSED) {}
void refresh_branch(App *t G_GNUC_UNUSED) {}
void picker_open(App *t G_GNUC_UNUSED, const char *placeholder G_GNUC_UNUSED, PickerQuery query G_GNUC_UNUSED,
                 PickerChoose choose G_GNUC_UNUSED) {}
void picker_add(App *t G_GNUC_UNUSED, guint generation G_GNUC_UNUSED, const char *label G_GNUC_UNUSED,
                const char *detail G_GNUC_UNUSED, const char *payload G_GNUC_UNUSED) {}
void picker_message(App *t G_GNUC_UNUSED, guint generation G_GNUC_UNUSED, const char *text G_GNUC_UNUSED) {}
int fuzzy_score(const char *query G_GNUC_UNUSED, const char *text G_GNUC_UNUSED) { return 0; }
/* clang-format on */
