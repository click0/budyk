#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
#
# FreeBSD: the swap total counts every swap device. kvm_getswapinfo()
# fills one entry per device, as many as the array holds less one, and
# the total of all of them in the entry after: summing the entries
# missed every device past the 15th. This adds 17 swap devices (malloc
# md(4) disks of 16 MB; needs root or sudo without a password), checks
# budyk's swap.total against swapinfo(8)'s, and removes them. Anywhere
# else it says so and passes.
#
#   tests/smoke/swap_devices.sh path/to/budyk [port]

set -eu
BIN=${1:?usage: swap_devices.sh path/to/budyk [port]}
PORT=${2:-18771}

if [ "$(uname -s)" != "FreeBSD" ]; then
    echo "swap_devices smoke test: skipped (not FreeBSD)"
    exit 0
fi
SUDO=
if [ "$(id -u)" -ne 0 ]; then
    if command -v sudo > /dev/null && sudo -n true 2> /dev/null; then
        SUDO=sudo
    else
        echo "swap_devices smoke test: skipped (needs root or sudo)"
        exit 0
    fi
fi

DIR=$(mktemp -d)
PID=
MDS=
cleanup() {
    kill "$PID" 2> /dev/null || true
    for md in $MDS; do
        $SUDO swapoff "/dev/$md" 2> /dev/null || true
        $SUDO mdconfig -d -u "$md" 2> /dev/null || true
    done
    rm -rf "$DIR"
}
trap cleanup EXIT

i=0
while [ "$i" -lt 17 ]; do
    md=$($SUDO mdconfig -a -t malloc -s 16m)
    MDS="$MDS $md"
    $SUDO swapon "/dev/$md"
    i=$((i + 1))
done
devices=$(swapinfo -k | awk 'NR > 1 && $1 != "Total"' | wc -l | tr -d ' ')
want=$(swapinfo -k | awk 'NR > 1 && $1 != "Total" { kb += $2 } END { print kb * 1024 }')
echo "     $devices swap devices, $want bytes in swapinfo -k"

cat > "$DIR/config.yaml" <<EOF
data_dir: $DIR/data
listen: 127.0.0.1
port: $PORT
EOF
"$BIN" serve --config "$DIR/config.yaml" > "$DIR/log" 2>&1 &
PID=$!
i=0
until curl -s -o /dev/null "http://127.0.0.1:$PORT/api/health"; do
    i=$((i + 1))
    [ "$i" -lt 50 ] || { echo "daemon did not start"; cat "$DIR/log"; exit 1; }
    sleep 0.1
done

got=
i=0
while [ -z "$got" ]; do
    got=$(curl -s "http://127.0.0.1:$PORT/api/samples" | python3 -c '
import json, sys
s = json.load(sys.stdin)["samples"]
if s and "swap" in s[-1]["groups"]:
    print(s[-1]["swap"]["total"])')
    i=$((i + 1))
    [ -n "$got" ] || [ "$i" -lt 50 ] || { echo "FAIL: no sample with swap"; cat "$DIR/log"; exit 1; }
    [ -n "$got" ] || sleep 0.2
done

# Both read the same counters: only a device added or removed in between
# could move them apart. A MB of slack, a device is 16.
if [ "$devices" -gt 16 ] && python3 -c 'import sys; sys.exit(abs(int(sys.argv[1]) - int(sys.argv[2])) > (1 << 20))' "$got" "$want"; then
    echo "ok   swap total over $devices devices: budyk $got, swapinfo $want"
    echo "swap_devices smoke test: PASS"
else
    echo "FAIL: swap total over $devices devices: budyk $got, swapinfo $want"
    exit 1
fi
