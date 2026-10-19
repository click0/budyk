#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
#
# Build the Linux packages of budyk from a built binary: a .deb (with
# dpkg-deb) and an .rpm (with rpmbuild), both of the same tree:
#
#   /usr/bin/budyk
#   /etc/budyk/config.yaml        the example config, Linux paths   (conffile)
#   /etc/budyk/rules.lua          rules/examples.lua                (conffile)
#   /usr/lib/systemd/system/budyk.service
#   /usr/share/man/man8/budyk.8.gz
#   /usr/share/budyk/rules/       the example rule files
#   /usr/share/doc/budyk/         README, CHANGELOG, copyright
#
# Installing creates the system user and group `budyk` and the data
# directory /var/lib/budyk (0750); the config is root:budyk 0640 (it may
# hold a password hash). The service is not started: `systemctl enable
# --now budyk` does that. Removing stops it; purging removes the data.
#
#   addons/linux/mkpkg.sh path/to/budyk VERSION OUTDIR [ARCH]
#
# ARCH is the Debian name (amd64, arm64); the .rpm gets the RPM one.
# MAINTAINER (environment) goes into the .deb's Maintainer field.

set -eu
BIN=${1:?usage: mkpkg.sh path/to/budyk VERSION OUTDIR [ARCH]}
VER=${2:?usage: mkpkg.sh path/to/budyk VERSION OUTDIR [ARCH]}
OUT=${3:?usage: mkpkg.sh path/to/budyk VERSION OUTDIR [ARCH]}
ARCH=${4:-amd64}
# Who the .deb names as its maintainer ("Name <address>").
MAINTAINER=${MAINTAINER:-budyk <https://github.com/click0/budyk/issues>}
case "$ARCH" in
    amd64) RPMARCH=x86_64 ;;
    arm64) RPMARCH=aarch64 ;;
    *)     RPMARCH=$ARCH ;;
esac
SRC=$(cd "$(dirname "$0")/../.." && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)

SUMMARY="Lightweight server monitoring with adaptive collection"
DESC="budyk is a self-contained monitoring daemon: one static binary, ring
files instead of a database, a web dashboard, a terminal UI and Lua rules
with alerts. It collects at a 5-minute heartbeat while nobody looks or
anything is wrong, and at 1 Hz when a dashboard is open or a threshold
holds."

# --- The tree ---------------------------------------------------------------
ROOT=$WORK/root
install -d "$ROOT/usr/bin" "$ROOT/etc/budyk" "$ROOT/usr/lib/systemd/system" \
           "$ROOT/usr/share/man/man8" "$ROOT/usr/share/budyk/rules" \
           "$ROOT/usr/share/doc/budyk"
install -m 0755 "$BIN" "$ROOT/usr/bin/budyk"
sed -e 's|/var/db/budyk|/var/lib/budyk|g' \
    -e 's|/usr/local/etc/budyk/rules.lua|/etc/budyk/rules.lua|' \
    "$SRC/config.example.yaml" > "$ROOT/etc/budyk/config.yaml"
grep -q '^data_dir: "/var/lib/budyk"' "$ROOT/etc/budyk/config.yaml"
install -m 0644 "$SRC/rules/examples.lua" "$ROOT/etc/budyk/rules.lua"
sed -e 's|/usr/local/bin/budyk|/usr/bin/budyk|g' -e 's|/var/db/budyk|/var/lib/budyk|g' \
    "$SRC/addons/linux/budyk.service" > "$ROOT/usr/lib/systemd/system/budyk.service"
gzip -9n -c "$SRC/docs/budyk.8" > "$ROOT/usr/share/man/man8/budyk.8.gz"
install -m 0644 "$SRC"/rules/*.lua "$ROOT/usr/share/budyk/rules/"
install -m 0644 "$SRC/README.md" "$ROOT/usr/share/doc/budyk/"
gzip -9n -c "$SRC/CHANGELOG.md" > "$ROOT/usr/share/doc/budyk/changelog.gz"
{
    echo "Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/"
    echo "Upstream-Name: budyk"
    echo "Source: https://github.com/click0/budyk"
    echo
    echo "Files: *"
    echo "License: BSD-3-Clause"
    echo
    sed 's/^/ /; s/^ $/ ./' "$SRC/LICENSE"
} > "$ROOT/usr/share/doc/budyk/copyright"
chmod 0644 "$ROOT/etc/budyk/config.yaml" "$ROOT/usr/lib/systemd/system/budyk.service" \
           "$ROOT/usr/share/man/man8/budyk.8.gz" "$ROOT/usr/share/doc/budyk/copyright" \
           "$ROOT/usr/share/doc/budyk/changelog.gz"

# What installing and removing do, shared by both formats.
add_user='getent group budyk >/dev/null || groupadd --system budyk
getent passwd budyk >/dev/null || useradd --system --gid budyk --home-dir /var/lib/budyk --no-create-home --shell /usr/sbin/nologin --comment "budyk monitoring daemon" budyk'
setup='install -d -o budyk -g budyk -m 0750 /var/lib/budyk
chown root:budyk /etc/budyk/config.yaml && chmod 0640 /etc/budyk/config.yaml
if [ -d /run/systemd/system ]; then systemctl daemon-reload >/dev/null 2>&1 || true; fi'
stop='if [ -d /run/systemd/system ]; then systemctl stop budyk.service >/dev/null 2>&1 || true; systemctl disable budyk.service >/dev/null 2>&1 || true; fi'
reload='if [ -d /run/systemd/system ]; then systemctl daemon-reload >/dev/null 2>&1 || true; fi'

# --- .deb -------------------------------------------------------------------
DEB=$WORK/deb
cp -a "$ROOT" "$DEB"
install -d "$DEB/DEBIAN"
SIZE=$(du -sk "$ROOT" | cut -f1)
{
    echo "Package: budyk"
    echo "Version: $VER"
    echo "Architecture: $ARCH"
    echo "Maintainer: $MAINTAINER"
    echo "Installed-Size: $SIZE"
    echo "Depends: adduser | passwd"
    echo "Section: admin"
    echo "Priority: optional"
    echo "Homepage: https://github.com/click0/budyk"
    echo "Description: $SUMMARY"
    echo "$DESC" | sed 's/^/ /'
} > "$DEB/DEBIAN/control"
printf '/etc/budyk/config.yaml\n/etc/budyk/rules.lua\n' > "$DEB/DEBIAN/conffiles"
printf '#!/bin/sh\nset -e\nif [ "$1" = configure ]; then\n%s\n%s\nfi\n' "$add_user" "$setup" > "$DEB/DEBIAN/postinst"
printf '#!/bin/sh\nset -e\nif [ "$1" = remove ] || [ "$1" = deconfigure ]; then\n%s\nfi\n' "$stop" > "$DEB/DEBIAN/prerm"
printf '#!/bin/sh\nset -e\n%s\nif [ "$1" = purge ]; then\n    rm -rf /var/lib/budyk\nfi\n' "$reload" > "$DEB/DEBIAN/postrm"
chmod 0755 "$DEB/DEBIAN/postinst" "$DEB/DEBIAN/prerm" "$DEB/DEBIAN/postrm"
dpkg-deb --root-owner-group --build "$DEB" "$OUT/budyk_${VER}_${ARCH}.deb" >/dev/null

# --- .rpm -------------------------------------------------------------------
RPMTOP=$WORK/rpm
install -d "$RPMTOP/SPECS" "$RPMTOP/BUILD" "$RPMTOP/RPMS"
{
    echo "Name:      budyk"
    echo "Version:   $(echo "$VER" | tr - _)"
    echo "Release:   1"
    echo "Summary:   $SUMMARY"
    echo "License:   BSD-3-Clause"
    echo "URL:       https://github.com/click0/budyk"
    echo "BuildArch: $RPMARCH"
    echo "Requires(pre): shadow-utils"
    echo "%global debug_package %{nil}"
    echo "%global __os_install_post %{nil}"
    echo
    echo "%description"
    echo "$DESC"
    echo
    echo "%install"
    echo "cp -a $ROOT/. %{buildroot}/"
    echo
    echo "%pre"
    echo "$add_user"
    echo
    echo "%post"
    echo "$setup"
    echo
    echo "%preun"
    echo 'if [ "$1" -eq 0 ]; then'
    echo "$stop"
    echo "fi"
    echo
    echo "%postun"
    echo "$reload"
    echo
    echo "%files"
    echo "%attr(0755,root,root) /usr/bin/budyk"
    echo "%dir /etc/budyk"
    echo "%config(noreplace) /etc/budyk/config.yaml"
    echo "%config(noreplace) /etc/budyk/rules.lua"
    echo "/usr/lib/systemd/system/budyk.service"
    echo "/usr/share/man/man8/budyk.8.gz"
    echo "/usr/share/budyk"
    echo "%doc /usr/share/doc/budyk"
} > "$RPMTOP/SPECS/budyk.spec"
rpmbuild --quiet --define "_topdir $RPMTOP" --target "$RPMARCH" \
         -bb "$RPMTOP/SPECS/budyk.spec" >/dev/null
cp "$RPMTOP"/RPMS/*/budyk-*.rpm "$OUT/"

ls "$OUT"/budyk_"${VER}"_"${ARCH}".deb "$OUT"/budyk-*.rpm
