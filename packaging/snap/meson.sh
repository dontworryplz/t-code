#!/bin/sh
set -eu
# The distro script's absolute shebang would bypass the bundled Python interpreter.
exec "$SNAP/toolchain/usr/bin/python3" "$SNAP/toolchain/usr/bin/meson" "$@"
