#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
#
# Crash test for `budyk serve` (spec §5 item 7, design decision 5: a
# crash loses at most one record). Runs the daemon with a fast custom
# level, kills it with SIGKILL at a random moment while a WebSocket
# client is reading samples, and starts it again on the same data_dir,
# several times over. After each restart:
#
#   - the daemon starts and serves /api/range on the existing rings;
#   - every sample the client saw before the kill, except possibly the
#     last one, is in the ring;
#   - timestamps in the ring are strictly increasing;
#   - the log has no sanitizer report.
#
#   tests/smoke/crash.sh path/to/budyk [port] [rounds]

set -eu
BIN=${1:?usage: crash.sh path/to/budyk [port] [rounds]}
PORT=${2:-18766}
ROUNDS=${3:-8}
DIR=$(mktemp -d)
PID=
trap 'kill -9 "$PID" 2>/dev/null || true; rm -rf "$DIR"' EXIT

cat > "$DIR/config.yaml" <<EOF
data_dir: $DIR
listen: 127.0.0.1
port: $PORT
collection:
  levels:
    - { name: burst, interval: 0.2, priority: 40, when: "true", storage_mb: 1 }
EOF

start() {
    "$BIN" serve --config "$DIR/config.yaml" >> "$DIR/log" 2>&1 &
    PID=$!
    i=0
    until curl -s -o /dev/null "http://127.0.0.1:$PORT/api/health"; do
        i=$((i + 1))
        [ "$i" -lt 50 ] || { echo "FAIL: daemon did not start"; cat "$DIR/log"; exit 1; }
        sleep 0.1
    done
}

# The client reads samples for a random 0.5-2.5 s, then kills the daemon
# itself, so the last sample it saw is as close to the kill as it gets.
# It appends the timestamps it saw (one per line) to seen.<round>.
watch_and_kill() {   # <round>
    python3 - "$PORT" "$PID" "$DIR/seen.$1" <<'EOF'
import base64, json, os, random, signal, socket, struct, sys, time, urllib.request
port, pid, out = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3]
# Only burst samples go to the burst ring; the first sample after a
# start can be L1, taken before the level's condition was evaluated.
levels = json.loads(urllib.request.urlopen(
    "http://127.0.0.1:%d/api/levels" % port).read())
burst = [lv["id"] for lv in levels if lv["name"] == "burst"][0]
s = socket.create_connection(("127.0.0.1", port))
key = base64.b64encode(os.urandom(16)).decode()
s.send(("GET /api/ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\n"
        "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n" % key).encode())
buf = b""
while b"\r\n\r\n" not in buf:
    buf += s.recv(4096)
buf = buf.split(b"\r\n\r\n", 1)[1]
seen = []
def parse():
    global buf
    while len(buf) >= 2:
        n, o = buf[1] & 127, 2
        if n == 126:
            if len(buf) < 4: return
            n, o = struct.unpack(">H", buf[2:4])[0], 4
        elif n == 127:
            if len(buf) < 10: return
            n, o = struct.unpack(">Q", buf[2:10])[0], 10
        if len(buf) < o + n: return
        payload, buf = buf[o:o + n], buf[o + n:]
        for smp in json.loads(payload).get("samples", []):
            if smp["level"] == burst:
                seen.append(smp["ts"])
s.settimeout(0.05)
deadline = time.time() + random.uniform(0.5, 2.5)
while time.time() < deadline:
    try:
        d = s.recv(65536)
    except socket.timeout:
        continue
    if not d:
        break
    buf += d
    parse()
os.kill(pid, signal.SIGKILL)
with open(out, "w") as f:
    f.write("".join("%d\n" % t for t in seen))
EOF
    wait "$PID" 2>/dev/null || true
}

fail=0
start
r=1
while [ "$r" -le "$ROUNDS" ]; do
    watch_and_kill "$r"
    start
    python3 - "$PORT" "$DIR/seen.$r" "$r" <<'EOF' || fail=1
import json, sys, urllib.request
port, seen_path, rnd = int(sys.argv[1]), sys.argv[2], sys.argv[3]
seen = [int(x) for x in open(seen_path).read().split()]
body = urllib.request.urlopen(
    "http://127.0.0.1:%d/api/range?level=burst&limit=5000" % port).read()
stored = [smp["ts"] for smp in json.loads(body)["samples"]]
ok = True
if any(b <= a for a, b in zip(stored, stored[1:])):
    print("FAIL round %s: ring timestamps are not strictly increasing" % rnd); ok = False
have = set(stored)
missing = [t for t in seen if t not in have]
# The last sample seen may have been pushed before it was stored.
lost_ok = len(missing) == 0 or (len(missing) == 1 and missing[0] == max(seen))
if not seen:
    print("FAIL round %s: the client saw no samples" % rnd); ok = False
elif not lost_ok:
    print("FAIL round %s: %d of %d samples seen before the kill are not in the ring"
          % (rnd, len(missing), len(seen))); ok = False
print("%s round %s: saw %d, ring holds %d, lost %d"
      % ("ok  " if ok else "FAIL", rnd, len(seen), len(stored), len(missing)))
sys.exit(0 if ok else 1)
EOF
    r=$((r + 1))
done

kill -TERM "$PID"
wait "$PID" || { echo "FAIL: daemon exited non-zero after SIGTERM"; fail=1; }
PID=

if grep -E "ERROR: (Address|Leak)Sanitizer|ThreadSanitizer|runtime error:" "$DIR/log"; then
    echo "FAIL: sanitizer report in the daemon log"; fail=1
fi

[ "$fail" -eq 0 ] && echo "crash test: PASS" || { echo "crash test: FAIL"; tail -20 "$DIR/log"; }
exit "$fail"
