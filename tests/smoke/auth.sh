#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
#
# Smoke test for the dashboard's login: with web.auth on, the API wants
# a session; a WebSocket opened with one is dropped when the session
# ends (the upgrade checked it once, and the stream used to outlive a
# logout); a WebSocket from another site's page (an Origin that is not
# the Host) is refused. The config says `enabled: True`, as Python and
# Ansible write it (it used to leave the dashboard open without a word),
# and has a key budyk does not know, which the log must name. Its
# `rules: path:` is left empty: no rules, said so (a null kept the default
# path, and the daemon loaded the rules file the FreeBSD package puts
# there). Then an exec allowlist written as mappings: it allows nothing
# (it was left empty, which let exec() run anything).
#
#   tests/smoke/auth.sh path/to/budyk [port]

set -eu
BIN=${1:?usage: auth.sh path/to/budyk [port]}
PORT=${2:-18775}
DIR=$(mktemp -d)
PID=
trap 'kill "$PID" 2>/dev/null || true; rm -rf "$DIR"' EXIT

HASH=$(printf 'smoke-pass\n' | "$BIN" hash-password 2>/dev/null)

# Ctrl-C (SIGINT) at hash-password's prompt (on a terminal: a pty here)
# leaves the terminal echoing again; it was left with echo off. The pty
# is not the child's controlling terminal (FreeBSD revokes that when the
# child exits), so the SIGINT is sent, not typed; its output is read as a
# terminal's would be (FreeBSD's TCSAFLUSH waits for that). Under TSan the
# handler runs late and the read ends first (EINTR: an empty password,
# exit 1): either way the terminal must echo again.
python3 - "$BIN" > "$DIR/pty.out" 2>&1 <<'EOF' || { echo "FAIL: $(cat "$DIR/pty.out")"; exit 1; }
import os, pty, select, signal, sys, termios, time
master, slave = pty.openpty()
pid = os.fork()
if pid == 0:
    signal.signal(signal.SIGINT, signal.SIG_DFL)    # whatever the CI shell left
    for f in (0, 1, 2):
        os.dup2(slave, f)
    os.execv(sys.argv[1], [sys.argv[1], "hash-password"])
def drain():
    while select.select([master], [], [], 0)[0]:
        try:
            os.read(master, 4096)
        except OSError:
            return
echo = lambda: termios.tcgetattr(slave)[3] & termios.ECHO
deadline = time.time() + 5
while echo() and time.time() < deadline:
    drain()                                # until the prompt turns echo off
    time.sleep(0.05)
if echo():
    sys.exit("the password prompt did not turn echo off")
os.kill(pid, signal.SIGINT)
deadline = time.time() + 5
while time.time() < deadline:
    drain()
    done, status = os.waitpid(pid, os.WNOHANG)
    if done:
        break
    time.sleep(0.05)
else:
    os.kill(pid, signal.SIGKILL)
    sys.exit("hash-password went on after SIGINT")
if os.WIFEXITED(status) and os.WEXITSTATUS(status) == 0:
    sys.exit("hash-password exited 0 after SIGINT")
if not echo():
    sys.exit("Ctrl-C at the password prompt left the terminal without echo")
EOF
echo "ok   Ctrl-C at the password prompt gives the terminal its echo back"
cat > "$DIR/config.yaml" <<EOF
data_dir: $DIR/data
listen: 127.0.0.1
port: $PORT
collection:
  l3: { interval: 1, grace_period: 2 }
web:
  auth:
    enabled: True
    password_hash: "$HASH"
    persist_sessions: false
rules:
  path:
colour: red
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
if grep -q "budyk config: colour: unknown key; ignored" "$DIR/log"; then
    echo "ok   an unknown key is named in the log"
else echo "FAIL: no word in the log on the unknown key 'colour'"; fail=1; fi
if grep -q "budyk serve: rules.path is empty; running without rules" "$DIR/log"; then
    echo "ok   an empty rules.path is no rules, and the log says so"
else echo "FAIL: an empty rules.path: $(grep -i rules "$DIR/log" | head -3)"; fail=1; fi
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

# An exec allowlist written as mappings (`- path: ...`) has nothing
# usable in it: it allows nothing, and says so. It was left empty, and
# an empty one let a rule's exec() run any absolute path.
mkdir -p "$DIR/data2"
cat > "$DIR/rules2.lua" <<EOF
watch('touch', { cooldown = 0, when = function()
  return exec({ '/bin/sh', '-c', 'touch $DIR/PWNED' }, { timeout = 5, wait = true }).ok
end })
EOF
cat > "$DIR/config2.yaml" <<EOF
data_dir: $DIR/data2
listen: 127.0.0.1
port: $PORT
collection:
  mode: always
  l3: { interval: 1 }
rules:
  path: $DIR/rules2.lua
  exec:
    enabled: true
    allow:
      - path: /usr/local/bin/budyk-alerter
EOF
"$BIN" serve --config "$DIR/config2.yaml" >> "$DIR/log" 2>&1 &
PID=$!
sleep 3
kill -TERM "$PID"
wait "$PID" || { echo "FAIL: daemon (exec allowlist) exited non-zero"; fail=1; }
PID=
if [ -e "$DIR/PWNED" ]; then
    echo "FAIL: an allowlist of mappings let exec() run /bin/sh"; fail=1
elif grep -q "rules.exec.allow: no usable entry; it allows nothing" "$DIR/log" &&
     grep -q "exec: '/bin/sh' not in allowlist" "$DIR/log"; then
    echo "ok   an allowlist with no usable entry allows nothing, and says so"
else
    echo "FAIL: exec allowlist of mappings: $(grep -E 'exec|allow' "$DIR/log" | head -3)"; fail=1
fi

if grep -qE 'ERROR: (Address|Leak)Sanitizer|ThreadSanitizer|runtime error:' "$DIR/log"; then
    echo "FAIL: sanitizer report"; fail=1
fi
if [ "$fail" -ne 0 ]; then
    echo "--- daemon log ---"; cat "$DIR/log"
    exit 1
fi
echo "auth smoke test: PASS"
