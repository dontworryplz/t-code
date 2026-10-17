# Runs as a normal user inside a fresh container with the t-code package installed (test.sh):
# checks the installed files, starts t-code on a headless X server with five C files and checks
# that the file manager, micro (only for the tab shown), the shared clangd (with fewer malloc
# arenas) and the per-user micro config come up, and that micro can save a file through that config.
set -uo pipefail

failures=0
pass() { printf '  \033[1;32mok\033[0m   %s\n' "$1"; }
fail() { printf '  \033[1;31mFAIL\033[0m %s\n' "$1"; failures=$((failures + 1)); }
check() { # <description> <command...>
  local what=$1
  shift
  if "$@" >/dev/null 2>&1; then pass "$what"; else fail "$what"; fi
}

share=/usr/share/t-code/micro
state="$HOME/.local/state/t-code/micro"
helpers=/usr/libexec/t-code
[[ -d $helpers ]] || helpers=/usr/lib/t-code

echo "Installed files"
check "t-code --version prints 't-code $VERSION'" test "$(t-code --version)" = "t-code $VERSION"
check "t-code-fm and t-code-lsp in $helpers" test -x "$helpers/t-code-fm" -a -x "$helpers/t-code-lsp"
check "micro config in $share" test -f "$share/settings.json" -a -f "$share/plug/lsp/main.lua" \
  -a -f "$share/plug/tcode/tcode.lua" -a -f "$share/syntax/c.yaml"
check "micro-clip is executable" test -x "$share/bin/micro-clip"
check "desktop entry is valid" desktop-file-validate /usr/share/applications/io.github.tcode.desktop
check "icon installed" test -f /usr/share/icons/hicolor/scalable/apps/io.github.tcode.svg
for cmd in micro clangd git; do
  check "dependency '$cmd' installed" command -v "$cmd"
done
if [[ -n "${NO_FONT_PACKAGE:-}" ]]; then
  check "a monospace font for the Cascadia Code fallback" sh -c "fc-match -f '%{spacing}' monospace | grep -q 100"
else
  check "Cascadia Code font installed" sh -c "fc-match -f '%{family}' 'Cascadia Code' | grep -q 'Cascadia Code'"
fi

echo "Running t-code"
export XDG_RUNTIME_DIR="/tmp/runtime-$USER" DISPLAY=:99 GDK_BACKEND=x11 GSK_RENDERER=cairo
mkdir -p -m 700 "$XDG_RUNTIME_DIR"
project="$HOME/project"
mkdir -p "$project"
printf '#include <stdio.h>\n\nint main(void)\n{\n    printf("hi\\n");\n    return 0;\n}\n' > "$project/main.c"
for f in a b c d; do printf 'int %s(void) { return 0; }\n' "$f" > "$project/$f.c"; done
git -C "$project" init -q
Xvfb :99 -screen 0 1600x1000x24 >/dev/null 2>&1 &
xvfb=$!
sleep 2
(cd "$project" && exec dbus-run-session -- t-code "$project"/{a,b,c,d}.c "$project/main.c") > /tmp/t-code.log 2>&1 &
app=$!
# clangd starts when micro's lsp plugin asks t-code for it; give it time. (clangd names its main
# thread "clangd.main", so it is found by its command line.)
for _ in $(seq 30); do pgrep -u "$USER" -f '^clangd ' >/dev/null && break; sleep 1; done

tcode=$(pgrep -u "$USER" -x t-code | head -1)
check "t-code is running" test -n "$tcode"
check "file manager (t-code-fm) started" pgrep -u "$USER" -f "$helpers/t-code-fm"
check "micro started with the per-user config" pgrep -u "$USER" -f "micro -config-dir $state"
check "micro's lsp plugin connected through t-code-lsp" pgrep -u "$USER" -f "$helpers/t-code-lsp"
check "shared clangd started" pgrep -u "$USER" -f '^clangd '
# Five tabs, one shown: the hidden tabs' editors start when they are first shown.
check "micro runs for the shown tab only (1 of 5)" \
  test "$(pgrep -u "$USER" -fc "micro -config-dir $state")" -eq 1
check "clangd runs with 2 malloc arenas" \
  sh -c "tr '\\0' '\\n' < /proc/\$(pgrep -u $USER -f '^clangd ' | head -1)/environ | grep -q arena_max=2"
check "per-user config links to the shared one" test "$(readlink "$state/plug")" = "$share/plug"
check "control socket created" sh -c "ls $XDG_RUNTIME_DIR/t-code-*.sock"
# The Markdown preview: WebKit's web content process has to start.
printf '# Notes\n\nhello\n' > "$project/README.md"
python3 -I -c 'import socket, sys; s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1]); s.send(sys.argv[2].encode())' \
  "$XDG_RUNTIME_DIR/t-code-$tcode.sock" "preview $project/README.md"$'\n' 2>/dev/null
for _ in $(seq 15); do pgrep -u "$USER" -f WebKitWebProcess >/dev/null && break; sleep 1; done
check "Markdown preview started WebKit's web process" pgrep -u "$USER" -f WebKitWebProcess
kill -TERM "${tcode:-0}" 2>/dev/null
for _ in $(seq 10); do kill -0 "$app" 2>/dev/null || break; sleep 1; done
check "t-code quits on SIGTERM" sh -c "! pgrep -u $USER -x t-code"
check "control socket removed on quit" sh -c "! ls $XDG_RUNTIME_DIR/t-code-*.sock"
check "no GTK criticals in the log" sh -c "! grep -E 'CRITICAL|segfault|Segmentation' /tmp/t-code.log"
kill "$xvfb" 2>/dev/null

echo "Saving in micro through the per-user config"
# The shared config is read-only; micro saves files through its config dir, so this is what
# fails without the per-user one ("Permission denied ... save this file using sudo?").
printf 'hello\n' > "$project/note.txt"
python3 -I - "$state" "$project/note.txt" <<'EOF'
import os, pty, select, sys, time

pid, fd = pty.fork()
if pid == 0:
    os.environ["TERM"] = "xterm-256color"
    os.execvp("micro", ["micro", "-config-dir", sys.argv[1], sys.argv[2]])

screen = b""

def pump(seconds):
    global screen
    end = time.time() + seconds
    while time.time() < end:
        if select.select([fd], [], [], 0.1)[0]:
            try:
                screen += os.read(fd, 65536)
            except OSError:
                return

pump(3)
for keys in (b"x", b"\x13", b"\x11"):  # type x, Ctrl+S, Ctrl+Q
    os.write(fd, keys)
    pump(1)
try:
    os.kill(pid, 9)
except ProcessLookupError:
    pass
os.waitpid(pid, 0)
open("/tmp/micro.out", "wb").write(screen)
EOF
# A plugin error (an API the installed micro lacks) stops micro at "Press enter to continue".
check "micro's plugins load without errors" sh -c "! grep -aq 'Press enter to continue' /tmp/micro.out"
check "micro saved the file" grep -qx xhello "$project/note.txt"

if ((failures)); then
  echo "--- t-code log ---"
  cat /tmp/t-code.log
  echo "--- micro screen ---"
  sed 's/\x1b\[[0-9;?]*[a-zA-Z]/ /g' /tmp/micro.out | tr -s ' ' | tail -c 1500
  echo "$failures check(s) failed"
  exit 1
fi
echo "All checks passed"
