# Moving the editor from micro to GtkSourceView 5

Done on `main`. The micro-based t-code (0.1.1) is kept in the `micro-main` branch.

## Why

- **Language server:** micro's Lua plugin API has no popups and no colored underlines, so clangd's
  errors were underlined in the text color and completion cycled through the info bar.
  GtkSourceView has a wavy underline, completion, hover and snippets built in.
- **Memory:** every micro was its own process (~20 MB) with a `t-code-lsp` relay and a `cat` next
  to it. A GtkSourceView buffer lives in t-code's process.
- **Simpler:** no terminal title reports, no control FIFO, no LSP relay socket, no Lua, no micro
  version differences. One client talks to clangd, the way the protocol is meant to be used.
- **A plain text editor**, without modes, like VS Code.

## What went, and what took its place

| Before (micro) | Now |
|---|---|
| `micro/` (config, `tcode.lua`, the pinned `lsp` plugin, `micro-clip`, syntax files, colorscheme) | `styles/dark-modern.xml` (a GtkSourceView style scheme) and GtkSourceView's own C / C++ definitions, with clangd's semantic tokens on top (`src/lsp/semantic.c`) |
| `src/lsp/main.c` (`t-code-lsp`), the multiplexer in `src/ipc/lsp.c`, `lspjson.c` | `src/lsp/client.c`: one client, one clangd per window, json-glib |
| `src/editor/control.c` (control FIFO), the terminal title protocol | direct calls, buffer signals |
| `src/editor/minimap.c`, `lexer.c` | `GtkSourceMap`, and a problems strip next to it |
| `src/editor/guides.c` reading the terminal's text | `src/editor/guides.c` drawing under the text of a GtkSourceView subclass |
| micro's diff gutter, fed by `tcode.lua` | `src/editor/diffgutter.c` with `src/git/linediff.c` |
| micro's editing keys | GtkSourceView's, plus `src/editor/editing.c` and `findbar.c` |
| `writable_micro_config()`, lazy micro start, t-code's autosave via `command save` | not needed; clangd gets a file once its tab is shown |

## How it went

1. **Foundation:** editor tabs as GtkSourceView (`editor.c`: loading with GtkSourceFileLoader,
   saving with `g_file_replace_contents`, autosave, reload on change, asking before closing;
   `editorview.c`: the page, the style scheme, the font, the settings).
2. **Editing:** find / replace, go to line, comment, duplicate / move / delete lines, closing
   brackets and quotes, `Enter` between braces.
3. **Language server:** the client with incremental sync in the server's position encoding,
   diagnostics as wavy underlines, the problems strip, hover.
4. **Popups:** completion (snippets, the `#include` a symbol needs), parameter hints, Go to
   Definition / back, references, rename, format, semantic colors; word completion for other files,
   format on save and clang-tidy as settings.
5. **The page around it:** indent guides, the diff gutter and the comparisons' gutters.
6. **Switch over:** micro, `t-code-lsp` and their tools removed; packages, install scripts, tools
   and docs updated; the code formatted with 2 spaces (`.clang-format`).

Each step was built warning-free and tested (`make test`); the language server parts against a
fake server (`tests/test_lsp.c`) and checked against the real clangd.

## Given up

micro's multiple cursors (GtkSourceView 5 has none), its macros and its command bar.
