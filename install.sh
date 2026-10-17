#!/usr/bin/env bash
# Builds t-code and installs it under ~/.local:
#   ~/.local/bin/t-code
#   ~/.local/libexec/t-code/t-code-fm
#   ~/.local/libexec/t-code/t-code-lsp                   shares one clangd between editors
#   ~/.local/share/t-code/micro/                         editor config (colors, C syntax, clangd plugin)
#   ~/.local/share/applications/io.github.tcode.desktop app menu entry
#   ~/.local/share/icons/hicolor/scalable/apps/io.github.tcode.svg
# Run it again after pulling changes to update the installed copy.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PREFIX="${PREFIX:-$HOME/.local}"
DATA_HOME="${XDG_DATA_HOME:-$HOME/.local/share}"
BIN="$PREFIX/bin/t-code"
LIBEXEC="$PREFIX/libexec/t-code"
SHARE="$PREFIX/share/t-code"
APP_DIR="$DATA_HOME/applications"
ICON="$DATA_HOME/icons/hicolor/scalable/apps/io.github.tcode.svg"

info() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m!!\033[0m  %s\n' "$*"; }

# 1. Dependencies
missing=()
for cmd in gcc make pkg-config micro clangd; do
  command -v "$cmd" >/dev/null || missing+=("$cmd")
done
if ((${#missing[@]})); then
  warn "Missing required tools: ${missing[*]}"
  warn "Fedora: sudo dnf install gcc make pkgconf micro clang-tools-extra"
  exit 1
fi
if ! pkg-config --exists gtk4 vte-2.91-gtk4 webkitgtk-6.0 md4c-html; then
  warn "GTK4 / VTE / WebKitGTK / md4c development files are missing"
  warn "Fedora: sudo dnf install gtk4-devel vte291-gtk4-devel webkitgtk6.0-devel md4c-devel"
  exit 1
fi
[[ "$(fc-match -f '%{family}' 'Cascadia Code' 2>/dev/null)" == *"Cascadia Code"* ]] ||
  warn "Font 'Cascadia Code' not found (Fedora: sudo dnf install cascadia-code-fonts)"

# 2. Build
info "Building..."
make -C "$ROOT" --no-print-directory

# 3. Programs (a leftover symlink from a manual 'ln -s' setup is replaced by a real copy)
for dst in "$BIN" "$LIBEXEC/t-code-fm" "$LIBEXEC/t-code-lsp"; do
  [[ -L "$dst" ]] && rm "$dst"
done
install -Dm755 "$ROOT/build/t-code" "$BIN"
install -Dm755 "$ROOT/build/t-code-fm" "$LIBEXEC/t-code-fm"
install -Dm755 "$ROOT/build/t-code-lsp" "$LIBEXEC/t-code-lsp"
info "Installed $BIN"

# 4. Editor config. micro keeps its own state (cursor positions, backups) next to it; that stays.
mkdir -p "$SHARE/micro"
for item in settings.json bindings.json colorschemes syntax plug bin; do
  rm -rf "${SHARE:?}/micro/$item"
  cp -r "$ROOT/micro/$item" "$SHARE/micro/$item"
done
info "Installed editor config in $SHARE/micro"

# 5. App menu entry and icon
install -Dm644 "$ROOT/desktop/io.github.tcode.svg" "$ICON"
touch "$DATA_HOME/icons/hicolor" # tells the desktop to rescan its icon cache
mkdir -p "$APP_DIR"
sed "s|@BIN@|$BIN|" "$ROOT/desktop/io.github.tcode.desktop.in" > "$APP_DIR/io.github.tcode.desktop"
command -v update-desktop-database >/dev/null && update-desktop-database "$APP_DIR" 2>/dev/null || true
info "Installed desktop entry: $APP_DIR/io.github.tcode.desktop"

info "Done. Run 't-code [dir]' or open 't-code' from the app menu."
case ":$PATH:" in *":$PREFIX/bin:"*) ;; *) warn "$PREFIX/bin is not in your PATH" ;; esac
