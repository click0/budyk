#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
#
# The FreeBSD port in addons/freebsd/, end to end, on a FreeBSD host (CI
# runs it on 14.4 and 15.1): built by the ports framework from this tree,
# its plist and the ports QA checked, the package installed, then the
# service started by its rc.d script. budyk must run as budyk, answer on
# 127.0.0.1:8080, log to syslog, have loaded the rules the package put
# in place, take budyk_flags as its own options, stop on `service budyk
# stop`, and the package must remove cleanly.
#
# Needs root or sudo without a password, git, curl and the build
# dependencies: pkg install cmake ninja pkgconf lua54 libyaml libargon2.
# It fetches the framework (Mk/, Keywords/, Templates/ of the ports tree)
# into WORK. It installs, starts and removes the package: run it on a
# machine for testing.
#
#   addons/freebsd/test-port.sh [WORK]   (default: a new temporary directory)

set -eu
SRC=$(cd "$(dirname "$0")/../.." && pwd)
WORK=${1:-$(mktemp -d)}
SUDO=
[ "$(id -u)" -eq 0 ] || SUDO=sudo
PORTS=$WORK/ports
PORT=$PORTS/sysutils/budyk
CONF=/usr/local/etc/budyk

pmake() {
    $SUDO env PORTSDIR="$PORTS" DISTDIR="$WORK/distfiles" WRKDIRPREFIX="$WORK/wrk" \
        make -C "$PORT" "$@"
}
fail() { echo "FAIL: $*"; tail -20 /var/log/messages | grep budyk || true; exit 1; }

echo "== the ports framework"
if [ ! -d "$PORTS/Mk" ]; then
    git clone -q --depth 1 --filter=blob:none --sparse \
        https://github.com/freebsd/freebsd-ports.git "$PORTS"
    git -C "$PORTS" sparse-checkout set Mk Keywords Templates
fi
# budyk is not in the tree's UIDs and GIDs (a port submitted to it adds
# itself there): this copy gets it, under an id no one has.
if ! grep -q '^budyk:' "$PORTS/UIDs"; then
    id=871
    while grep -q "^[^:]*:[^:]*:$id:" "$PORTS/UIDs" "$PORTS/GIDs"; do id=$((id + 1)); done
    echo "budyk:*:$id:$id::0:0:budyk monitoring daemon:/nonexistent:/usr/sbin/nologin" >> "$PORTS/UIDs"
    echo "budyk:*:$id:" >> "$PORTS/GIDs"
fi
rm -rf "$PORT"
mkdir -p "$PORT"
cp -R "$SRC/addons/freebsd/." "$PORT/"
rm -f "$PORT/test-port.sh"

echo "== the distfile: this tree, as GitHub would serve the tag"
DF=$(pmake -V DISTFILES | sed 's/:.*//')
WS=$(basename "$(pmake -V WRKSRC)")
mkdir -p "$WORK/distfiles"
tar -C "$SRC" -czf "$WORK/distfiles/$DF" -s ",^\./,$WS/," \
    --exclude './build*' --exclude ./.git --exclude ./budyk-cross .
echo "$DF ($WS/)"

echo "== build, stage, QA, plist, package"
pmake makesum
pmake stage
pmake stage-qa
pmake check-plist
pmake package
PKG=$(ls "$(pmake -V WRKDIR)"/pkg/budyk-*.pkg)
echo "$PKG"

echo "== install"
$SUDO pkg delete -y budyk > /dev/null 2>&1 || true
$SUDO pkg add "$PKG"
id budyk
[ -f "$CONF/config.yaml" ] || fail "no $CONF/config.yaml from its sample"
[ -f "$CONF/rules.lua" ]   || fail "no $CONF/rules.lua from its sample"
[ "$(stat -f %Su:%Sg /var/db/budyk)" = budyk:budyk ] || fail "/var/db/budyk is not budyk's"
[ -f /usr/local/share/man/man8/budyk.8.gz ] || fail "no man page"
pkg info -D budyk | grep -q "$CONF/config.yaml" && ! pkg info -D budyk | grep -q "%%" ||
    fail "the pkg-message: $(pkg info -D budyk)"
/usr/local/bin/budyk version

echo "== the service"
started() {
    i=0
    until curl -s -o /dev/null http://127.0.0.1:8080/api/health; do
        i=$((i + 1))
        [ "$i" -lt 50 ] || fail "budyk did not answer on 127.0.0.1:8080"
        sleep 0.2
    done
}
$SUDO service budyk onestart
started
$SUDO service budyk onestatus
PID=$($SUDO cat /var/run/budyk/budyk.pid)   # 0600, budyk's
[ "$(ps -o user= -p "$PID" | tr -d ' ')" = budyk ] || fail "budyk does not run as budyk"
sleep 1
grep -q "budyk\[[0-9]*\]: budyk serve: started (config=$CONF/config.yaml" /var/log/messages ||
    fail "no 'started' line in /var/log/messages"
grep "budyk serve: started" /var/log/messages | tail -1 | grep -q "rules=[1-9]" ||
    fail "the rules in $CONF/rules.lua were not loaded"
echo "ok   runs as budyk, answers, logs to syslog, rules loaded"

$SUDO service budyk onestop
! $SUDO service budyk onestatus > /dev/null 2>&1 || fail "still running after stop"
! curl -s -o /dev/null http://127.0.0.1:8080/api/health || fail "still answering after stop"
echo "ok   stops"

# budyk_flags as README had it: rc.subr gives it to daemon(8), which
# refused it; the script takes it as budyk's own.
$SUDO sysrc budyk_flags="--enable-exec" > /dev/null
$SUDO service budyk onestart
started
ps -o args= -p "$($SUDO cat /var/run/budyk/budyk.pid)" | grep -q -- "--enable-exec" ||
    fail "budyk_flags did not reach budyk"
$SUDO service budyk onestop
$SUDO sysrc -x budyk_flags
echo "ok   budyk_flags are budyk's options"

echo "== remove"
$SUDO pkg delete -y budyk
[ ! -e /usr/local/bin/budyk ] || fail "the binary is still there"
echo "test-port: PASS"
