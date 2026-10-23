#!/usr/bin/env bash
# Builds the source-based AUR recipe in the Arch Linux container.
set -euo pipefail

pacman -Syu --noconfirm --needed base-devel
useradd -m builder
work=/home/builder/t-code-aur
install -d -o builder -g builder "$work"
cp /src/packaging/aur/PKGBUILD /src/packaging/aur/.SRCINFO "$work/"
chown -R builder:builder "$work"
su builder -c "cd '$work' && makepkg --printsrcinfo > .SRCINFO.generated &&
  diff -u .SRCINFO .SRCINFO.generated && rm .SRCINFO.generated &&
  makepkg --noconfirm --cleanbuild"
cp "$work"/*.pkg.tar.* /out/
[[ -z "${HOST_OWNER:-}" ]] || chown -R "${HOST_OWNER}" /out
