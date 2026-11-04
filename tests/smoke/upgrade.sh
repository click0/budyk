#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
#
# Rings written by an older budyk (smaller records, an older codec
# version) are converted when this one starts: an older release
# collects for a few seconds, then this budyk starts on the same
# data_dir and must log the conversion of every ring, serve every
# sample the old one had stored (same timestamps), and leave rings
# that fsck finds clean. An older budyk of the same record format (the
# release before this one, when the codec did not change) has nothing
# to convert, and nothing must be: its rings are used as they are.
#
#   tests/smoke/upgrade.sh path/to/budyk path/to/older/budyk [port]
#
# CI passes the v0.6.4 and v0.7.0 Linux release binaries as the older
# one, and the build itself for the same format.

set -eu
BIN=${1:?usage: upgrade.sh path/to/budyk path/to/older/budyk [port]}
OLD=${2:?usage: upgrade.sh path/to/budyk path/to/older/budyk [port]}
PORT=${3:-18777}
DIR=$(mktemp -d)
PID=
trap 'kill "$PID" 2>/dev/null || true; rm -rf "$DIR"' EXIT

cat > "$DIR/config.yaml" <<EOF
data_dir: $DIR/data
listen: 127.0.0.1
port: $PORT
collection:
  l1: { interval: 1 }
  levels:
    - { name: burst, interval: 0.2, priority: 40, when: "load.avg_1m >= 0", storage_mb: 1 }
EOF
mkdir -p "$DIR/data"

run() {   # run <binary>: start it on the config and wait for HTTP
    "$1" serve --config "$DIR/config.yaml" >> "$DIR/log" 2>&1 &
    PID=$!
    i=0
    until curl -s -o /dev/null "http://127.0.0.1:$PORT/api/health"; do
        i=$((i + 1))
        [ "$i" -lt 50 ] || { echo "daemon did not start"; cat "$DIR/log"; exit 1; }
        sleep 0.1
    done
}
stop() {
    kill -TERM "$PID"
    wait "$PID" || { echo "FAIL: daemon exited non-zero"; fail=1; }
    PID=
}
timestamps() {   # timestamps <file>: the "ts" of every sample in an /api/range answer
    python3 -c 'import json,sys; print("\n".join(str(s["ts"]) for s in json.load(open(sys.argv[1]))["samples"]))' "$1"
}
record_size() {   # record_size <ring>: the record size in the ring's header
    python3 -c 'import struct,sys; f=open(sys.argv[1],"rb"); f.seek(16); print(struct.unpack("<I", f.read(4))[0])' "$1"
}
fail=0

run "$OLD"
echo "older budyk: $("$OLD" version)"
sleep 2.5
stop
# What it stored, read back through the old binary's own API.
run "$OLD"
curl -s "http://127.0.0.1:$PORT/api/range?level=burst&limit=5000" > "$DIR/before.json"
stop
before=$(timestamps "$DIR/before.json" | wc -l)
old_size=$(record_size "$DIR/data/level-burst.ring")
echo "     $before burst samples in $old_size-byte records"

: > "$DIR/log"
run "$BIN"
new_size=$(record_size "$DIR/data/level-burst.ring")
if [ "$new_size" -eq "$old_size" ]; then
    # The same record format: the rings are used as they are, and a
    # conversion would copy every ring for nothing on every upgrade.
    if grep -q ": converted from " "$DIR/log"; then
        echo "FAIL: a ring of $old_size-byte records, this budyk's own, was converted:"
        grep ": converted from " "$DIR/log"; fail=1
    else
        echo "ok   $old_size-byte records are this budyk's own: used as they are"
    fi
else
    for ring in tier1.ring tier2.ring tier3.ring level-burst.ring; do
        if grep -q "$ring: converted from $old_size-byte records (an older budyk)" "$DIR/log"; then
            echo "ok   $ring converted on start"
        else
            echo "FAIL: no conversion logged for $ring"; fail=1
        fi
    done
fi
curl -s "http://127.0.0.1:$PORT/api/range?level=burst&limit=5000" > "$DIR/after.json"
stop
timestamps "$DIR/before.json" | sort > "$DIR/before.ts"
timestamps "$DIR/after.json"  | sort > "$DIR/after.ts"
missing=$(comm -23 "$DIR/before.ts" "$DIR/after.ts" | wc -l)
if [ "$before" -gt 0 ] && [ "$missing" -eq 0 ]; then
    echo "ok   all $before old samples are served by the new budyk"
else
    echo "FAIL: $missing of $before old samples missing under the new budyk"; fail=1
fi
if "$BIN" fsck "$DIR/data" > "$DIR/fsck.out" 2>&1; then
    echo "ok   fsck: clean"
else
    echo "FAIL: fsck under the new budyk:"; cat "$DIR/fsck.out"; fail=1
fi
if ls "$DIR/data"/*.convert > /dev/null 2>&1; then
    echo "FAIL: a .convert file was left behind"; fail=1
fi

if grep -qE 'ERROR: (Address|Leak)Sanitizer|ThreadSanitizer|runtime error:' "$DIR/log"; then
    echo "FAIL: sanitizer report"; fail=1
fi
if [ "$fail" -ne 0 ]; then
    echo "--- daemon log ---"; cat "$DIR/log"
    exit 1
fi
echo "upgrade smoke test: PASS"
