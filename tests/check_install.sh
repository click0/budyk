#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
#
# `cmake --install` of a built tree: where the binary, the example config,
# the rules and the man page land, with the default prefix (/usr/local:
# the config in /usr/local/etc/budyk) and with a prefix of /usr (the
# config in /etc/budyk; it went to /usr/etc/budyk). Reconfigures the
# build directory's prefix, and puts it back to /usr/local at the end.
#
#   tests/check_install.sh path/to/build

set -eu
BUILD=${1:?usage: check_install.sh path/to/build}
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
fail=0

check() {   # check <stage> <prefix> <sysconfdir>
    for f in "$2/bin/budyk" "$3/budyk/config.example.yaml" \
             "$2/share/budyk/rules/examples.lua" "$2/share/man/man8/budyk.8"; do
        if [ -f "$1$f" ]; then
            echo "ok   $f"
        else
            echo "FAIL: no $f under the prefix $2"; fail=1
        fi
    done
}

cmake -B "$BUILD" -DCMAKE_INSTALL_PREFIX=/usr/local > /dev/null
DESTDIR="$STAGE/local" cmake --install "$BUILD" > /dev/null
check "$STAGE/local" /usr/local /usr/local/etc

cmake -B "$BUILD" -DCMAKE_INSTALL_PREFIX=/usr > /dev/null
DESTDIR="$STAGE/usr" cmake --install "$BUILD" > /dev/null
check "$STAGE/usr" /usr /etc
if [ -e "$STAGE/usr/usr/etc" ]; then
    echo "FAIL: a prefix of /usr installed into /usr/etc"; fail=1
fi
cmake -B "$BUILD" -DCMAKE_INSTALL_PREFIX=/usr/local > /dev/null

if [ "$fail" -eq 0 ]; then echo "install paths: OK"; fi
exit "$fail"
