# Runs inside the Arch Linux container (packaging/build.sh arch): builds the package from
# /out/t-code-$VERSION.tar.gz into /out. makepkg refuses to run as root, so a user builds it.
set -euo pipefail

pacman -Syu --noconfirm --needed base-devel
# shellcheck source=/dev/null
source /src/packaging/arch/PKGBUILD
pacman -S --noconfirm --needed "${depends[@]}" "${makedepends[@]}"

useradd -m builder
work=/home/builder/t-code
mkdir -p "$work"
cp /src/packaging/arch/PKGBUILD "/out/t-code-$VERSION.tar.gz" "$work/"
sed -i "s/^sha256sums=.*/sha256sums=('$(sha256sum "$work/t-code-$VERSION.tar.gz" | cut -d' ' -f1)')/" \
  "$work/PKGBUILD"
chown -R builder: "$work"
su builder -c "cd $work && makepkg --noconfirm --cleanbuild"
cp "$work"/*.pkg.tar.* /out/
[[ -z "${HOST_OWNER:-}" ]] || chown -R "$HOST_OWNER" /out
