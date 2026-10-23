#!/usr/bin/env bash
# Builds the t-code AppImage into packaging/packages/appimage/:
# a single file without an install (t-code, GTK4, GtkSourceView, VTE and what they need are
# bundled with linuxdeploy), built in a Debian container like the other packages above. It runs on
# the distributions in lib.sh's DISTROS and more, x86_64, at least; the package managers' versions
# above are smaller to pull down, so this is for the rest.
# Usage: packaging/appimage.sh   (podman or docker like packaging/build.sh)
source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
build_appimage
