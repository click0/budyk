#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
#
# Install, run and remove the packages mkpkg.sh built (as root):
#   .deb  on this host (dpkg): the binary and its version, the budyk
#         user, the data directory and the config's owner and mode, the
#         daemon running as budyk with the packaged config and rules,
#         fsck finding that config with no --config, the man page's
#         paths; under systemd (when it is PID 1) the service, and an
#         upgrade restarting it; then remove (the config stays) and purge
#         (config and data gone);
#   .rpm  in a Fedora container when docker works: the same install and
#         run checks, then rpm -e; without docker, its file list and
#         scripts only.
#
#   addons/linux/test-packages.sh OUTDIR VERSION
#
# VERSION is the packages' (as in their file names); the binary may say
# less of it (a dry run's 0.8.0-1a2b3c4d holds a budyk 0.8.0).

set -eu
OUT=${1:?usage: test-packages.sh OUTDIR VERSION}
VER=${2:?usage: test-packages.sh OUTDIR VERSION}
OUT=$(cd "$OUT" && pwd)
DEB=$(ls "$OUT"/budyk_"$VER"_*.deb)
RPM=$(ls "$OUT"/budyk-*.rpm)
fail=0
trap 'rm -f "$OUT/check-installed.sh"' EXIT
ok()  { echo "ok   $*"; }
bad() { echo "FAIL: $*"; fail=1; }

# The checks after installing, the same for both formats (run inside the
# container for the .rpm). Starts the daemon on port $1.
cat > "$OUT/check-installed.sh" <<'EOF'
#!/bin/sh
set -u
PORT=$1 PKGVER=$2
fail=0
VER=$(/usr/bin/budyk version | cut -d' ' -f2)
case "$PKGVER" in
    "$VER"|"$VER"-*|"$VER"_*) echo "ok   budyk version $VER" ;;
    *) echo "FAIL: budyk version $VER in a package of $PKGVER"; fail=1 ;;
esac
id budyk >/dev/null 2>&1 && echo "ok   the budyk user" || { echo "FAIL: no budyk user"; fail=1; }
[ "$(stat -c '%U:%G %a' /var/lib/budyk)" = "budyk:budyk 750" ] && echo "ok   /var/lib/budyk budyk:budyk 0750" ||
    { echo "FAIL: /var/lib/budyk is $(stat -c '%U:%G %a' /var/lib/budyk)"; fail=1; }
[ "$(stat -c '%U:%G %a' /etc/budyk/config.yaml)" = "root:budyk 640" ] && echo "ok   the config root:budyk 0640" ||
    { echo "FAIL: config is $(stat -c '%U:%G %a' /etc/budyk/config.yaml)"; fail=1; }
grep -q '^data_dir: "/var/lib/budyk"' /etc/budyk/config.yaml && grep -q 'path: "/etc/budyk/rules.lua"' /etc/budyk/config.yaml &&
    echo "ok   the config has the Linux paths" || { echo "FAIL: config paths"; fail=1; }
grep -q '^ExecStart=/usr/bin/budyk serve --config /etc/budyk/config.yaml' /usr/lib/systemd/system/budyk.service &&
grep -q '^ReadWritePaths=/var/lib/budyk' /usr/lib/systemd/system/budyk.service &&
    echo "ok   the unit runs /usr/bin/budyk on /var/lib/budyk" || { echo "FAIL: unit paths"; fail=1; }
# The daemon as budyk, the packaged config, another port.
sed "s/^port: 8080/port: $PORT/" /etc/budyk/config.yaml > /tmp/budyk-test.yaml
chmod 0644 /tmp/budyk-test.yaml
runuser -u budyk -- /usr/bin/budyk serve --config /tmp/budyk-test.yaml > /tmp/budyk-test.log 2>&1 &
DPID=$!
i=0
until curl -s -o /dev/null "http://127.0.0.1:$PORT/api/health"; do
    i=$((i + 1)); [ "$i" -lt 50 ] || break; sleep 0.1
done
if curl -s "http://127.0.0.1:$PORT/api/health" | grep -q "\"version\":\"$VER\""; then
    echo "ok   the daemon serves as budyk"
else
    echo "FAIL: the daemon did not start: $(cat /tmp/budyk-test.log)"; fail=1
fi
[ "$(stat -c %U /var/lib/budyk/tier1.ring 2>/dev/null)" = budyk ] && echo "ok   its rings are budyk's" ||
    { echo "FAIL: no ring owned by budyk in /var/lib/budyk"; fail=1; }
grep "budyk serve: started" /tmp/budyk-test.log | grep -q "rules=[1-9]" && echo "ok   the packaged rules loaded" ||
    { echo "FAIL: the rules did not load: $(grep rules /tmp/budyk-test.log)"; fail=1; }
kill -TERM "$DPID" 2>/dev/null; wait "$DPID" 2>/dev/null
# With no --config, the packages' /etc/budyk/config.yaml (fsck looked
# only under /usr/local and gave up).
if [ ! -e /usr/local/etc/budyk/config.yaml ]; then
    runuser -u budyk -- /usr/bin/budyk fsck > /tmp/budyk-fsck.log 2>&1 && echo "ok   budyk fsck with no --config" ||
        { echo "FAIL: budyk fsck: $(cat /tmp/budyk-fsck.log)"; fail=1; }
fi
exit $fail
EOF
chmod 0755 "$OUT/check-installed.sh"

# The man page with the packages' paths (read from the package: a
# minimal system may not install man pages).
dpkg-deb --fsys-tarfile "$DEB" | tar -xO ./usr/share/man/man8/budyk.8.gz | gzip -dc > "$OUT/budyk.8"
grep -q '^\.I /etc/budyk/config.yaml' "$OUT/budyk.8" && ! grep -q '^\.I /usr/local/etc\|^\.I /var/db/' "$OUT/budyk.8" &&
    ok "the man page has the packages' paths" || bad "the man page's paths"
rm -f "$OUT/budyk.8"

# --- .deb -----------------------------------------------------------------
dpkg -i "$DEB" >/dev/null
"$OUT/check-installed.sh" 18990 "$VER" || fail=1
if [ -d /run/systemd/system ]; then
    systemctl start budyk
    i=0
    until curl -s -o /dev/null http://127.0.0.1:8080/api/health; do
        i=$((i + 1)); [ "$i" -lt 50 ] || break; sleep 0.1
    done
    if [ "$(ps -o user= -C budyk | head -1)" = budyk ] && curl -s http://127.0.0.1:8080/api/health | grep -q ok; then
        ok "systemctl start budyk: running as budyk"
    else
        bad "the service: $(systemctl status budyk --no-pager 2>&1 | tail -5)"
    fi
    # An upgrade (here the same package again) restarts the running
    # service: the old binary used to go on running.
    before=$(systemctl show -p MainPID --value budyk)
    dpkg -i "$DEB" >/dev/null
    after=$(systemctl show -p MainPID --value budyk)
    if systemctl is-active --quiet budyk && [ "$after" != 0 ] && [ "$after" != "$before" ]; then
        ok "an upgrade restarts the service ($before -> $after)"
    else
        bad "the service after an upgrade: MainPID $before -> $after"
    fi
fi
echo "# a local change" >> /etc/budyk/config.yaml
dpkg -r budyk >/dev/null
[ ! -e /usr/bin/budyk ] && grep -q '# a local change' /etc/budyk/config.yaml && ok "dpkg -r: the binary gone, the config kept" ||
    bad "after dpkg -r"
if [ -d /run/systemd/system ] && systemctl is-active --quiet budyk; then bad "the service runs after dpkg -r"; fi
dpkg -P budyk >/dev/null
[ ! -e /etc/budyk/config.yaml ] && [ ! -e /var/lib/budyk ] && ok "dpkg -P: config and data gone" || bad "after dpkg -P"

# --- .rpm -----------------------------------------------------------------
if docker info >/dev/null 2>&1; then
    if docker run --rm -v "$OUT:/pkg:ro" fedora:41 sh -c "
        dnf -y -q install /pkg/$(basename "$RPM") procps-ng curl util-linux gzip >/dev/null &&
        /pkg/check-installed.sh 18991 $VER &&
        rpm -e budyk && [ ! -e /usr/bin/budyk ] && echo 'ok   rpm -e: the binary gone'"; then
        ok ".rpm installed, ran and removed in Fedora"
    else
        bad ".rpm in Fedora"
    fi
else
    rpm -qpl "$RPM" | grep -qx /usr/bin/budyk && rpm -qp --scripts "$RPM" | grep -q useradd &&
        ok ".rpm file list and scripts (no docker to install it)" || bad ".rpm contents"
fi

[ "$fail" -eq 0 ] && echo "package test: PASS" || { echo "package test: FAIL"; exit 1; }
