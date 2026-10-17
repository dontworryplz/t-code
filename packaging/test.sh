#!/usr/bin/env bash
# Installs the packages from packaging/packages/<distro>/ into a fresh container of that distro
# (so the package manager has to pull in every dependency itself) and runs smoke-test.sh there.
# Usage: packaging/test.sh [fedora|arch|debian|all]...   (default: all)
source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

# Installs the package, then what smoke-test.sh needs: a headless X server, a session bus,
# python3 (to drive micro through a pty) and pgrep. Only this version's package ($VERSION, set in the
# container), when older ones are still in packaging/packages.
declare -A INSTALL=(
  [fedora]='dnf -y install $(ls /out/t-code-$VERSION-[0-9]*.x86_64.rpm) xorg-x11-server-Xvfb dbus-daemon
            python3 procps-ng desktop-file-utils fontconfig util-linux'
  [arch]='pacman -Syu --noconfirm && pacman -U --noconfirm $(ls /out/t-code-$VERSION-[0-9]*-x86_64.pkg.tar.*) &&
          pacman -S --noconfirm --needed xorg-server-xvfb dbus python procps-ng desktop-file-utils'
  [debian]='apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y
            $(ls /out/t-code_$VERSION-[0-9]*_amd64.deb) xvfb dbus python3
            procps desktop-file-utils'
)

test_distro() {
  local distro=$1 out="$OUT/$1"
  compgen -G "$out/*t-code*" >/dev/null || die "No $distro package in $out, run packaging/build.sh $distro"
  info "Testing the $distro package in a fresh ${IMAGE[$distro]}"
  run_in_container "$distro" "$out" bash -c "
    set -euo pipefail
    { ${INSTALL[$distro]//$'\n'/ }; } >/tmp/install.log 2>&1 || { tail -40 /tmp/install.log; exit 1; }
    useradd -m tester
    runuser -u tester -- env VERSION=$VERSION bash /src/packaging/smoke-test.sh"
}

targets=("$@")
((${#targets[@]})) || targets=(all)
[[ " ${targets[*]} " == *" all "* ]] && targets=("${DISTROS[@]}")
failed=()
for distro in "${targets[@]}"; do
  case "$distro" in
    fedora | arch | debian) test_distro "$distro" || failed+=("$distro") ;;
    *) die "Unknown target '$distro' (fedora, arch, debian or all)" ;;
  esac
done
((${#failed[@]} == 0)) || die "Failed: ${failed[*]}"
info "All package tests passed: ${targets[*]}"
