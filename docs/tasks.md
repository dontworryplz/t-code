# Project Tasks & Launch Profiles

`t-code` supports configuring custom build, run, and debug tasks via `.t-code/tasks.json` or `t-code.json` in your project root.

When this file is present, it takes precedence over heuristic build detection (such as scanning `CMakeCache.txt` for executables).

## Quick Example: Single Configuration

```json
{
  "build": "make -j4",
  "run": "./build/t-code",
  "debug": "./build/t-code",
  "args": ["src/main.c"],
  "env": {
    "G_MESSAGES_DEBUG": "all",
    "TCODE_DEV": "1"
  },
  "cwd": "."
}
```

## Multi-Profile Configuration

You can define multiple named profiles (e.g. `debug`, `release`, `asan`) and select the active one with `"default"`:

```json
{
  "default": "debug",
  "profiles": {
    "debug": {
      "build": "cmake --build build --config Debug",
      "run": "build/bin/myapp",
      "debug": "build/bin/myapp",
      "args": ["--port", "8080"]
    },
    "release": {
      "build": "cmake --build build --config Release",
      "run": "build/bin/myapp",
      "args": ["--production"]
    },
    "test": {
      "build": "ctest --test-dir build --output-on-failure",
      "run": "build/tests/unit_tests"
    }
  }
}
```

## Fields

| Field | Type | Description |
|---|---|---|
| `build` | string | Shell command executed on Build (`Ctrl+Shift+B`) |
| `run` | string | Command or executable launched on Run (`Ctrl+F5`) |
| `debug` | string | Binary path handed to GDB on Debug (`F5`) |
| `args` | array or string | Arguments passed to `run` (e.g. `["--verbose", "-n"]`) |
| `env` | object | Environment variables injected before running (e.g. `{"PORT": "3000"}`) |
| `cwd` | string | Working directory relative to project root |

## Shortcuts

- `Ctrl+Shift+B`: Runs the `build` task.
- `Ctrl+F5`: Builds (if configured) and executes `run` with arguments, environment, and working directory in the integrated shell.
- `F5`: Builds and launches the debugger on `debug` (or `run`).
