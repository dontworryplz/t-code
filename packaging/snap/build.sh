#!/usr/bin/env bash
# Usage: bash packaging/snap/build.sh [source-tree] [output-directory]
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source="$(cd "${1:-$here/../..}" && pwd)"
output="${2:-$source/packaging/packages/snap}"
mkdir -p "$output"
output="$(cd "$output" && pwd)"
version="$(sed -n 's/^VERSION *:= *//p' "$source/Makefile")"
[[ -n "$version" ]] || { echo 'No Makefile version' >&2; exit 1; }
grep -qx "version: '$version'" "$source/packaging/snap/snapcraft.yaml" || {
  echo 'Snap version and Makefile disagree' >&2
  exit 1
}
[[ "$(uname -m)" == x86_64 && "$(uname -s)" == Linux ]] || {
  echo 'Build on a Linux amd64 host/VM with Snapcraft 9 and initialized LXD' >&2
  exit 1
}

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
# Use a clean copy; never let host objects, previous packages or Snapcraft state enter the build.
tar -C "$source" --exclude='./.git' --exclude='./build' --exclude='./parts' \
  --exclude='./stage' --exclude='./prime' --exclude='./.snapcraft' \
  --exclude='./packaging/packages' --exclude='*.snap' -cf - . |
  tar -C "$work" -xf -
mkdir -p "$work/snap"
cp "$work/packaging/snap/snapcraft.yaml" "$work/snap/snapcraft.yaml"

# Record the tools/providers used for this build; package manifests retain resolved apt versions.
{
  snapcraft --version
  snap list snapcraft
  snap version
  lxc version
} > "$output/build-environment.txt"
(
  cd "$work"
  SNAPCRAFT_BUILD_INFO=1 snapcraft pack --use-lxd --output "$output/"
)
artifact="$output/t-code_${version}_amd64.snap"
[[ -f "$artifact" ]] || { echo "Snapcraft did not produce $artifact" >&2; exit 1; }
unsquashfs -cat "$artifact" snap/manifest.yaml > "$output/build-manifest.yaml"
(
  cd "$output"
  sha256sum "${artifact##*/}" > SHA256SUMS
)
printf '%s\n' "$artifact"
