# Moving the editor from micro to GtkSourceView 5

The micro-based t-code (0.1.1) is kept in the `micro-main` branch. On `main`, editor tabs become
GtkSourceView 5 views inside the t-code process.

## Why

- **Language server:** micro's Lua plugin API has no popups and no colored underlines, so clangd's
  errors are underlined in the text color and completion cycles through the info bar.
  GtkSourceView has a red wavy underline (`PANGO_UNDERLINE_ERROR`), completion, hover and
  signature popups built in.
- **Memory:** every micro is its own process (~20–30 MB) with a `t-code-lsp` relay and a `cat`
  next to it. A GtkSourceView buffer lives in t-code's process and costs a few MB.
- **Simpler:** no OSC title reports, no control FIFO, no LSP relay socket, no Lua, no micro
  version differences. One client talks to clangd, the way the protocol is meant to be used.
- **Plain text editor feel:** like gedit or VS Code, without modes.

## What goes

| Now | Replaced by |
|---|---|
| `micro/` (config, `tcode.lua`, the pinned `lsp` plugin, `micro-clip`, syntax files, colorscheme) | a GtkSourceView style scheme (Dark Modern) and its own C / C++ language definitions |
| `src/lsp/main.c` (`t-code-lsp`) | nothing: the client is in process |
| `src/editor/control.c` (control FIFO) | direct calls |
| editor-side multiplexing in `src/ipc/lsp.c` (id renumbering, cached `initialize`, per-editor `didClose`) | one LSP client per server |
| `src/editor/minimap.c`, `lexer.c` | `GtkSourceMap`, with clangd's problem lines drawn on it |
| the OSC title parsing in `src/editor/editor.c` | buffer signals (`changed`, `modified-changed`, `mark-set`) |
| `writable_micro_config()`, lazy micro start, t-code's autosave via `command save` | not needed |

## What stays

The window, the file manager (`t-code-fm`), the shell, Search, Source Control, Quick Open, the
command palette, Markdown previews, sessions, Build / Run, and the indent guides (now read from
the buffer instead of the screen).

## Steps

Each step builds, passes `make test` and can be used on its own.

1. **Foundation:** the `gtksourceview-5` dependency (Makefile, packaging, README), a
   `TAB_SOURCE` editor tab next to the micro tab: open, edit, save, modified dot, cursor position
   in the status bar, the Dark Modern style scheme, Cascadia Code.
2. **Editing:** find / replace bar, go to line, toggle comment, duplicate / move lines, auto
   closing brackets, tab size / spaces / detect indentation / word wrap from Settings, autosave,
   reload when the file changes on disk.
3. **Language server:** an in-process LSP client over `lspchannel.c` (initialize, `didOpen`,
   incremental `didChange`, `didClose`), diagnostics as red / yellow wavy underlines with the
   message on hover and in the status bar counts.
4. **Popups:** completion while typing (`GtkSourceCompletionProvider`), signature help, hover,
   Go to Definition, References, format.
5. **The page around it:** `GtkSourceMap` minimap with problem lines, indent guides, git diff
   gutter (`git diff -U0` hunks in a `GtkSourceGutterRenderer`), side-by-side comparisons.
6. **Switch over:** GtkSourceView tabs become the default, the micro tab, `micro/`,
   `t-code-lsp` and `control.c` are removed, the docs (README, CLAUDE.md) and packages updated,
   memory measured against 0.1.1 with `tools/`.

## Dependencies

Build: `gtksourceview-5` headers (Fedora `gtksourceview5-devel`, Debian `libgtksourceview-5-dev`,
Arch `gtksourceview5`, Void `gtksourceview5-devel`) and a JSON parser (`json-glib`) for the
language server's messages. Runtime: `micro` is no longer needed.

## Given up

micro's multiple cursors (GtkSourceView 5 has none), its macros and its command bar.
