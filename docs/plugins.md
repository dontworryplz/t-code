# Plugin System Design for t-code

This document outlines the proposed plugin architecture for `t-code`.

## Philosophy

`t-code`'s core strength is its fast startup, minimal memory footprint (~70 MB PSS with 25 open tabs), and responsiveness. Any plugin mechanism must uphold these invariants:

1. **Zero idle overhead:** Installing plugins must not degrade performance or increase idle RAM if those plugins are not active.
2. **Crash isolation:** A malfunctioning plugin must never crash the editor window or freeze the GTK main loop.
3. **No embedded heavy runtimes:** Avoid embedding a heavy JavaScript engine (V8, QuickJS) or Python runtime inside the core binary.
4. **Simple, language-agnostic integration:** Anyone can author a plugin using shell scripts, C, Rust, Go, Python, or any tool that reads and writes standard streams or sockets.

---

## Architecture: Two-Tier Plugin Model

### Tier 1: IPC Process Plugins (Recommended & Primary)

Plugins run as independent subprocesses communicating with `t-code` via its UNIX domain socket (`$TCODE_SOCKET`, documented in `src/ipc/socket.h`).

#### Manifest (`plugin.json`)

Plugins are discovered in `~/.config/t-code/plugins/<plugin_id>/` and `/usr/share/t-code/plugins/<plugin_id>/`.

```json
{
  "name": "Git Blame",
  "version": "1.0.0",
  "description": "Shows blame for the active line",
  "executable": "blame.sh",
  "commands": [
    {
      "id": "git-blame.show",
      "name": "Git: Show Line Blame",
      "keys": "Ctrl+Alt+B",
      "trigger": "<Control><Alt>b"
    }
  ]
}
```

`keys` is the label shown beside the command. `trigger` is GTK's shortcut syntax; commands without
it remain available in the palette. A duplicate trigger keeps the first discovered command, and an
invalid trigger is skipped. Reload Installed Plugins refreshes bindings in both windows.

#### Socket Protocol

The control socket (`$TCODE_SOCKET`) already supports line-oriented command routing:

| Command | Arguments | Description |
|---|---|---|
| `open` | `<path> [line] [column]` | Opens a file at the given position |
| `get-file` | *(none)* | Returns the active tab's file path |
| `get-project` | *(none)* | Returns current project directory root |
| `get-text` | *(none)* | Returns the active editor buffer, terminated by NUL so multiline text is unambiguous |
| `insert` | `<text>` | Inserts text at the cursor position |
| `append` | `<text>` | Appends text at the end of the buffer |
| `replace-all`| `<text>` | Replaces the buffer content |
| `save` | *(none)* | Saves the active editor tab |
| `say` | `<message>` | Displays a message on the status bar |
| `copy` | `<text>` | Puts text into the system clipboard |
| `preview` | `<file>` | Opens Markdown preview tab |
| `diff` | `<path> <old_path>` | Opens side-by-side diff comparison |
| `debug` | `<program>` | Hands binary to the integrated debugger |


Requests normally end with a newline. Send NUL-terminated requests when an argument contains newlines, such as `replace-all`; a connection using NUL framing should terminate each request with NUL. `get-file` and `get-project` return one newline-terminated response. `get-text` returns the full buffer followed by a NUL byte, preserving embedded newlines and distinguishing an empty buffer from a closed connection.

#### Execution Flow

1. User invokes a plugin command via Command Palette (`Ctrl+P` > `>`) or assigned shortcut.
2. `t-code` sets environment variables (`$TCODE_SOCKET`, `$TCODE_FILE`, `$TCODE_LINE`, `$TCODE_PROJECT`) and spawns the plugin executable asynchronously via GLib's `g_spawn_async_with_pipes()`.
3. The plugin connects to `$TCODE_SOCKET`, inspects or updates buffer state, and terminates.
4. Output (stderr/stdout) can be piped to the status bar or the Problems panel.

---

### Tier 2: Native GModule C Extensions (Future / Advanced)

For extensions requiring deep GTK4 widget hierarchy integration (such as custom panels, custom diff gutters, or custom syntax highlighters), a C GModule (`.so`) interface will be exposed:

```c
typedef struct {
  const char *name;
  const char *version;
  void (*init)(App *t);
  void (*shutdown)(App *t);
} TCodePlugin;
```

Native plugins are loaded via `g_module_open()` on demand. They are intended for performance-critical extensions where subprocess IPC latency is unacceptable.

---

## Roadmap

- **v0.4:** Stabilize socket commands (`get-file`, `get-text`, `replace-all`, `say`), document `$TCODE_SOCKET`.
- **v0.5:** Plugin manifest discovery (`~/.config/t-code/plugins/`), palette command injection, and keyboard shortcut binding.
- **v0.6:** Event notification hooks (e.g., `did_open`, `did_save` broadcasts over a socket stream).
