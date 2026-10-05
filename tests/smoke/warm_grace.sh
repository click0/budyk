#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
#
# hot_buffer.warm_grace on the running daemon (spec §3.4, M4.2): once no
# client has been connected for the grace period, the hot buffer is
# emptied, so the next client's catch-up does not replay a 1 Hz session
# that ended earlier.
#
# A WebSocket client holds L3 for 4 s and leaves. L1 is set to 30 s, so
# without the warm-grace wake-up the daemon would sleep through the
# reset. A second client connects 5 s later (warm_grace 2 s): its
# catch-up frame must hold at most the sample taken at the reset, not
# the first session's samples.
#
#   tests/smoke/warm_grace.sh path/to/budyk [port]

set -eu
BIN=${1:?usage: warm_grace.sh path/to/budyk [port]}
PORT=${2:-18767}
DIR=$(mktemp -d)
PID=
trap 'kill "$PID" 2>/dev/null || true; rm -rf "$DIR"' EXIT

cat > "$DIR/config.yaml" <<EOF
data_dir: $DIR
listen: 127.0.0.1
port: $PORT
collection:
  l1: { interval: 30 }
  l3: { interval: 1, grace_period: 1 }
  hot_buffer: { warm_grace: 2 }
EOF

"$BIN" serve --config "$DIR/config.yaml" > "$DIR/log" 2>&1 &
PID=$!
i=0
until curl -s -o /dev/null "http://127.0.0.1:$PORT/api/health"; do
    i=$((i + 1))
    [ "$i" -lt 50 ] || { echo "daemon did not start"; cat "$DIR/log"; exit 1; }
    sleep 0.1
done

fail=0
python3 - "$PORT" <<'EOF' || fail=1
import base64, json, os, socket, struct, sys, time
port = int(sys.argv[1])

def session(secs):
    """Connect, read frames for `secs`, close. Returns the decoded frames;
    the first is the catch-up."""
    s = socket.create_connection(("127.0.0.1", port))
    key = base64.b64encode(os.urandom(16)).decode()
    s.send(("GET /api/ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\n"
            "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n" % key).encode())
    buf = b""
    while b"\r\n\r\n" not in buf:
        buf += s.recv(4096)
    buf = buf.split(b"\r\n\r\n", 1)[1]
    s.settimeout(0.2)
    frames, t = [], time.time()
    while time.time() - t < secs:
        try:
            d = s.recv(65536)
        except socket.timeout:
            d = b""
        buf += d
        while len(buf) >= 2:
            n, o = buf[1] & 127, 2
            if n == 126:
                n, o = struct.unpack(">H", buf[2:4])[0], 4
            elif n == 127:
                n, o = struct.unpack(">Q", buf[2:10])[0], 10
            if len(buf) < o + n:
                break
            frames.append(json.loads(buf[o:o + n]))
            buf = buf[o + n:]
    s.close()
    return frames

first = session(4.0)
seen = first[0]["count"] + len(first) - 1
time.sleep(5.0)
second = session(0.5)
kept = second[0]["count"]
print("first session: %d sample(s); catch-up 5 s after it left: %d" % (seen, kept))
if seen < 3:
    sys.exit("FAIL: the first session saw too few samples to tell")
if kept > 2:
    sys.exit("FAIL: the hot buffer was not emptied after warm_grace")
print("ok   hot buffer emptied after warm_grace")
EOF

kill -TERM "$PID"
wait "$PID" || { echo "FAIL: daemon exited non-zero after SIGTERM"; fail=1; }
PID=

if grep -E "ERROR: (Address|Leak)Sanitizer|ThreadSanitizer|runtime error:" "$DIR/log"; then
    echo "FAIL: sanitizer report in the daemon log"; fail=1
fi

[ "$fail" -eq 0 ] && echo "warm_grace smoke test: PASS" || { echo "warm_grace smoke test: FAIL"; tail -20 "$DIR/log"; }
exit "$fail"
