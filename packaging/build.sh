#!/usr/bin/env bash
# Builds the t-code packages into packaging/packages/<distro>/:
#   fedora  .rpm (+ .src.rpm), with rpmbuild on this machine when it runs Fedora, else in a container
#   arch    .pkg.tar.zst, in an Arch Linux container
#   debian  .deb, in a Debian 13 (trixie) container
# Usage: packaging/build.sh [fedora|arch|debian|all]...   (default: all)
# Then packaging/test.sh installs them in fresh containers and checks that they work.
source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

build_fedora() {
  local out="$OUT/fedora"
  rm -rf "$out" && mkdir -p "$out"
  if [[ "$(. /etc/os-release && echo "$ID")" == fedora ]] && command -v rpmbuild >/dev/null; then
    local top
    top="$(mktemp -d)"
    trap 'rm -rf "$top"' RETURN
    make_tarball "$top/SOURCES"
    info "Building the RPM with this machine's rpmbuild"
    rpmbuild -ba --define "_topdir $top" "$PACKAGING/fedora/t-code.spec" ||
      die "rpmbuild failed (missing build dependencies? sudo dnf builddep packaging/fedora/t-code.spec)"
    cp "$top"/RPMS/*/*.rpm "$top"/SRPMS/*.rpm "$out/"
  else
    make_tarball "$out"
    info "Building the RPM in ${IMAGE[fedora]}"
    run_in_container fedora "$out" bash /src/packaging/fedora/build-in-container.sh
    rm -f "$out/t-code-$VERSION.tar.gz"
  fi
}

build_arch() {
  local out="$OUT/arch"
  rm -rf "$out" && make_tarball "$out"
  info "Building the Arch package in ${IMAGE[arch]}"
  run_in_container arch "$out" bash /src/packaging/arch/build-in-container.sh
  rm -f "$out/t-code-$VERSION.tar.gz"
}

build_debian() {
  local out="$OUT/debian"
  rm -rf "$out" && make_tarball "$out"
  info "Building the Debian package in ${IMAGE[debian]}"
  run_in_container debian "$out" bash /src/packaging/debian/build-in-container.sh
  rm -f "$out/t-code-$VERSION.tar.gz"
}

targets=("$@")
((${#targets[@]})) || targets=(all)
[[ " ${targets[*]} " == *" all "* ]] && targets=("${DISTROS[@]}")
check_versions
for distro in "${targets[@]}"; do
  case "$distro" in
    fedora | arch | debian) "build_$distro" ;;
    *) die "Unknown target '$distro' (fedora, arch, debian or all)" ;;
  esac
done
info "Packages (t-code $VERSION):"
find "$OUT" -type f \( -name '*.rpm' -o -name '*.pkg.tar.*' -o -name '*.deb' \) | sort |
  sed "s|^$ROOT/|  |"
