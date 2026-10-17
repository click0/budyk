#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
#
# budyk fsck and the data_dir lock on real rings:
#
#   - a clean data_dir checks clean (exit 0), also while the daemon
#     runs (with a note); --repair is refused then (exit 8), and so is
#     a second daemon on the same data_dir;
#   - a crash between a record's pwrite and the write_idx update, made
#     by moving write_idx back one: fsck finds the record (exit 4),
#     --repair counts it in (exit 1), and a later check is clean; done
#     again, the next `budyk serve` counts it in on start and logs it;
#   - a damaged record: reported, cannot be repaired (exit 4).
#
#   tests/smoke/fsck.sh path/to/budyk [port]

set -eu
BIN=${1:?usage: fsck.sh path/to/budyk [port]}
PORT=${2:-18769}
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

start() {
    "$BIN" serve --config "$DIR/config.yaml" >> "$DIR/log" 2>&1 &
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
fail=0
expect_fsck() {   # expect_fsck <exit status> <text in the output> <args...>
    want=$1; text=$2; shift 2
    set +e
    "$BIN" fsck "$@" > "$DIR/fsck.out" 2>&1
    got=$?
    set -e
    if [ "$got" -ne "$want" ] || ! grep -qF -- "$text" "$DIR/fsck.out"; then
        echo "FAIL: budyk fsck $*: exit $got (want $want), output:"; cat "$DIR/fsck.out"; fail=1
    else
        echo "ok   budyk fsck $* -> $got ($text)"
    fi
}
# write_idx is the little-endian u64 at offset 28 of a ring's header.
widx() {   # widx <ring> [new value]: print it, or set it
    python3 - "$@" <<'EOF'
import struct, sys
with open(sys.argv[1], "r+b") as f:
    f.seek(28)
    if len(sys.argv) > 2:
        f.write(struct.pack("<Q", int(sys.argv[2])))
    else:
        print(struct.unpack("<Q", f.read(8))[0])
EOF
}

start
sleep 1.5
expect_fsck 0 "note: a running budyk (pid $PID)" --config "$DIR/config.yaml"
expect_fsck 8 "in use by a running budyk (pid $PID): stop it before --repair" --repair "$DIR/data"
if "$BIN" serve --config "$DIR/config.yaml" > "$DIR/second.log" 2>&1; then
    echo "FAIL: a second daemon started on the same data_dir"; fail=1
elif grep -qF "is in use by another budyk (pid $PID" "$DIR/second.log"; then
    echo "ok   a second daemon on the same data_dir is refused"
else
    echo "FAIL: second daemon: $(cat "$DIR/second.log")"; fail=1
fi
stop
expect_fsck 0 "0 with problems left" "$DIR/data"

RING="$DIR/data/level-burst.ring"
n=$(widx "$RING")
widx "$RING" $((n - 1))
expect_fsck 4 "1 record written past write_idx and never counted" "$DIR/data"
expect_fsck 1 "counted in" --repair "$DIR/data"
if [ "$(widx "$RING")" = "$n" ]; then echo "ok   write_idx is back at $n"; else echo "FAIL: write_idx $(widx "$RING"), want $n"; fail=1; fi
expect_fsck 0 "0 repaired, 0 with problems left" "$DIR/data"

widx "$RING" $((n - 1))
start
if grep -q "level-burst.ring: 1 record(s) written before a crash and not yet indexed, now counted in" "$DIR/log"; then
    echo "ok   budyk serve counts the record in on start"
else
    echo "FAIL: no start-up recovery in the log"; fail=1
fi
stop
expect_fsck 0 "0 with problems left" "$DIR/data"

# A damaged record: one byte of a payload in the middle of the ring.
python3 - "$RING" <<'EOF'
import struct, sys
with open(sys.argv[1], "r+b") as f:
    f.seek(16)
    rsize = struct.unpack("<I", f.read(4))[0]   # record size, from the header
    f.seek(64 + 2 * rsize + 40)                 # slot 2, inside the payload
    b = f.read(1)
    f.seek(-1, 1)
    f.write(bytes([b[0] ^ 0x5A]))
EOF
expect_fsck 4 "1 damaged record (slot 2): skipped on read, cannot be repaired" "$DIR/data"
expect_fsck 4 "1 with problems left" --repair "$DIR/data"
expect_fsck 8 "not a directory" "$DIR/config.yaml"

if grep -qE 'ERROR: (Address|Leak)Sanitizer|ThreadSanitizer|runtime error:' "$DIR/log" "$DIR/fsck.out"; then
    echo "FAIL: sanitizer report"; fail=1
fi
if [ "$fail" -ne 0 ]; then
    echo "--- daemon log ---"; cat "$DIR/log"
    exit 1
fi
echo "fsck smoke test: PASS"
