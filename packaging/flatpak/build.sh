#!/usr/bin/env bash
# Build the pinned release, or replace only its source with a working tree for CI.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source_dir=
out="$here/../packages/flatpak"
usage() {
  echo "Usage: bash packaging/flatpak/build.sh [--local-source DIR] [OUTPUT_DIR]" >&2
  exit 2
}
if [[ "${1:-}" == --local-source ]]; then
  (($# >= 2)) || usage
  source_dir="$(cd "$2" && pwd)"
  shift 2
fi
(($# <= 1)) || usage
if (($#)); then
  [[ "$1" != -* ]] || usage
  out=$1
fi
for command in flatpak flatpak-builder jq; do
  command -v "$command" >/dev/null || { echo "Missing build prerequisite: $command" >&2; exit 1; }
done
version="$(jq -er '."x-tcode-version"' "$here/io.github.tcode.json")"
arch="${FLATPAK_ARCH:-$(flatpak --default-arch)}"
case "$arch" in
  x86_64 | aarch64) ;;
  *) echo "Unsupported Flatpak architecture: $arch" >&2; exit 1 ;;
esac
if [[ -n "$source_dir" ]]; then
  read -r key operator source_version < "$source_dir/Makefile"
  [[ "$key" == VERSION && "$operator" == ':=' && "$source_version" == "$version" ]] || {
    echo "Local source Makefile must declare VERSION := $version" >&2
    exit 1
  }
fi
mkdir -p "$out"
out="$(cd "$out" && pwd)"
[[ "$out" != "$here" && "$out" != "$source_dir" ]] || {
  echo "The output directory must not be the package directory or local source root" >&2
  exit 1
}
skip_output=
if [[ -n "$source_dir" && "$out" == "$source_dir/"* ]]; then
  skip_output="${out#"$source_dir"/}"
fi
manifest="$out/io.github.tcode.json"

# Absolute metadata paths keep both the checkout and an exported source tree read-only.
jq --arg packaging "$here" --arg source "$source_dir" --arg output "$skip_output" '
  .modules |= map(
    if .name == "t-code" then
      .sources |= map(
        if .["x-tcode-source"] == true and $source != "" then
          {
            "type": "dir",
            "path": $source,
            "skip": ([".git", "build", ".flatpak-builder", "packaging/packages", $output] | map(select(length > 0)))
          }
        elif .type == "file" and has("path") then
          .path = ($packaging + "/" + .path)
        else . end
      )
    else . end
  )
' "$here/io.github.tcode.json" > "$manifest"

flatpak --user remote-add --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo
flatpak-builder --user --install-deps-from=flathub --force-clean \
  --disable-rofiles-fuse --sandbox --arch="$arch" \
  --state-dir="$out/state" --repo="$out/repo" \
  "$out/build" "$manifest"
bundle="$out/t-code-$version-$arch.flatpak"
flatpak build-bundle --arch="$arch" \
  --runtime-repo=https://dl.flathub.org/repo/flathub.flatpakrepo \
  "$out/repo" "$bundle" io.github.tcode stable
printf 'Bundle: %s\nRepository: %s\n' "$bundle" "$out/repo"
