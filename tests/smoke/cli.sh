#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
#
# The one-shot commands on real data:
#
#   - suggest-rules reads the L1 and L2 rings (spec §3.7.1): a daemon
#     that ran with no client (adaptive: L1 only, the L3 ring empty) gave
#     "Generated from 0 samples", since only the L3 ring was read;
#   - suggest-rules names a ring it cannot read, and why; watch-files
#     with nothing to watch and hash-password with an argument or a full
#     output fail (they exited 0);
#   - suggest-rules says when its output cannot be written (/dev/full),
#     and refuses a --window past a year however it is written (a huge
#     number wrapped to some hours);
#   - Ctrl-C during suggest-rules --ai (a stand-in curl that takes its
#     time) kills curl and leaves no temp file in $TMPDIR, the API key's
#     among them (they stayed, and curl finished the request alone);
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

# A config that is not the daemon's (rings of other sizes): each ring
# said, with why (it was "cannot read ... run `budyk serve` first").
{ cat "$DIR/config.yaml"; printf 'storage:\n  tier2_max_mb: 3\n  tier3_max_mb: 3\n'; } > "$DIR/other.yaml"
if "$BIN" suggest-rules --config "$DIR/other.yaml" --window 1h > /dev/null 2> "$DIR/err"; then
    echo "FAIL: suggest-rules on rings of another size exited 0"; fail=1
elif grep -q "tier2.ring': file size does not match" "$DIR/err" &&
     grep -q "tier3.ring': file size does not match" "$DIR/err"; then
    echo "ok   a ring that cannot be read is named, with why"
else
    echo "FAIL: suggest-rules on rings of another size: $(cat "$DIR/err")"; fail=1
fi

# watch-files with nothing it can watch, hash-password with an argument
# or a full output: each a failure (each exited 0; watch-files waited out
# its timeout first).
if "$BIN" watch-files --timeout 100 "$DIR/no-such-file" > /dev/null 2> "$DIR/err"; then
    echo "FAIL: watch-files on nothing it can watch exited 0"; fail=1
elif grep -q "none of the paths can be watched" "$DIR/err"; then
    echo "ok   watch-files with nothing to watch is a failure"
else echo "FAIL: watch-files on nothing: $(cat "$DIR/err")"; fail=1; fi
if printf 'pw\n' | "$BIN" hash-password --rounds 3 > /dev/null 2> "$DIR/err"; then
    echo "FAIL: hash-password took an argument it does not have"; fail=1
else echo "ok   hash-password refuses an argument"; fi
if [ -w /dev/full ]; then
    if printf 'pw\n' | "$BIN" hash-password > /dev/full 2> "$DIR/err"; then
        echo "FAIL: hash-password > /dev/full exited 0"; fail=1
    elif grep -q "writing the standard output: No space left on device" "$DIR/err"; then
        echo "ok   hash-password says when the hash cannot be written"
    else echo "FAIL: hash-password > /dev/full: $(cat "$DIR/err")"; fail=1; fi
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
python3 - "$BIN" "$DIR" <<'EOF' || fail=1
import os, signal, subprocess, sys, time
binary, d = sys.argv[1], sys.argv[2]
os.makedirs(d + "/bin")
os.makedirs(d + "/tmp")
with open(d + "/bin/curl", "w") as f:
    f.write("#!/bin/sh\ntouch '%s/started'\nsleep 3\ntouch '%s/finished'\n" % (d, d))
os.chmod(d + "/bin/curl", 0o755)
env = dict(os.environ, PATH=d + "/bin:" + os.environ["PATH"], TMPDIR=d + "/tmp",
           ANTHROPIC_API_KEY="sk-smoke-not-a-key")
p = subprocess.Popen([binary, "suggest-rules", "--config", d + "/config.yaml",
                      "--window", "1h", "--ai"], env=env, stderr=subprocess.PIPE, text=True)
end = time.time() + 10
while not os.path.exists(d + "/started") and time.time() < end and p.poll() is None:
    time.sleep(0.05)
if not os.path.exists(d + "/started"):
    p.kill()
    sys.exit("FAIL: the stand-in curl never started: %s" % p.communicate()[1].strip())
while_running = os.listdir(d + "/tmp")
p.send_signal(signal.SIGINT)                     # Ctrl-C
try:
    err = p.communicate(timeout=5)[1]
except subprocess.TimeoutExpired:
    p.kill()
    sys.exit("FAIL: suggest-rules --ai went on after SIGINT")
time.sleep(4)                                    # past the stand-in's 3 s
left = os.listdir(d + "/tmp")
if not while_running:
    sys.exit("FAIL: no temp file while curl ran: the check proves nothing")
if p.returncode != -signal.SIGINT:
    sys.exit("FAIL: suggest-rules --ai: exit %d after SIGINT: %s" % (p.returncode, err.strip()))
if left:
    sys.exit("FAIL: temp files left in TMPDIR after Ctrl-C: %s" % left)
if os.path.exists(d + "/finished"):
    sys.exit("FAIL: curl went on with the request after budyk was interrupted")
print("ok   Ctrl-C during suggest-rules --ai kills curl and removes its %d temp files"
      % len(while_running))
EOF
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

# stdin at its end (`budyk tui < /dev/null`, a script, a piped password
# once read): getch() returned ERR at once instead of after a second, and
# the loop fetched as fast as the daemon answered, about 700 requests a
# second on the one HTTP thread. One request a second whatever stdin is.
# The fake API answers every request with a sample at once.
#
# Ending the TUI: a SIGTERM runs ncurses' endwin(), which sets the
# terminal back with TCSADRAIN; on FreeBSD that waits for the output to
# be read, up to kern.tty_drainwait (300 s), so the master is read until
# the TUI is gone (a plain waitpid took 5 minutes there).
python3 - "$BIN" <<'EOF' || fail=1
import os, pty, select, signal, socket, sys, threading, time
binary = sys.argv[1]
def end_tui(pid, fd):
    os.kill(pid, signal.SIGTERM)
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
srv = socket.socket()
srv.bind(("127.0.0.1", 0))
srv.listen(64)
port = srv.getsockname()[1]
count = {"samples": 0}
sample = (b'[{"ts":1,"level":1,"groups":["cpu"],"cpu":{"total_percent":1.0,"count":1}}]')
def serve():
    while True:
        try:
            c, _ = srv.accept()
        except OSError:
            return
        req = b""
        while b"\r\n\r\n" not in req:
            chunk = c.recv(4096)
            if not chunk:
                break
            req += chunk
        if b"GET /api/samples" in req:
            count["samples"] += 1
        c.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %d\r\n"
                  b"Connection: close\r\n\r\n" % len(sample) + sample)
        c.close()
threading.Thread(target=serve, daemon=True).start()
pid, fd = pty.fork()
if pid == 0:
    os.environ["TERM"] = "xterm"
    devnull = os.open(os.devnull, os.O_RDONLY)
    os.dup2(devnull, 0)                      # stdin at its end
    os.execv(binary, [binary, "tui", "--port", str(port)])
end = time.time() + 4
while time.time() < end:
    try:
        if select.select([fd], [], [], 0.2)[0]:
            os.read(fd, 65536)
    except OSError:
        break
n = count["samples"]
end_tui(pid, fd)
if n > 8:
    sys.exit("FAIL: the TUI with stdin at its end made %d /api/samples requests in 4 s" % n)
if n < 2:
    sys.exit("FAIL: the TUI with stdin at its end made only %d request(s) in 4 s" % n)
print("ok   the TUI with stdin at its end asks once a second: %d requests in 4 s" % n)
EOF

# The session ends (the daemon restarted with a new password hash) and
# the stored password is no longer accepted: the TUI posted it once a
# second for ever, which with the daemon's throttle (5 failures a
# minute) locked its address, a reverse proxy's address meaning everyone,
# out of the login, and said only "login required or expired". One
# attempt, then the reason on the screen.
#
# The password prompt turns echo off with TCSAFLUSH, which drops what was
# typed ahead; on FreeBSD it first waits for the prompt to be read, so an
# answer sent the moment the prompt shows can land before the flush and be
# dropped with it: the answer is sent half a second later. The TUI is
# ended as above (endwin() drains the terminal).
python3 - "$BIN" <<'EOF' || fail=1
import os, pty, select, signal, socket, sys, threading, time
binary = sys.argv[1]
def end_tui(pid, fd):
    os.kill(pid, signal.SIGTERM)
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
srv = socket.socket()
srv.bind(("127.0.0.1", 0))
srv.listen(64)
port = srv.getsockname()[1]
count = {"login": 0, "samples": 0}
def reply(c, status, body, extra=b""):
    c.sendall(b"HTTP/1.1 " + status + b"\r\nContent-Type: application/json\r\n" + extra +
              b"Content-Length: %d\r\nConnection: close\r\n\r\n" % len(body) + body)
def serve():
    while True:
        try:
            c, _ = srv.accept()
        except OSError:
            return
        req = b""
        while b"\r\n\r\n" not in req:
            chunk = c.recv(4096)
            if not chunk:
                break
            req += chunk
        if b"POST /api/auth/login" in req:
            count["login"] += 1
            if count["login"] == 1:          # the password at the prompt: accepted
                reply(c, b"200 OK", b'{"ok":true}',
                      b"Set-Cookie: budyk_session=abc; HttpOnly; Path=/\r\n")
            else:                            # the daemon has a new hash now
                reply(c, b"401 Unauthorized", b'{"error":"invalid credentials"}')
        elif b"GET /api/samples" in req:
            count["samples"] += 1
            reply(c, b"401 Unauthorized", b'{"error":"unauthenticated"}')
        else:
            reply(c, b"200 OK", b'{}')
        c.close()
threading.Thread(target=serve, daemon=True).start()
pid, fd = pty.fork()
if pid == 0:
    os.environ["TERM"] = "xterm"
    os.execv(binary, [binary, "tui", "--port", str(port)])
out = b""
answer_at = None                                 # when to answer the prompt
end = time.time() + 6
while time.time() < end:
    if answer_at is not None and time.time() >= answer_at:
        os.write(fd, b"secret\n")
        answer_at = None
    try:
        if select.select([fd], [], [], 0.1)[0]:
            chunk = os.read(fd, 65536)
            if b"password:" in chunk and not out:  # the prompt, once
                answer_at = time.time() + 0.5
            out += chunk
    except OSError:
        break
logins = count["login"]
end_tui(pid, fd)
text = out.decode("utf-8", "replace")
if logins != 2:
    sys.exit("FAIL: the TUI posted the rejected password %d time(s) in 6 s, not once: %r"
             % (logins - 1, text[-300:]))
if "no longer accepted" not in text:
    sys.exit("FAIL: the TUI did not say the password is no longer accepted: %r" % text[-300:])
print("ok   a rejected password is posted once, and the screen says so")
EOF

if grep -qE 'ERROR: (Address|Leak)Sanitizer|ThreadSanitizer|runtime error:' "$DIR/log"; then
    echo "FAIL: sanitizer report"; fail=1
fi
[ "$fail" -eq 0 ] && echo "cli smoke test: PASS" || { echo "cli smoke test: FAIL"; exit 1; }
