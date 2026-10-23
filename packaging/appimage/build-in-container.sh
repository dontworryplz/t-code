# Runs inside the Debian container (packaging/appimage.sh): builds t-code from
# /out/t-code-$VERSION.tar.gz, stages it under /AppDir like `make install` puts it in /usr, and
# bundles it with everything it needs (GTK4, GtkSourceView, VTE, md4c, json-glib and their
# helpers) through linuxdeploy and its GTK plugin into /out/t-code-$VERSION-x86_64.AppImage.
set -euo pipefail

: "${VERSION:?}" # comes in from packaging/lib.sh's run_in_container

export DEBIAN_FRONTEND=noninteractive
apt-get update
# shellcheck disable=SC2086
apt-get install -y --no-install-recommends gcc make pkg-config librsvg2-bin ca-certificates curl wget dpkg-dev file \
  libgtk-4-dev libgtksourceview-5-dev libjson-glib-dev libvte-2.91-gtk4-dev libmd4c-dev

# The build: what `make install` would put in /usr goes under /AppDir/usr; the AppImage's frame
# wants its icon as a PNG (the SVG stands in the menus).
work=/tmp/t-code
rm -rf "$work" && mkdir -p "$work"
tar -C "$work" --strip-components=1 -xzf "/out/t-code-$VERSION.tar.gz"
cd "$work"
make -j"$(nproc)" all
make install PREFIX=/usr DESTDIR=/AppDir
mkdir -p /AppDir/usr/share/icons/hicolor/256x256/apps
rsvg-convert /AppDir/usr/share/icons/hicolor/scalable/apps/io.github.tcode.svg \
  -w 256 -h 256 -o /AppDir/usr/share/icons/hicolor/256x256/apps/io.github.tcode.png
sed -i 's|Exec=/usr/bin/t-code|Exec=t-code|' /AppDir/usr/share/applications/io.github.tcode.desktop

# linuxdeploy and its GTK plugin (the plugin ships as the plain shell script inside its repo, and
# the AppImage runs without FUSE in this container through APPIMAGE_EXTRACT_AND_RUN).
tools=/tmp/tools
mkdir -p "$tools"
curl -fsSL https://github.com/linuxdeploy/linuxdeploy/releases/latest/download/linuxdeploy-x86_64.AppImage \
  -o "$tools/linuxdeploy"
curl -fsSL https://raw.githubusercontent.com/linuxdeploy/linuxdeploy-plugin-gtk/master/linuxdeploy-plugin-gtk.sh \
  -o "$tools/linuxdeploy-plugin-gtk"
chmod +x "$tools"/linuxdeploy*
export APPIMAGE_EXTRACT_AND_RUN=1
export OUTPUT="t-code-$VERSION-x86_64.AppImage"
cd /tmp
"$tools/linuxdeploy" \
  --appdir /AppDir \
  --executable /AppDir/usr/bin/t-code \
  --desktop-file /AppDir/usr/share/applications/io.github.tcode.desktop \
  --icon-file /AppDir/usr/share/icons/hicolor/256x256/apps/io.github.tcode.png \
  --plugin gtk \
  --output appimage
mv "$OUTPUT" /out/

[[ -z "${HOST_OWNER:-}" ]] || chown -R "$HOST_OWNER" /out
