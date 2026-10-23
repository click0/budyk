#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
#
# Smoke test for the dashboard's login: with web.auth on, the API wants
# a session; a WebSocket opened with one is dropped when the session
# ends (the upgrade checked it once, and the stream used to outlive a
# logout); a WebSocket from another site's page (an Origin that is not
# the Host) is refused.
#
#   tests/smoke/auth.sh path/to/budyk [port]

set -eu
BIN=${1:?usage: auth.sh path/to/budyk [port]}
PORT=${2:-18775}
DIR=$(mktemp -d)
PID=
trap 'kill "$PID" 2>/dev/null || true; rm -rf "$DIR"' EXIT

HASH=$(printf 'smoke-pass\n' | "$BIN" hash-password 2>/dev/null)
cat > "$DIR/config.yaml" <<EOF
data_dir: $DIR/data
listen: 127.0.0.1
port: $PORT
collection:
  l3: { interval: 1, grace_period: 2 }
web:
  auth:
    enabled: true
    password_hash: "$HASH"
    persist_sessions: false
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
code=$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/api/samples")
if [ "$code" = 401 ]; then echo "ok   /api/samples without a session -> 401"
else echo "FAIL: /api/samples without a session -> $code"; fail=1; fi

python3 - "$PORT" > "$DIR/py.out" 2>&1 <<'EOF' || { echo "FAIL: $(cat "$DIR/py.out")"; fail=1; }
import base64, os, socket, sys, time, urllib.request
port = int(sys.argv[1])
base = "http://127.0.0.1:%d" % port

def request(method, path, body=None, cookie=None):
    req = urllib.request.Request(base + path, data=body, method=method)
    if cookie:
        req.add_header("Cookie", cookie)
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status, r.headers
    except urllib.error.HTTPError as e:
        return e.code, e.headers

status, headers = request("POST", "/api/auth/login", b'{"password":"smoke-pass"}')
if status != 200:
    sys.exit("login: %d" % status)
cookie = headers["Set-Cookie"].split(";", 1)[0]
if request("GET", "/api/samples", cookie=cookie)[0] != 200:
    sys.exit("/api/samples with the session is not 200")
print("ok   login gives a session the API takes")

def upgrade(extra):
    s = socket.create_connection(("127.0.0.1", port))
    key = base64.b64encode(os.urandom(16)).decode()
    s.send(("GET /api/ws HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nUpgrade: websocket\r\n"
            "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\n"
            "Sec-WebSocket-Version: 13\r\n%s\r\n" % (port, key, extra)).encode())
    s.settimeout(5)
    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = s.recv(4096)
        if not chunk:
            break
        buf += chunk
    return s, buf

s, head = upgrade("Cookie: %s\r\nOrigin: http://evil.example\r\n" % cookie)
s.close()
if not head.startswith(b"HTTP/1.1 403"):
    sys.exit("a cross-origin WebSocket was not refused: %r" % head[:40])
print("ok   a WebSocket from another site's page -> 403")

s, head = upgrade("Cookie: %s\r\nOrigin: http://127.0.0.1:%d\r\n" % (cookie, port))
if not head.startswith(b"HTTP/1.1 101"):
    sys.exit("the dashboard's own WebSocket was refused: %r" % head[:40])
print("ok   the dashboard's WebSocket (same origin, session) -> 101")

if request("POST", "/api/auth/logout", b"", cookie=cookie)[0] != 200:
    sys.exit("logout failed")
# The hub checks sessions every tick (L3: once a second): the stream
# ends with a close frame (1008) within a few seconds.
s.settimeout(0.5)
data, end, closed = b"", time.time() + 6, False
while time.time() < end and not closed:
    try:
        chunk = s.recv(65536)
        if not chunk:
            closed = True
        data += chunk
    except socket.timeout:
        pass
s.close()
if not closed:
    sys.exit("the WebSocket outlived the logout")
if b"\x88\x02\x03\xf0" not in data:
    sys.exit("no close frame with 1008 before the end")
if request("GET", "/api/samples", cookie=cookie)[0] != 401:
    sys.exit("/api/samples after logout is not 401")
print("ok   logout ends the session's WebSocket (close 1008) and its API access")
EOF
cat "$DIR/py.out"

kill -TERM "$PID"
if ! wait "$PID"; then
    echo "FAIL: daemon exited non-zero"; fail=1
fi
PID=
if grep -qE 'ERROR: (Address|Leak)Sanitizer|ThreadSanitizer|runtime error:' "$DIR/log"; then
    echo "FAIL: sanitizer report"; fail=1
fi
if [ "$fail" -ne 0 ]; then
    echo "--- daemon log ---"; cat "$DIR/log"
    exit 1
fi
echo "auth smoke test: PASS"
