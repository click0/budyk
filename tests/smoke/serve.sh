#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
#
# Smoke test for `budyk serve`: the part of the daemon unit tests don't
# reach (HTTP server, WebSocket hub, collection loop, signals). Starts the
# daemon with a custom level and a rule, exercises every endpoint plus a
# WebSocket client and SIGHUP, then stops it with SIGTERM.
#
# Fails if any request gets an unexpected status, the daemon doesn't exit
# 0, or its log has a sanitizer report — so run it against an
# ENABLE_SANITIZERS=ON build to catch memory errors and UB in main.cpp.
#
#   tests/smoke/serve.sh path/to/budyk [port]

set -eu
BIN=${1:?usage: serve.sh path/to/budyk [port]}
PORT=${2:-18765}
DIR=$(mktemp -d)
trap 'kill "$PID" 2>/dev/null || true; rm -rf "$DIR"' EXIT

# data_dir does not exist yet: the daemon creates it (mode 0750).
cat > "$DIR/config.yaml" <<EOF
data_dir: $DIR/data
listen: 127.0.0.1
port: $PORT
rules: { path: $DIR/rules.lua }
collection:
  l1: { interval: 2 }
  l3: { interval: 1, grace_period: 2 }
  levels:
    - { name: burst, interval: 0.3, priority: 40, when: "load.avg_1m >= 0", hold: 1, storage_mb: 1 }
EOF
cat > "$DIR/rules.lua" <<'EOF'
n = 0
watch("tick", { when = function() n = n + 1; return true end, action = "log", message = "tick", cooldown = 2 })
watch("esc",  { when = function() return n == 3 end, action = function() escalate("L3", 2) end })
watch("beat", { when = function() return n % 4 == 0 end, severity = "critical",
                message = "every fourth tick", cooldown = 0 })
EOF

# The rules file is left group/world-writable on purpose: the daemon
# must warn about it (whoever can write it runs Lua as the daemon).
chmod 666 "$DIR/rules.lua"

"$BIN" serve --config "$DIR/config.yaml" > "$DIR/log" 2>&1 &
PID=$!

# Wait for the HTTP server.
i=0
until curl -s -o /dev/null "http://127.0.0.1:$PORT/api/health"; do
    i=$((i + 1))
    [ "$i" -lt 50 ] || { echo "daemon did not start"; cat "$DIR/log"; exit 1; }
    sleep 0.1
done

fail=0
if grep -q "rules file '$DIR/rules.lua' is writable by group or others" "$DIR/log"; then
    echo "ok   warned about the world-writable rules file"
else
    echo "FAIL: no warning about the world-writable rules file"; fail=1
fi
if grep -q "created data_dir '$DIR/data'" "$DIR/log" && [ -d "$DIR/data" ] &&
   [ "$(stat -c %a "$DIR/data" 2>/dev/null || stat -f %Lp "$DIR/data")" = 750 ]; then
    echo "ok   created the missing data_dir, mode 0750"
else
    echo "FAIL: data_dir was not created (or not 0750)"; fail=1
fi
ring_mode=$(stat -c %a "$DIR/data/tier1.ring" 2>/dev/null || stat -f %Lp "$DIR/data/tier1.ring")
if [ "$ring_mode" = 600 ]; then
    echo "ok   ring files are 0600"
else
    echo "FAIL: tier1.ring has mode $ring_mode, expected 600"; fail=1
fi
# Numbers on the command line are parsed whole (atoi() read "99999" as
# a port and "5s" as 5 ms): a bad one is an error that names it.
cli_rejects() {   # cli_rejects <expected message> <args...>
    msg=$1; shift
    if "$BIN" "$@" > /dev/null 2> "$DIR/cli.err" < /dev/null; then
        echo "FAIL: budyk $* succeeded"; fail=1
    elif ! grep -qF -- "$msg" "$DIR/cli.err"; then
        echo "FAIL: budyk $*: no '$msg' in: $(cat "$DIR/cli.err")"; fail=1
    else
        echo "ok   budyk $* -> rejected"
    fi
}
cli_rejects "--port wants a number from 1 to 65535, got '99999'" tui --port 99999
cli_rejects "--timeout wants milliseconds (-1 for none), got '5s'" watch-files --timeout 5s "$DIR"

expect() {   # expect <status> <path>
    got=$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT$2")
    if [ "$got" != "$1" ]; then
        echo "FAIL $2: expected $1, got $got"; fail=1
    else
        echo "ok   $2 -> $got"
    fi
}
expect 200 /
expect 200 /api/health
expect 200 /api/samples
expect 200 /api/levels
expect 200 /api/alerts
expect 200 "/api/range?level=all"
expect 200 "/api/range?level=burst"
expect 404 "/api/range?level=nope"
expect 200 "/api/range?tier=2&since=1&until=99999999999999999999999&limit=0"
expect 404 /nope

# A WebSocket client that reads frames for a while, and one that drops
# mid-way. The frames carry samples and, from the "beat" rule, alerts
# (spec §3.6.4: an alert is a WS event).
python3 - "$PORT" > "$DIR/ws.out" <<'EOF' || { echo "FAIL: websocket client: $(cat "$DIR/ws.out")"; fail=1; }
import base64, os, socket, sys, time
port = int(sys.argv[1])
def connect():
    s = socket.create_connection(("127.0.0.1", port))
    key = base64.b64encode(os.urandom(16)).decode()
    s.send(("GET /api/ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\n"
            "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n" % key).encode())
    return s
s = connect()
s.settimeout(0.2)
buf = b""
while b"\r\n\r\n" not in buf:
    buf += s.recv(4096)
head, buf = buf.split(b"\r\n\r\n", 1)
if not head.startswith(b"HTTP/1.1 101"):
    sys.exit("websocket handshake failed")
frames, end = [], time.time() + 2.5
while time.time() < end:
    try:
        chunk = s.recv(65536)
        if not chunk:
            break
        buf += chunk
    except socket.timeout:
        pass
    while len(buf) >= 2:            # server frames: unmasked
        n, off = buf[1] & 0x7f, 2
        if n == 126:
            if len(buf) < 4: break
            n, off = int.from_bytes(buf[2:4], "big"), 4
        elif n == 127:
            if len(buf) < 10: break
            n, off = int.from_bytes(buf[2:10], "big"), 10
        if len(buf) < off + n: break
        frames.append(buf[off:off + n].decode())
        buf = buf[off + n:]
s.close()
connect().close()
junk = socket.create_connection(("127.0.0.1", port))
junk.send(b"\x00\xff not http\r\n\r\n")
junk.close()
alerts = [f for f in frames if f.startswith('{"count":') and '"alerts":[' in f]
if not any('"samples":[' in f for f in frames) or not alerts:
    sys.exit("frames: %d, with alerts: %d" % (len(frames), len(alerts)))
if '"rule":"beat"' not in alerts[0] or '"severity":"critical"' not in alerts[0]:
    sys.exit("alert frame: " + alerts[0])
print("ok   the WebSocket carries samples and alert events")
EOF
cat "$DIR/ws.out"
if curl -s "http://127.0.0.1:$PORT/api/alerts" | grep -q '"rule":"beat","message":"every fourth tick"' &&
   grep -q 'budyk alert: critical beat: every fourth tick' "$DIR/log"; then
    echo "ok   alerts are logged and kept for /api/alerts"
else
    echo "FAIL: no beat alert in /api/alerts or in the log"; fail=1
fi

# The per-entity detail (cores, disks, interfaces, filesystems) rides
# along with /api/samples: one core at least, the filesystems the totals
# counted.
if curl -s "http://127.0.0.1:$PORT/api/samples" | python3 -c '
import json, sys
doc = json.load(sys.stdin)
d, s = doc["detail"], doc["samples"][-1]
assert len(d["cores"]) >= 1 and len(d["cores"]) == s["cpu"]["count"], d["cores"]
assert len(d["filesystems"]) == s["fs"]["count"], (d["filesystems"], s["fs"])
assert all(m["mount"].startswith("/") for m in d["filesystems"])
assert isinstance(d["disks"], list) and isinstance(d["interfaces"], list)
'; then
    echo "ok   /api/samples carries the per-entity detail"
else
    echo "FAIL: no or inconsistent detail in /api/samples"; fail=1
fi

# The burst level holds throughout, yet the L1 ring still gets a record
# per L1 interval (2 s here), folded from the burst samples; level=all
# shows those samples once, from the burst ring, without the folded copy.
i=0
until curl -s "http://127.0.0.1:$PORT/api/range?level=L1" | grep -q '"folded":true'; do
    i=$((i + 1))
    [ "$i" -lt 50 ] || break
    sleep 0.1
done
if curl -s "http://127.0.0.1:$PORT/api/range?level=L1" | grep -q '"folded":true'; then
    echo "ok   the L1 ring gets folded records while the burst level runs"
else
    echo "FAIL: no folded record in the L1 ring"; fail=1
fi
all=$(curl -s "http://127.0.0.1:$PORT/api/range?level=all")
if echo "$all" | grep -q '"folded"'; then
    echo "FAIL: level=all repeats folded records"; fail=1
elif ! echo "$all" | grep -q "\"level\":4,"; then
    echo "FAIL: level=all has no burst samples"; fail=1
else
    echo "ok   level=all shows the burst samples, not their folded copies"
fi

# Every descriptor the daemon holds must be close-on-exec (rule exec()
# and the alert channels fork): the listening socket, the WS client
# above, the ring files, the wake pipe. Linux only — FreeBSD has no
# fdinfo; the flags are set with the same calls on both.
if [ -d "/proc/$PID/fdinfo" ]; then
    python3 - "$PID" <<'EOF' || fail=1
import os, sys
pid = sys.argv[1]
# Descriptors the daemon inherited from this shell (a CI runner keeps
# a few pipes open in every child) are not its to set CLOEXEC on. This
# script inherited the same ones, so anything it holds itself is
# excluded by identity ("pipe:[123]", "/path").
def targets(p):
    out = {}
    for fd in os.listdir("/proc/%s/fd" % p):
        try:
            out[fd] = os.readlink("/proc/%s/fd/%s" % (p, fd))
        except OSError:
            pass
    return out
inherited = set(targets("self").values())
bad = []
for fd, target in targets(pid).items():
    if int(fd) < 3 or target in inherited:
        continue
    try:
        with open("/proc/%s/fdinfo/%s" % (pid, fd)) as f:
            flags = [l.split()[1] for l in f if l.startswith("flags:")][0]
    except OSError:
        continue
    if not int(flags, 8) & 0o2000000:
        bad.append("fd %s -> %s (flags %s)" % (fd, target, flags))
if bad:
    print("FAIL: descriptors without O_CLOEXEC:\n  " + "\n  ".join(bad))
    sys.exit(1)
print("ok   all descriptors are close-on-exec")
EOF
fi

kill -HUP "$PID"
sleep 1.5
kill -TERM "$PID"
if ! wait "$PID"; then
    echo "FAIL: daemon exited non-zero"; fail=1
fi
PID=

if grep -qE 'ERROR: (Address|Leak)Sanitizer|ThreadSanitizer|runtime error:' "$DIR/log"; then
    echo "FAIL: sanitizer report"; fail=1
fi
grep -q 'rules reloaded' "$DIR/log" || { echo "FAIL: SIGHUP reload not logged"; fail=1; }

if [ "$fail" -ne 0 ]; then
    echo "--- daemon log ---"; cat "$DIR/log"
    exit 1
fi
echo "serve smoke test: PASS"
