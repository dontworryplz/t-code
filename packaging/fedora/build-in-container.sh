# Runs inside the Fedora container (packaging/build.sh fedora on a machine without rpmbuild):
# builds the RPM from /out/t-code-$VERSION.tar.gz into /out.
set -euo pipefail

dnf -y install rpm-build dnf-plugins-core
dnf -y builddep /src/packaging/fedora/t-code.spec
top=/tmp/rpmbuild
mkdir -p "$top/SOURCES"
cp "/out/t-code-$VERSION.tar.gz" "$top/SOURCES/"
rpmbuild -ba --define "_topdir $top" /src/packaging/fedora/t-code.spec
cp "$top"/RPMS/*/*.rpm "$top"/SRPMS/*.rpm /out/
[[ -z "${HOST_OWNER:-}" ]] || chown -R "$HOST_OWNER" /out
