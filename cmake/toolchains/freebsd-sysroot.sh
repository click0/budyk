#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
#
# Make a FreeBSD sysroot on a Linux host, for the cross toolchain files
# next to this script:
#
#   cmake/toolchains/freebsd-sysroot.sh VERSION ARCH DIR
#   cmake -B build-freebsd --toolchain cmake/toolchains/freebsd-ARCH.cmake \
#         -DFREEBSD_SYSROOT=DIR
#
# VERSION is a FreeBSD release (14.4, 15.0, ...), ARCH amd64 or aarch64.
# DIR gets the release's headers and libraries (/usr/include, /usr/lib and
# /lib of base.txz) and those of the packages budyk links against (lua54,
# libyaml, libargon2, quarterly branch of pkg.freebsd.org).
#
# Every download is checked: base.txz against the release's MANIFEST, the
# package catalogue's signature against the pkg key in that base.txz
# (/usr/share/keys/pkg/trusted), each package against the catalogue's
# checksum. A DIR already made for the same VERSION and ARCH is left as it
# is.
#
# FREEBSD_DIST overrides the release directory (a mirror, or
# https://ftp-archive.freebsd.org/pub/FreeBSD-Archive/old-releases/... for a
# release past its end of life); FREEBSD_PKG the package branch (.../latest).
#
# Needs curl, GNU tar with xz and zstd, sha256sum, openssl and python3.

set -eu
VER=${1:?usage: freebsd-sysroot.sh VERSION ARCH DIR}
ARCH=${2:?usage: freebsd-sysroot.sh VERSION ARCH DIR}
DIR=${3:?usage: freebsd-sysroot.sh VERSION ARCH DIR}
PKGS="lua54 libyaml libargon2"

case "$ARCH" in
    amd64)   DISTARCH=amd64 ;;
    aarch64) DISTARCH=arm64/aarch64 ;;
    *) echo "freebsd-sysroot.sh: ARCH is amd64 or aarch64, not '$ARCH'" >&2; exit 2 ;;
esac
case "$VER" in
    [0-9]*.[0-9]*) MAJOR=${VER%%.*} ;;
    *) echo "freebsd-sysroot.sh: VERSION is a release such as 14.4, not '$VER'" >&2; exit 2 ;;
esac
DIST=${FREEBSD_DIST:-https://download.freebsd.org/releases/$DISTARCH/$VER-RELEASE}
PKGREPO=${FREEBSD_PKG:-https://pkg.freebsd.org/FreeBSD:$MAJOR:$ARCH/quarterly}

mkdir -p "$DIR"
DIR=$(cd "$DIR" && pwd)
if [ "$(cat "$DIR/.budyk-sysroot" 2>/dev/null)" = "$VER $ARCH" ]; then
    echo "$DIR: FreeBSD $VER $ARCH sysroot already there"
    exit 0
fi
DL=$(mktemp -d)
trap 'rm -rf "$DL"' EXIT
fetch() { curl -fsSL --retry 3 -o "$2" "$1"; }
untar() { tar --warning=no-unknown-keyword --no-same-owner "$@"; }

# --- Base: headers and libraries --------------------------------------------
echo "base.txz of FreeBSD $VER $ARCH"
fetch "$DIST/MANIFEST" "$DL/MANIFEST"
fetch "$DIST/base.txz" "$DL/base.txz"
want=$(awk '$1 == "base.txz" { print $2 }' "$DL/MANIFEST")
have=$(sha256sum "$DL/base.txz" | cut -c1-64)
if [ -z "$want" ] || [ "$want" != "$have" ]; then
    echo "freebsd-sysroot.sh: base.txz has sha256 $have, its MANIFEST says '$want'" >&2
    exit 1
fi
untar -xJf "$DL/base.txz" -C "$DIR" --exclude=./usr/lib/debug --exclude=./usr/lib/clang \
    ./usr/include ./usr/lib ./lib ./usr/share/keys/pkg

# --- Packages: the catalogue, its signature, the packages -------------------
echo "packages from $PKGREPO: $PKGS"
fetch "$PKGREPO/packagesite.pkg" "$DL/packagesite.pkg"
mkdir "$DL/site"
untar --zstd -xf "$DL/packagesite.pkg" -C "$DL/site" \
    packagesite.yaml packagesite.yaml.pub packagesite.yaml.sig
# The key the catalogue comes with must be one base trusts (its sha256 is
# a fingerprint in /usr/share/keys/pkg/trusted), and the signature is over
# the catalogue's sha256 in hex.
key=$(sha256sum "$DL/site/packagesite.yaml.pub" | cut -c1-64)
if ! grep -qs "^fingerprint: \"$key\"" "$DIR"/usr/share/keys/pkg/trusted/*; then
    echo "freebsd-sysroot.sh: the catalogue's key ($key) is not one FreeBSD $VER trusts" >&2
    exit 1
fi
printf %s "$(sha256sum "$DL/site/packagesite.yaml" | cut -c1-64)" |
    openssl dgst -sha256 -verify "$DL/site/packagesite.yaml.pub" \
        -signature "$DL/site/packagesite.yaml.sig" >/dev/null || {
    echo "freebsd-sysroot.sh: the package catalogue's signature does not verify" >&2
    exit 1
}
# One JSON object per line; "sum" is pkg's "2$" form: the blake2b-512 of
# the file in pkg's base32 (least significant bits first).
python3 -I - "$DL/site/packagesite.yaml" $PKGS > "$DL/wanted" <<'EOF'
import json, sys
names = sys.argv[2:]
found = {}
with open(sys.argv[1], encoding="utf-8") as f:
    for line in f:
        d = json.loads(line)
        if d.get("name") in names:
            found[d["name"]] = d
for n in names:
    if n not in found:
        sys.exit("freebsd-sysroot.sh: no package %s in the catalogue" % n)
    print(n, found[n]["version"], found[n]["path"], found[n]["sum"])
EOF
while read -r name version path sum; do
    echo "  $name $version"
    fetch "$PKGREPO/$path" "$DL/$name.pkg"
    python3 -I - "$DL/$name.pkg" "$sum" <<'EOF'
import hashlib, sys
b32 = "ybndrfg8ejkmcpqxot1uwisza345h769"
h = hashlib.blake2b(open(sys.argv[1], "rb").read()).digest()
n = int.from_bytes(h, "little")
got = "2$" + "".join(b32[(n >> (5 * i)) & 31] for i in range((len(h) * 8 + 4) // 5))
if got != sys.argv[2]:
    sys.exit("freebsd-sysroot.sh: %s has checksum %s, the catalogue says %s"
             % (sys.argv[1], got, sys.argv[2]))
EOF
    mkdir "$DL/$name"
    # Its paths are absolute: tar says it strips the leading '/', say the rest.
    untar --zstd -xf "$DL/$name.pkg" -C "$DL/$name" 2>"$DL/tar.err" || {
        cat "$DL/tar.err" >&2; exit 1; }
    mkdir -p "$DIR/usr/local"
    cp -R "$DL/$name/usr/local/include" "$DL/$name/usr/local/lib" "$DIR/usr/local/"
done < "$DL/wanted"

echo "$VER $ARCH" > "$DIR/.budyk-sysroot"
echo "$DIR: FreeBSD $VER $ARCH sysroot"
