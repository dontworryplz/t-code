# <img src="desktop/io.github.tcode.svg" alt="" width="48" align="top"> t-code

One window, three kinds of terminals, nothing else.

**Version 0.1** — the first release, with packages for Fedora, Arch Linux and Debian
([Install](#install)). `t-code --version` prints the installed version.

![t-code editing its own source: the file manager on the left, src/t-code.c open in the editor with indent guides and the minimap, the shell below, and the status bar](docs/screenshot.png)

<sub>t-code open on its own repository: the file manager with the <code>src/</code> tree expanded, <code>src/t-code.c</code> in the editor with indent guides (the block around the cursor highlighted) and the minimap next to the scrollbar, the shell below, and the status bar with the git branch, clangd's errors and warnings, Build / Run and the cursor position.</sub>

Two small C programs for Linux. Uses 100-150MB RAM (language server is not included, it also uses approximately ~1GB RAM). `t-code` (GTK4) is the window; every pane in it is a real terminal (VTE, the
same widget GNOME's terminals use):

- **left:** `t-code-fm`, a file manager written in plain C (no libraries)
- **right:** editor tabs, each one its own terminal running [micro](https://micro-editor.github.io)
  with clangd checking the code (micro is choosen because vim/neovim usage is hard for new users)
- **bottom:** your shell

All terminals use Cascadia Code and VS Code's Dark Modern colors. C and C++ code is colored the
way VS Code with clangd colors it (`micro/colorschemes/dark-modern.micro`, `micro/syntax/c.yaml`,
`micro/syntax/cpp.yaml`): the colors come from VS Code's own theme files, so functions are yellow,
types teal, variables light blue, macros blue, enum constants bright blue, control flow
(`if`, `return`, `#include`) purple, and comments, control flow and type keywords (`int`,
`static`, `const`, `class`) are italic. clangd tells these apart by meaning; t-code tells them
apart by the shape of the name (`GtkWidget` and `std::vector` are types, `FOO(...)` is a macro,
`TAB_EDITOR` an enum constant), which is right almost always but not every time. VS Code's
colored bracket pairs are not possible in micro. `.h` files open as C++ when they contain
C++ (`class`, `namespace`, `std::`, ...).

micro runs with its own config in `micro/` (your `~/.config/micro` is untouched). Its only plugin,
the LSP client for clangd, is copied into `micro/plug/lsp` at a fixed version, so nothing downloads
or updates by itself.

## Requirements

`micro` (2.0.14 or newer), `clangd`, `git`, the Cascadia Code font (`ripgrep` is optional, for a faster Search
panel), and to build: `gcc` 14 or newer (C23), `make` and the GTK4, VTE, WebKitGTK and md4c
headers. The packages below pull in all of these by themselves.

```sh
sudo dnf install gtk4-devel vte291-gtk4-devel webkitgtk6.0-devel md4c-devel
```

## Install

### With a package (Fedora, Arch Linux, Debian)

t-code 0.1 has packages for Fedora 44, Arch Linux and Debian 13 (trixie), x86_64. Install the one
for your distribution; the package manager pulls in micro, clangd, git and the Cascadia Code font:

```sh
sudo dnf install ./t-code-0.1-1.fc44.x86_64.rpm          # Fedora
sudo pacman -U t-code-0.1-1-x86_64.pkg.tar.zst           # Arch Linux
sudo apt install ./t-code_0.1-1_amd64.deb                # Debian
```

Then start `t-code` from the app menu, or `t-code [folder]` in a terminal. To remove it, use
your package manager (`sudo dnf remove t-code`, `sudo pacman -R t-code`, `sudo apt remove t-code`).

The package puts `t-code` in `/usr/bin`, the file manager and `t-code-lsp` in
`/usr/libexec/t-code/` (`/usr/lib/t-code/` on Arch), the editor config in
`/usr/share/t-code/micro/`, and the app menu entry and icon. Each user gets their own editor
folder in `~/.local/state/t-code/micro/`, where micro keeps its backups and history; it links to
the shared config, so package updates reach it.

### Building the packages yourself

All three packages are built from this repository with one script. The Arch and Debian packages
(and the Fedora one, when you are not on Fedora) are built in containers, so all you need is
`podman` or `docker`:

```sh
packaging/build.sh                # all three, or pick: packaging/build.sh fedora arch debian
packaging/test.sh                 # tries each package out in a fresh container
```

The packages land in `packaging/packages/fedora/`, `arch/` and `debian/`. On Fedora, the RPM is
built directly with `rpmbuild`; install its build dependencies once with
`sudo dnf install rpm-build dnf-plugins-core && sudo dnf builddep packaging/fedora/t-code.spec`.

`packaging/test.sh` installs each package in a fresh container of its distribution (so the
package manager has to find every dependency), checks the installed files, and starts t-code on
a headless X server with a C file: the file manager, micro, `t-code-lsp` and clangd have to come
up, t-code has to quit cleanly, and micro has to load its plugins and save a file. While they
build, the packages also run the tests that need no display.

### From the source, into ~/.local

```sh
./install.sh
```

This builds t-code and installs it under `~/.local`: the `t-code` command, the file manager in
`libexec/t-code/`, the editor config in `share/t-code/micro/`, and a "t-code" entry in the app menu
with its own icon (`desktop/io.github.tcode.svg`, installed to `~/.local/share/icons`). Run it again after changing the code to update the installed copy.

To remove everything it installed:

```sh
./uninstall.sh
```

`make install` installs system-wide instead (`PREFIX=/usr/local` by default, `DESTDIR` for
staging; the packages use it). For development, `make` builds into `build/`, and `build/t-code`
runs straight from the repo.
`make test` runs the tests in `tests/` (the Search panel, minimap and Source Control ones open a
window, so they need a display).

## Usage

```sh
t-code            # current directory
t-code path/dir
t-code a.c b.c    # open files as tabs
```

### Keys

The **⌨ Keybinds** button in the status bar shows all of them, and `Ctrl+Shift+P` (command
palette) finds any command by name.

| Where       | Key                                    | Action                                    |
|-------------|----------------------------------------|-------------------------------------------|
| anywhere    | `Ctrl+P`                               | open a file by typing part of its name    |
| anywhere    | `Ctrl+Shift+F`                         | search the text of all files (the Search tab) |
| anywhere    | `Ctrl+Shift+P`, `F1`                   | command palette                           |
| anywhere    | `Ctrl+G`                               | go to line                                |
| anywhere    | `F5` / `Ctrl+Shift+B`                  | build and run / build only (see below)    |
| anywhere    | `Ctrl+Alt+D` / `Ctrl+Alt+H`            | compare with the last commit / with a commit from the file's history |
| anywhere    | `Ctrl+Shift+V`                         | markdown preview of the current file      |
| anywhere    | `Ctrl+Shift+O`                         | open another project folder               |
| anywhere    | `Ctrl+\`                               | split the editor into two groups / join   |
| anywhere    | `Ctrl+1` / `Ctrl+2`                    | left / right editor group                 |
| anywhere    | `Ctrl+PageDown` / `Ctrl+PageUp`, `Alt+.` / `Alt+,` | next / previous tab           |
| anywhere    | `Alt+1` / `Alt+2` / `Alt+3`            | file manager / editor / shell             |
| anywhere    | `Ctrl+B` / `Ctrl+J`                    | hide / show the file manager / the shell  |
| anywhere    | `F7`                                   | code coloring on / off (all tabs)         |
| anywhere    | `Ctrl+=` / `Ctrl+-` / `Ctrl+0`         | bigger / smaller / normal text            |
| shell       | `Ctrl+Click` on `file.c:12:5`          | open the file at that line (compiler messages) |
| tab         | left click / middle click / `×`        | switch to / close / close                 |
| files       | click, `Enter`, `l`, `→`               | open folder, or open the file in a tab    |
| files       | `h`, `Backspace`, `←`                  | collapse folder / go to parent            |
| files       | `↑` `↓`, `j` `k`, wheel                 | move / scroll                             |
| files       | `a` / `r` / `d`, `Delete`              | new file (end with `/` for a folder) / rename / delete |
| files       | `Ctrl+X` / `Ctrl+C` / `Ctrl+V`         | cut / copy / paste files and folders      |
| files       | drag a row onto a folder               | move it there                             |
| files       | `.` / `R`                              | hide / show dotfiles (shown by default) / refresh |
| files       | right click                            | Open, Open to the Side, Open Preview, Compare with HEAD, File History, New File/Folder, Open in Terminal, Cut/Copy/Paste, Copy (Relative) Path, Rename, Delete, Open Folder as Project |
| editor      | `Ctrl+S` / `Ctrl+Q`                    | save / close this tab                     |
| editor      | `Ctrl+F`, `F3` / `Shift+F3`            | find, next / previous                     |
| editor      | `Tab`, `Ctrl+Space`                    | completion                                |
| editor      | `Alt+K`                                | signature of the symbol under the cursor  |
| editor      | `Alt+D` / `Alt+R`                      | go to definition / list references        |
| editor      | `Alt+F`                                | format the file with clangd               |

### What else is there

**Editing.** Files with changes are saved every second (micro's `autosave`), and closing a tab
saves too, so `Ctrl+S` is rarely needed; a tab shows `●` while its file has unsaved changes.
clangd marks problem lines with `>>` in the gutter, and the minimap shows them as red (errors)
or yellow (warnings) lines across its width; move the cursor onto the line to read the message at
the bottom. Everything else in the editor is plain micro (`Ctrl+C`/`Ctrl+V`, `Ctrl+Z`,
...; `Ctrl+E` opens its command bar, `help` there shows its help).

**Status bar**, like VS Code's: the git branch (`*` when there are changes), clangd's errors and
warnings in the open files, **⚙ Build** and **▶ Run** with what they would build, the cursor's
line and column, and **⌨ Keybinds**.

**Build and run.** F5 / Run saves, then builds and runs in the shell (Build stops after
building), looking upwards from the current file: a `CMakeLists.txt` is configured and built in
`build/` and the newest program there runs; a `Makefile` runs `make`, then `make run` if there is
a `run` target; otherwise the current C or C++ file is compiled on its own (with AddressSanitizer
and UBSan) into `build/` and run. Ctrl+click a `file.c:12:5` in the compiler's output to go there.

**Git.** The editor marks changed lines in the gutter like VS Code (blue changed, green added, red
where lines were deleted), following new commits by itself. **Compare with HEAD** (`Ctrl+Alt+D`)
and **File History** (`Ctrl+Alt+H`, pick a commit) open the old version read-only to the side:
both gutters then mark the differences between the two. Closing the old version goes back to
comparing with the last commit. The file manager colors files by their git state (modified,
added, untracked, conflicted, ignored) with the letter at the end of the row.

**File manager.** It refreshes by itself when files are added, removed or saved, and after a git
commit. Files and folders can be cut, copied and pasted (a name that is taken gets " copy"), or
dragged onto a folder.

**Split editor.** `Ctrl+\` (or the title bar button) puts a second group of tabs next to the
first. New files open in the group you are in; "Open to the Side" in the file manager opens a
file straight into the right group. Tabs can be dragged between the groups, and the right group
closes when its last tab does. A file is never open in both groups at once, since two editors
saving the same file would overwrite each other.

**Projects.** The title bar has buttons to open another folder and to pick a recent one.
Switching remembers and closes the current project's tabs, restarts the file manager and the
shell in the new folder, and brings back that folder's tabs from last time. t-code also remembers
the window size, pane sizes, hidden panes and text size (in `~/.config/t-code/session.ini`).

**Search** (`Ctrl+Shift+F`), a tab next to Files and Source Control, like VS Code's: results
come as you type, grouped by file with the matches marked. **Aa**, **ab** and **.\*** next to the
text turn on Match Case, Match Whole Word and regular expressions; **⋯** shows "files to include"
and "files to exclude" (comma separated globs such as `*.c, src`). Click a file to fold its
results, a result to open the file there. ripgrep skips what `.gitignore` ignores; without it,
grep searches everything but `.git` and `build`.

**Settings** (`Ctrl+,` or the gear in the title bar): minimap, code coloring, indent guides,
word wrap (off by default; long lines then continue on the next row instead of running off the
screen), tab size, spaces for Tab, detecting a file's own indentation, autosave, fonts, text size
and reopening the last project. Changes apply at once and are kept in `session.ini`.

**Markdown preview** opens as a tab and follows the file as you type. Links to other markdown
files open their preview, other local files open in an editor, and web links open in the browser.

## Files

```
src/                 t-code, the window (see src/app.h for how the parts call each other)
  t-code.c           building the window, main()
  terminal.c         terminals; Ctrl+click on compiler messages in the shell
  tabs.c             editor tabs and the two groups
  commands.c         every command: shortcuts, command palette and the Keybinds list come from it
  picker.c           the popup list used by Ctrl+P, the palette and file history
  quickopen.c        Ctrl+P
  search.c           the Search tab, Ctrl+Shift+F (ripgrep)
  searchmatch.c      its command lines and output parsing (GLib only, tested on its own)
  editorview.c       next to each editor: the minimap, the scrollbar and the indent guides
  status.c           the status bar (and the thin title bar's style)
  keybinds.c         the Keybinds window
  project.c          switching folders, recent folders
  socket.c           requests from the file manager over $TCODE_SOCKET
  gitcompare.c       comparing with HEAD or a commit from the history
  run.c              build and run
  session.c          remembering the window and the tabs
  preview.c          markdown preview
  fm/                t-code-fm, the file manager (see src/fm/fm.h)
tests/               make test: searchmatch.c, the Search panel, the minimap and Source Control
micro/               editor config: settings, colors, C and C++ syntax, the clangd plugin, and the tcode
                     plugin that reports the editor's state to t-code and sets the git comparison
desktop/             app menu entry template and the app icon
packaging/           build.sh, test.sh: Fedora (fedora/t-code.spec), Arch (arch/PKGBUILD) and Debian
                     (debian/debian/) packages, built into packaging/packages/ (not in git)
install.sh, uninstall.sh
LICENSE
Makefile             VERSION, make install; also writes build/compile_commands.json, so clangd
                     understands this code
```

## License

MIT, see [LICENSE](LICENSE). The clangd plugin in `micro/plug/lsp` is by Robert Kunze, also MIT
(its own LICENSE is next to it).
