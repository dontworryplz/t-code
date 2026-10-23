#!/usr/bin/env bash
set -euo pipefail

FILE="${TCODE_ACTIVE_FILE:-}"

if [[ -n "$FILE" && -f "$FILE" ]]; then
  WORDS=$(wc -w < "$FILE" | tr -d ' ')
  LINES=$(wc -l < "$FILE" | tr -d ' ')
  MSG="Word Count: $WORDS words, $LINES lines"
else
  MSG="Word Count: No active saved file"
fi

if [[ -n "${TCODE_SOCKET:-}" && -S "${TCODE_SOCKET:-}" ]]; then
  if command -v python3 >/dev/null 2>&1; then
    python3 -c '
import socket, sys, os
sock_path = os.environ.get("TCODE_SOCKET")
msg = sys.argv[1]
if sock_path and os.path.exists(sock_path):
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.connect(sock_path)
        s.sendall(f"say {msg}\n".encode("utf-8"))
        s.close()
    except Exception:
        pass
' "$MSG"
  elif command -v nc >/dev/null 2>&1; then
    printf 'say %s\n' "$MSG" | nc -U "$TCODE_SOCKET" 2>/dev/null || true
  fi
fi

echo "$MSG"
