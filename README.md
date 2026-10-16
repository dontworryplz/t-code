# t-code

One window, three kinds of terminals, nothing else.

![t-code editing a small C project: file manager on the left, editor tabs with a clangd warning, make output in the shell below](docs/screenshot.png)

<sub>A small Makefile project: three files open as tabs, clangd marks an unused variable with <code>&gt;&gt;</code>, and the shell below has built and run the program.</sub>

Two small C programs for Linux. `t-code` (GTK4) is the window; every pane in it is a real terminal (VTE, the
same widget GNOME's terminals use):

- **left:** `t-code-fm`, a file manager written in plain C (no libraries)
- **right:** editor tabs, each one its own terminal running [micro](https://micro-editor.github.io)
  with clangd checking the code
- **bottom:** your shell

All terminals use Cascadia Code and VS Code's Dark Modern colors, and C code is colored the way
VS Code colors it (`micro/colorschemes/dark-modern.micro`, `micro/syntax/c.yaml`).

micro runs with its own config in `micro/` (your `~/.config/micro` is untouched). Its only plugin,
the LSP client for clangd, is copied into `micro/plug/lsp` at a fixed version, so nothing downloads
or updates by itself.

## Requirements

`micro`, `clangd`, `git`, the Cascadia Code font, and to build: `gcc`, `make` and the GTK4, VTE,
WebKitGTK and md4c headers.

```sh
sudo dnf install gtk4-devel vte291-gtk4-devel webkitgtk6.0-devel md4c-devel
```

## Install

```sh
./install.sh
```

This builds t-code and installs it under `~/.local`: the `t-code` command, the file manager in
`libexec/t-code/`, the editor config in `share/t-code/micro/`, and a "t-code" entry in the app menu
(same icon as C IDE). Run it again after changing the code to update the installed copy.

To remove everything it installed:

```sh
./uninstall.sh
```

For development, `make` builds into `build/`, and `build/t-code` runs straight from the repo.

## Usage

```sh
t-code            # current directory
t-code path/dir
t-code a.c b.c    # open files as tabs
```

### Keys

| Where       | Key                                    | Action                                    |
|-------------|----------------------------------------|-------------------------------------------|
| anywhere    | `Ctrl+PageDown` / `Ctrl+PageUp`        | next / previous tab                       |
| anywhere    | `Alt+.` / `Alt+,`                      | next / previous tab                       |
| anywhere    | `Alt+1` / `Alt+2` / `Alt+3`            | file manager / editor / shell             |
| anywhere    | `F5` / `Ctrl+Shift+B`                  | build and run / build only (see below)    |
| anywhere    | `Ctrl+Shift+O`                         | open another project folder               |
| anywhere    | `Ctrl+\`                               | split the editor into two groups / join   |
| anywhere    | `Ctrl+1` / `Ctrl+2`                    | left / right editor group                 |
| anywhere    | `F7`                                   | code coloring on / off (all tabs)         |
| anywhere    | `Ctrl+P`                               | open a file by typing part of its name    |
| anywhere    | `Ctrl+Shift+V`                         | markdown preview of the current file      |
| anywhere    | `Ctrl+B` / `Ctrl+J`                    | hide / show the file manager / the shell  |
| anywhere    | `Ctrl+=` / `Ctrl+-` / `Ctrl+0`         | bigger / smaller / normal text            |
| tab         | left click / middle click / `×`        | switch to / close / close                 |
| files       | click, `Enter`, `l`, `→`               | open folder, or open the file in a tab    |
| files       | `h`, `Backspace`, `←`                  | collapse folder / go to parent            |
| files       | `↑` `↓`, `j` `k`, wheel                 | move / scroll                             |
| files       | `a` / `r` / `d`, `Delete`              | new file (end with `/` for a folder) / rename / delete |
| files       | `.` / `R`                              | show hidden files / refresh               |
| files       | right click                            | Open, Open to the Side, Open Preview (markdown), New File/Folder, Open in Terminal, Copy (Relative) Path, Rename, Delete |
| editor      | `Ctrl+S` / `Ctrl+Q`                    | save / close this tab                     |
| editor      | `Tab`, `Ctrl+Space`                    | completion                                |
| editor      | `Alt+K`                                | signature of the symbol under the cursor  |
| editor      | `Alt+D` / `Alt+R`                      | go to definition / list references        |
| editor      | `Alt+F`                                | format the file with clangd               |

Tabs can be dragged to reorder them, and the dividers between panes can be dragged to resize.
Files with changes are saved every second (micro's `autosave`), and closing a tab
saves too, so `Ctrl+S` is rarely needed; a tab shows `●` while its file has unsaved changes.

The title bar has buttons to open another folder, pick one of the recent folders, and split the
editor. Switching folders remembers and closes the current project's tabs, restarts the file
manager and the shell in the new folder, and brings back that folder's tabs from last time.

**Split editor:** `Ctrl+\` (or the button) puts a second group of tabs next to the first, to
compare two files. New files open in the group you are in; "Open to the Side" in the file
manager opens a file straight into the right group. Tabs can be dragged between the groups, and
the right group closes when its last tab does. A file is never open in both groups at once, since
two editors saving the same file would overwrite each other.

The **status bar** at the bottom has **⚙ Build** and **▶ Run** buttons and shows what they would
build. **F5** / Run saves, then builds and runs in the shell (Build stops after building),
looking upwards from the current file:
a `CMakeLists.txt` is configured and built in `build/` and the newest program there runs; a
`Makefile` runs `make`, then `make run` if there is a `run` target; otherwise the current C or C++
file is compiled on its own (with AddressSanitizer and UBSan) into `build/` and run.

In a git repository the editor marks changed lines in the gutter like VS Code, compared with the
last commit: a blue bar for changed lines, green for added ones, and a red mark where lines were
deleted. The file manager colors files by their git state like VS Code (modified, added, untracked,
conflicted, ignored) and puts the letter at the end of the row.

The **markdown preview** opens as a tab and follows the file as you type (autosave). Links to
other markdown files open their preview, other local files open in an editor, and web links open
in the browser.

t-code remembers the window size, pane sizes, hidden panes and text size, and for each project
directory the open tabs (in `~/.config/t-code/session.ini`). Starting `t-code` in a directory
without naming files brings that directory's tabs back.

clangd marks problem lines with `>>` in the gutter; move the cursor onto the line to read the
message at the bottom. Everything else in the editor is plain micro (`Ctrl+C`/`Ctrl+V`, `Ctrl+Z`,
`Ctrl+F`, ..., `Ctrl+G` for its help).

## Files

```
src/t-code.c     the window: panes, tabs, shortcuts, and a unix socket the file manager uses
                 ("open <path>"-style lines, socket path in $TCODE_SOCKET)
src/run.c        build and run (F5, Ctrl+Shift+B, status bar buttons)
src/session.c    remembering the window and the tabs
src/quickopen.c  Ctrl+P
src/preview.c    markdown preview
src/fm.c         the file manager
micro/           editor config: settings, colors, C syntax, the clangd plugin, and the tcode
                 plugin that reports the open file and unsaved state to t-code
desktop/         app menu entry template
install.sh, uninstall.sh
Makefile
```
