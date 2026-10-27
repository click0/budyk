#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
#
# The one-shot commands on real data:
#
#   - suggest-rules reads the L1 and L2 rings (spec §3.7.1): a daemon
#     that ran with no client (adaptive: L1 only, the L3 ring empty) gave
#     "Generated from 0 samples", since only the L3 ring was read;
#   - suggest-rules says when its output cannot be written (/dev/full),
#     and refuses a --window past a year however it is written (a huge
#     number wrapped to some hours);
#   - the TUI against an address that takes the connection and never
#     answers gives up within seconds with the reason (it waited for
#     ever, `q` unread);
#   - the TUI started while an idle client holds the daemon's HTTP
#     thread (up to its 5 s I/O bound) waits its turn and draws (it gave
#     up after 3 s: "can't reach").
#
# The thresholds are out of reach: on a busy host (load over 4) the
# daemon went to L2, 30 s a sample, and the rings held one.
#
#   tests/smoke/cli.sh path/to/budyk [port]

set -eu
BIN=${1:?usage: cli.sh path/to/budyk [port]}
PORT=${2:-18776}
DIR=$(mktemp -d)
PID=
trap 'kill "$PID" 2>/dev/null || true; rm -rf "$DIR"' EXIT
fail=0

cat > "$DIR/config.yaml" <<EOF
data_dir: $DIR/data
listen: 127.0.0.1
port: $PORT
collection:
  l1: { interval: 1 }
  l2:
    escalation_thresholds: { load_1m: 1000, cpu_percent: 1000, swap_used_percent: 1000 }
EOF
"$BIN" serve --config "$DIR/config.yaml" > "$DIR/log" 2>&1 &
PID=$!
sleep 6

# A client that connects and sends nothing, then the TUI (in a pty).
python3 - "$BIN" "$PORT" <<'EOF' || fail=1
import os, pty, select, signal, socket, sys, time
binary, port = sys.argv[1], sys.argv[2]
idle = socket.create_connection(("127.0.0.1", int(port)))
time.sleep(0.2)
t = time.time()
pid, fd = pty.fork()
if pid == 0:
    os.environ["TERM"] = "xterm"
    os.execv(binary, [binary, "tui", "--port", port])
out, drew = b"", None
while time.time() < t + 15 and drew is None:
    if select.select([fd], [], [], 0.2)[0]:
        try:
            out += os.read(fd, 65536)
        except OSError:
            break
    if b"CPU" in out:
        drew = time.time() - t
idle.close()
try:
    os.write(fd, b"q")
except OSError:
    pass
end = time.time() + 5
while os.waitpid(pid, os.WNOHANG)[0] == 0:     # read what it writes until it quits
    if time.time() > end:
        os.kill(pid, signal.SIGKILL)
        os.waitpid(pid, 0)
        break
    try:
        if select.select([fd], [], [], 0.1)[0]:
            os.read(fd, 65536)
    except OSError:
        time.sleep(0.1)
text = out.decode("utf-8", "replace")
if drew is None:
    sys.exit("FAIL: the TUI behind an idle client did not draw: %r" % text[-300:])
if drew < 3:
    sys.exit("FAIL: the TUI drew in %.1f s: the idle client did not hold the daemon" % drew)
print("ok   the TUI behind an idle client waits its turn: drew in %.1f s" % drew)
EOF

kill -TERM "$PID"
wait "$PID" || { echo "FAIL: daemon exited non-zero"; fail=1; }
PID=

if "$BIN" suggest-rules --config "$DIR/config.yaml" --window 1h > "$DIR/rules.lua" 2> "$DIR/err"; then
    n=$(sed -n 's/.*Generated from \([0-9]*\) samples.*/\1/p' "$DIR/rules.lua" | head -1)
    if [ "${n:-0}" -ge 3 ]; then
        echo "ok   suggest-rules read the L1 ring: $n samples"
    else
        echo "FAIL: suggest-rules found ${n:-no} samples (want 3 or more)"; head -5 "$DIR/rules.lua"; fail=1
    fi
else
    echo "FAIL: suggest-rules: $(cat "$DIR/err")"; fail=1
fi

if [ -w /dev/full ]; then
    if "$BIN" suggest-rules --config "$DIR/config.yaml" --window 1h --output /dev/full 2> "$DIR/err"; then
        echo "FAIL: suggest-rules --output /dev/full exited 0: $(cat "$DIR/err")"; fail=1
    elif grep -q "writing '/dev/full': No space left on device" "$DIR/err"; then
        echo "ok   a write that fails is said so (exit 1)"
    else
        echo "FAIL: suggest-rules --output /dev/full: $(cat "$DIR/err")"; fail=1
    fi
fi
took=
for w in 367d 213503982334602d 99999999999999999999d 0d -1d 7 d; do
    if "$BIN" suggest-rules --config "$DIR/config.yaml" --window "$w" > /dev/null 2>&1; then
        took="$took $w"
    fi
done
if [ -z "$took" ]; then echo "ok   --window past a year or not a duration is refused"
else echo "FAIL: suggest-rules took --window$took"; fail=1; fi

# An address that takes the connection and says nothing.
python3 - "$BIN" "$DIR" <<'EOF' || fail=1
import os, socket, subprocess, sys, time
binary, d = sys.argv[1], sys.argv[2]
srv = socket.socket()
srv.bind(("127.0.0.1", 0))
srv.listen(8)
port = srv.getsockname()[1]
t = time.time()
try:
    p = subprocess.run([binary, "tui", "--port", str(port)], stdin=subprocess.DEVNULL,
                       capture_output=True, text=True, timeout=15)
except subprocess.TimeoutExpired:
    sys.exit("FAIL: budyk tui waited for ever on a daemon that does not answer")
took = time.time() - t
if p.returncode != 0 and "timed out" in p.stderr and took < 12:
    print("ok   the TUI gives up on a silent daemon in %.1f s: %s" % (took, p.stderr.strip()))
else:
    sys.exit("FAIL: budyk tui: exit %d after %.1f s: %s" % (p.returncode, took, p.stderr.strip()))
EOF

# A reply that takes 9 s, a piece a second (a slow link): longer than the
# TUI's bound on a wait, which is on a wait with no progress, so it is
# read (the TUI timed the whole request and gave up). Every reply is a
# 503, which the TUI then names.
python3 - "$BIN" <<'EOF' || fail=1
import os, pty, select, signal, socket, sys, threading, time
binary = sys.argv[1]
srv = socket.socket()
srv.bind(("127.0.0.1", 0))
srv.listen(8)
port = srv.getsockname()[1]
def serve():
    first = True
    while True:
        c, _ = srv.accept()
        req = b""
        while b"\r\n\r\n" not in req:
            chunk = c.recv(4096)
            if not chunk:
                break
            req += chunk
        body = b"x" * 9
        c.sendall(b"HTTP/1.1 503 Busy\r\nContent-Length: 9\r\nConnection: close\r\n\r\n")
        for i in range(len(body)):
            if first:
                time.sleep(1)
            c.sendall(body[i:i + 1])
        first = False
        c.close()
threading.Thread(target=serve, daemon=True).start()
t = time.time()
pid, fd = pty.fork()
if pid == 0:
    os.environ["TERM"] = "xterm"
    os.execv(binary, [binary, "tui", "--port", str(port)])
out = b""
while time.time() < t + 20 and b"HTTP 503" not in out:
    if select.select([fd], [], [], 0.2)[0]:
        try:
            out += os.read(fd, 65536)
        except OSError:
            break
took = time.time() - t
try:
    os.write(fd, b"q")
except OSError:
    pass
end = time.time() + 5
while os.waitpid(pid, os.WNOHANG)[0] == 0:
    if time.time() > end:
        os.kill(pid, signal.SIGKILL)
        os.waitpid(pid, 0)
        break
    try:
        if select.select([fd], [], [], 0.1)[0]:
            os.read(fd, 65536)
    except OSError:
        time.sleep(0.1)
if b"HTTP 503" not in out:
    sys.exit("FAIL: the TUI did not read a reply that took 9 s: %r"
             % out.decode("utf-8", "replace")[-300:])
print("ok   the TUI reads a reply that takes 9 s: drew in %.1f s" % took)
EOF

if grep -qE 'ERROR: (Address|Leak)Sanitizer|ThreadSanitizer|runtime error:' "$DIR/log"; then
    echo "FAIL: sanitizer report"; fail=1
fi
[ "$fail" -eq 0 ] && echo "cli smoke test: PASS" || { echo "cli smoke test: FAIL"; exit 1; }
