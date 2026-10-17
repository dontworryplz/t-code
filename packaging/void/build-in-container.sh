# Runs inside a Void Linux container (packaging/build.sh void / void-musl): builds t-code from
# /out/t-code-$VERSION.tar.gz, runs the tests that need no display, installs it into a staging
# folder and packs that with xbps-create into /out/t-code-$VERSION_<revision>.<arch>.xbps. The
# same script makes the glibc and the musl package; the container decides which.
# Not an xbps-src template: xbps-src builds in its own chroot, which a rootless container does
# not allow. The runtime dependencies are therefore listed here by hand.
set -euo pipefail

revision=1
arch=$(xbps-uhelper arch)

xbps-install -Syu xbps >/dev/null
xbps-install -Syu >/dev/null
xbps-install -y gcc make pkg-config gtk4-devel vte3-gtk4-devel libwebkitgtk60-devel \
  libmd4c-devel desktop-file-utils >/dev/null

work=/tmp/build
dest=/tmp/dest
mkdir -p "$work" "$dest"
tar -C "$work" -xzf "/out/t-code-$VERSION.tar.gz"
cd "$work/t-code-$VERSION"

# Void's own default flags for packages.
export CFLAGS="-O2 -pipe -fstack-clash-protection -D_FORTIFY_SOURCE=2 -fstack-protector-strong"
export LDFLAGS="-Wl,--as-needed -Wl,-z,relro -Wl,-z,now"
make
make build/test-searchmatch build/test-lsp
build/test-searchmatch
build/test-lsp
test "$(build/t-code --version)" = "t-code $VERSION"

make install DESTDIR="$dest" PREFIX=/usr LIBEXECDIR=/usr/libexec
install -Dm644 LICENSE "$dest/usr/share/licenses/t-code/LICENSE"
desktop-file-validate "$dest/usr/share/applications/io.github.tcode.desktop"
strip --strip-unneeded "$dest/usr/bin/t-code" "$dest/usr/libexec/t-code/"*

# Cascadia Code is not in Void's repositories: without it the terminals use the default
# monospace font (and any font can be picked in Settings).
cd /out
xbps-create -A "$arch" -n "t-code-${VERSION}_$revision" \
  -s "Terminal-centric C/C++ IDE: file manager, micro editor tabs and a shell" \
  -H "https://github.com/suacayipkisi/t-code" -l MIT \
  -m "suacayipkisi <ufukdenizcaglayan@gmail.com>" \
  -D "glib>=0 gtk4>=0 vte3-gtk4>=0 libwebkitgtk60>=0 libmd4c>=0 libmd4c-html>=0 micro>=2.0.14_1 clang-tools-extra>=0 git>=0 hicolor-icon-theme>=0" \
  "$dest"
[[ -z "${HOST_OWNER:-}" ]] || chown -R "$HOST_OWNER" /out
