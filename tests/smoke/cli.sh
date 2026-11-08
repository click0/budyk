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
#     output fail (they exited 0); watch-files waits for a path that is
#     not there yet (alone, it was refused), prints every change in its
#     timeout (it exited at the first), each as it comes, and a SIGTERM
#     ends `--timeout -1` with 0;
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

# A sanitizer's report from any process this script starts goes to
# $DIR/san.<pid>, checked at the end: the one-shot commands' expected
# failures accepted any non-zero exit with the message (an ASan abort
# after it passed), and the TUI's output in a pty was read and dropped.
export ASAN_OPTIONS="${ASAN_OPTIONS:+$ASAN_OPTIONS:}log_path=$DIR/san"
export UBSAN_OPTIONS="${UBSAN_OPTIONS:+$UBSAN_OPTIONS:}log_path=$DIR/san"
export TSAN_OPTIONS="${TSAN_OPTIONS:+$TSAN_OPTIONS:}log_path=$DIR/san"

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

# A client that connects and sends a request's head but not its body,
# then nothing more (one that sent nothing at all, or stalled in its
# head, is cut off after a second now), then the TUI (in a pty).
python3 - "$BIN" "$PORT" <<'EOF' || fail=1
import os, pty, select, signal, socket, sys, time
binary, port = sys.argv[1], sys.argv[2]
idle = socket.create_connection(("127.0.0.1", int(port)))
idle.sendall(b"POST /api/health HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 10\r\n\r\n")
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

# A window with no sample in it is said so, exit 1, and nothing is
# written: Tier A wrote a header alone ("Generated from 0 samples"),
# --ai sent "no samples available" to the API. The daemon stopped over
# two seconds ago, so a 1 s window holds nothing.
sleep 2
if "$BIN" suggest-rules --config "$DIR/config.yaml" --window 1s --output "$DIR/none.lua" 2> "$DIR/err"; then
    echo "FAIL: suggest-rules with no sample in the window exited 0"; fail=1
elif grep -q "no samples in the last 1s (the rings hold [1-9][0-9]* older records); nothing written" "$DIR/err" &&
     [ ! -e "$DIR/none.lua" ]; then
    echo "ok   suggest-rules with no sample in the window says so and writes nothing"
else
    echo "FAIL: suggest-rules with no sample in the window: $(cat "$DIR/err")"; fail=1
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

# watch-files with nothing it can watch (a path under a file), hash-password
# with an argument or a full output: each a failure (each exited 0;
# watch-files waited out its timeout first). A path that is not there
# yet is watched, as security.file_watch watches it, and reported as
# created when it appears (alone, it was "none of the paths can be
# watched", though next to a path that existed it was watched).
if "$BIN" watch-files --timeout 100 "$DIR/config.yaml/under-a-file" > /dev/null 2> "$DIR/err"; then
    echo "FAIL: watch-files on nothing it can watch exited 0"; fail=1
elif grep -q "none of the paths can be watched" "$DIR/err"; then
    echo "ok   watch-files with nothing to watch is a failure"
else echo "FAIL: watch-files on nothing: $(cat "$DIR/err")"; fail=1; fi
( sleep 0.5; : > "$DIR/later" ) &
if "$BIN" watch-files --timeout 5000 "$DIR/later" > "$DIR/out" 2> "$DIR/err" &&
   grep -q "^created .*/later" "$DIR/out" && grep -q "1 not there yet" "$DIR/out"; then
    echo "ok   watch-files waits for a path that is not there yet"
else echo "FAIL: watch-files on a path not there yet: $(cat "$DIR/out" "$DIR/err")"; fail=1; fi
wait
# Every change in the timeout is printed, as it comes (it exited at the
# first: a write, then a removal a second later, showed the write alone),
# and with -1 a SIGTERM ends it, exit 0, its lines already out on a pipe
# (stdout to a file or a pipe waited for the exit).
: > "$DIR/w2"
( sleep 0.5; echo a >> "$DIR/w2"; sleep 0.7; rm "$DIR/w2" ) &
if "$BIN" watch-files --timeout 2500 "$DIR/w2" > "$DIR/out" 2> "$DIR/err" &&
   grep -q "^modified .*/w2" "$DIR/out" && grep -q "^deleted .*/w2" "$DIR/out"; then
    echo "ok   watch-files prints every change in its timeout"
else echo "FAIL: watch-files printed the first change alone: $(cat "$DIR/out" "$DIR/err")"; fail=1; fi
wait
: > "$DIR/w3"
"$BIN" watch-files --timeout -1 "$DIR/w3" > "$DIR/out" 2> "$DIR/err" &
WPID=$!
sleep 0.5
lines_first=$(wc -l < "$DIR/out")            # "watching 1 file(s)", before any change
echo a >> "$DIR/w3"
sleep 0.5
lines_before=$(wc -l < "$DIR/out")
kill -TERM "$WPID" 2>/dev/null || true
wrc=0
wait "$WPID" || wrc=$?
if [ "$wrc" = 0 ] && [ "$lines_first" -ge 1 ] && [ "$lines_before" -ge 2 ] &&
   grep -q "^modified .*/w3" "$DIR/out"; then
    echo "ok   watch-files --timeout -1: lines out as they come, a SIGTERM ends it with 0"
else echo "FAIL: watch-files --timeout -1: $lines_first, then $lines_before line(s) before the TERM, exit $wrc: $(cat "$DIR/out" "$DIR/err")"; fail=1; fi
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

# The LLM's reply, through a stand-in curl that writes a canned one: a
# NUL byte in it (as \u0000) is refused (it hid the code after it from
# the check, which Lua then loaded); a rule calling exec() is refused
# while the config has exec off, as the daemon would refuse the file
# (the check passed it with exec on, the daemon loaded nothing); a good
# reply is written, and `exec` named in it is noted.
python3 - "$BIN" "$DIR" <<'EOF' || fail=1
import json, os, subprocess, sys
binary, d = sys.argv[1], sys.argv[2]
with open(d + "/bin/curl", "w") as f:
    f.write("#!/bin/sh\nwhile [ $# -gt 1 ]; do if [ \"$1\" = -o ]; then cp '%s/reply.json' \"$2\"; fi; shift; done\n" % d)
os.chmod(d + "/bin/curl", 0o755)
env = dict(os.environ, PATH=d + "/bin:" + os.environ["PATH"], TMPDIR=d + "/tmp",
           ANTHROPIC_API_KEY="sk-smoke-not-a-key")
def run(text, out):
    with open(d + "/reply.json", "w") as f:
        json.dump({"content": [{"type": "text", "text": text}], "stop_reason": "end_turn"}, f)
    if os.path.exists(out):
        os.unlink(out)
    p = subprocess.run([binary, "suggest-rules", "--config", d + "/config.yaml", "--window", "1h",
                        "--ai", "--output", out], env=env, stderr=subprocess.PIPE, text=True)
    return p.returncode, p.stderr
good = "watch(\"a\", { when = function() return cpu.total_percent > 95 end })\n"
rc, err = run("```lua\n" + good + "-- \u0000\nwatch(\"evil\", { when = function() return true end, action = exec{\"/bin/true\"} })\n```", d + "/ai1.lua")
if rc != 1 or "control byte (0x00 at offset" not in err or os.path.exists(d + "/ai1.lua"):
    sys.exit("FAIL: a reply with a NUL: exit %d, %s" % (rc, err.strip()))
rc, err = run("```lua\n" + good + "watch(\"e\", { when = function() return true end, action = exec{\"/bin/true\"} })\n```", d + "/ai2.lua")
if rc != 1 or "exec is disabled" not in err or os.path.exists(d + "/ai2.lua"):
    sys.exit("FAIL: a reply calling exec() with exec off in the config: exit %d, %s" % (rc, err.strip()))
rc, err = run("```lua\n" + good + "-- no exec here, exec is off\n```", d + "/ai3.lua")
if rc != 0 or "note: the suggestion calls exec()" not in err or not os.path.exists(d + "/ai3.lua"):
    sys.exit("FAIL: a good reply: exit %d, %s" % (rc, err.strip()))
if good not in open(d + "/ai3.lua").read():
    sys.exit("FAIL: the good reply was not written as it was")
print("ok   suggest-rules --ai refuses a reply with a NUL, and one calling exec() while exec is off")
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

# The TUI against a fake API, in a pty:
#   - a key that is not a quit is not a fetch: 200 key presses over 2 s
#     sent 200 requests (a held key, a paste), about 3 now;
#   - `q` while a request waits on a stalled daemon ends the TUI at once
#     (the key was read after the 8 s bound);
#   - a reply that streams for ever is cut at 16 MiB and named (it was
#     read into memory until the OOM killer);
#   - a login that does not get through when the session has to be
#     renewed (the connection closed with no reply: a restart, a proxy)
#     is tried again (one such gave up for good, "the password is no
#     longer accepted", though the password was right);
#   - the Procs line has no iowait while the cpu group is a baseline.
python3 - "$BIN" <<'EOF' || fail=1
import os, pty, select, signal, socket, sys, threading, time
binary = sys.argv[1]
SAMPLE = (b'{"count":1,"samples":[{"ts":1,"level":1,"groups":["cpu","mem","proc","uptime"],'
          b'"cpu":{"total_percent":1.0,"count":4,"iowait_percent":0},'
          b'"mem":{"total":1000,"available":500,"available_percent":50},'
          b'"proc":{"total":10,"blocked":0,"zombie":0},"uptime_seconds":100}],'
          b'"detail":{"collected":[],"processes":[]}}\n')
BASELINE = SAMPLE.replace(b'"groups":["cpu","mem","proc","uptime"],',
                          b'"groups":["mem","proc","uptime"],"baseline":["cpu"],')
class Server:
    def __init__(self, handler):
        self.srv = socket.socket()
        self.srv.bind(("127.0.0.1", 0))
        self.srv.listen(64)
        self.port = self.srv.getsockname()[1]
        self.handler = handler
        self.log = []
        threading.Thread(target=self.loop, daemon=True).start()
    def loop(self):
        while True:
            try:
                c, _ = self.srv.accept()
            except OSError:
                return
            threading.Thread(target=self.one, args=(c,), daemon=True).start()
    def one(self, c):
        req = b""
        while b"\r\n\r\n" not in req:
            ch = c.recv(4096)
            if not ch:
                break
            req += ch
        line = req.split(b"\r\n", 1)[0].decode("utf-8", "replace")
        self.log.append(line)
        try:
            self.handler(c, line, req)
        except OSError:
            pass
        try:
            c.close()
        except OSError:
            pass
def reply(c, status, body, extra=b""):
    c.sendall(b"HTTP/1.1 " + status + b"\r\nContent-Type: application/json\r\n" + extra +
              b"Content-Length: %d\r\nConnection: close\r\n\r\n" % len(body) + body)
def spawn(port):
    pid, fd = pty.fork()
    if pid == 0:
        os.environ["TERM"] = "xterm"
        os.execv(binary, [binary, "tui", "--port", str(port)])
    return pid, fd
def pump(fd, secs, out=b"", until=None):
    end = time.time() + secs
    while time.time() < end:
        try:
            if select.select([fd], [], [], 0.05)[0]:
                out += os.read(fd, 65536)
                if until is not None and until in out:
                    break
        except OSError:
            break
    return out
def wait_exit(pid, fd, secs):
    end = time.time() + secs
    t0 = time.time()
    while time.time() < end:
        got, st = os.waitpid(pid, os.WNOHANG)
        if got == pid:
            return st, time.time() - t0
        pump(fd, 0.1)
    os.kill(pid, signal.SIGKILL)
    os.waitpid(pid, 0)
    return None, time.time() - t0

# 1. Keys that are not a quit.
s = Server(lambda c, line, req: reply(c, b"200 OK", SAMPLE))
pid, fd = spawn(s.port)
pump(fd, 2.0)
n0 = sum(1 for e in s.log if "samples" in e)
for i in range(200):
    os.write(fd, b"x"); time.sleep(0.01)
pump(fd, 0.5)
n1 = sum(1 for e in s.log if "samples" in e)
os.write(fd, b"q")
st, took = wait_exit(pid, fd, 10)
if st is None or n1 - n0 > 6:
    sys.exit("FAIL: 200 key presses over 2.5 s made %d requests (exit %r)" % (n1 - n0, st))
print("ok   the TUI makes %d request(s) during 200 key presses over 2.5 s" % (n1 - n0))

# 2. `q` while the daemon has stalled.
state = {"stall": False}
def stalling(c, line, req):
    if state["stall"] and "samples" in line:
        time.sleep(20); return
    reply(c, b"200 OK", SAMPLE)
s = Server(stalling)
pid, fd = spawn(s.port)
pump(fd, 1.5)
state["stall"] = True
pump(fd, 1.5)
os.write(fd, b"q")
st, took = wait_exit(pid, fd, 15)
if st is None or took > 3:
    sys.exit("FAIL: `q` with the daemon stalled: exit %r after %.1f s" % (st, took))
print("ok   `q` while a request waits on a stalled daemon ends the TUI in %.1f s" % took)

# 3. A reply that streams for ever.
state = {"stream": False}
def streaming(c, line, req):
    if state["stream"] and "samples" in line:
        c.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\n")
        chunk = b"x" * 65536
        try:
            while True:
                c.sendall(chunk)
        except OSError:
            return
    reply(c, b"200 OK", SAMPLE)
s = Server(streaming)
pid, fd = spawn(s.port)
out = pump(fd, 1.5)
state["stream"] = True
out = pump(fd, 6, out, until=b"reply over 16 MiB")
os.write(fd, b"q")
st, took = wait_exit(pid, fd, 15)
if b"reply over 16 MiB" not in out or st is None:
    sys.exit("FAIL: a streaming reply: %r" % out.decode("utf-8", "replace")[-300:])
print("ok   a reply that streams for ever is cut at 16 MiB and named")

# 4. A login that does not get through at renewal; the password stays.
cnt = {"login": 0, "samples": 0, "valid": False}
def renewing(c, line, req):
    if "login" in line:
        cnt["login"] += 1
        if cnt["login"] == 2:
            return                           # closed without a reply
        cnt["valid"] = True
        reply(c, b"200 OK", b'{"ok":true}', b"Set-Cookie: budyk_session=abc; Path=/\r\n"); return
    if "samples" in line:
        cnt["samples"] += 1
        if cnt["samples"] == 4:
            cnt["valid"] = False             # the session ended
        if not cnt["valid"] or b"Cookie: budyk_session=abc" not in req:
            reply(c, b"401 Unauthorized", b'{}'); return
        reply(c, b"200 OK", SAMPLE); return
    reply(c, b"200 OK", b"{}")
s = Server(renewing)
pid, fd = spawn(s.port)
out = pump(fd, 3, until=b"assword")
time.sleep(0.5)          # the prompt, then echo off with TCSAFLUSH: typed sooner, the password is flushed
os.write(fd, b"secret\n")
out = pump(fd, 16, out, until=b"tick #12")
os.write(fd, b"q")
st, took = wait_exit(pid, fd, 10)
text = out.decode("utf-8", "replace")
if cnt["login"] < 3 or "no longer accepted" in text or "logging in again" not in text:
    sys.exit("FAIL: renewal after a dropped login: %d login(s); %r" % (cnt["login"], text[-400:]))
print("ok   a login that did not get through at renewal is tried again: %d logins" % cnt["login"])

# 5. No iowait next to a CPU row of "measuring...".
s = Server(lambda c, line, req: reply(c, b"200 OK", BASELINE))
pid, fd = spawn(s.port)
out = pump(fd, 3, until=b"measuring")
os.write(fd, b"q")
st, took = wait_exit(pid, fd, 10)
text = out.decode("utf-8", "replace")
if "measuring" not in text or "iowait" in text:
    sys.exit("FAIL: a cpu baseline: %r" % text[-400:])
print("ok   the Procs line has no iowait while the cpu group is a baseline")
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

# Ctrl-C ends the TUI as `q` does, exit 0: initscr() gave SIGINT a
# handler of ncurses' own, which set the terminal back and exited 1, so
# a script running the TUI read a failure in a plain quit. The fake API
# answers every request with a sample at once.
python3 - "$BIN" <<'EOF' || fail=1
import os, pty, select, signal, socket, sys, threading, time
binary = sys.argv[1]
srv = socket.socket()
srv.bind(("127.0.0.1", 0))
srv.listen(64)
port = srv.getsockname()[1]
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
        c.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %d\r\n"
                  b"Connection: close\r\n\r\n" % len(sample) + sample)
        c.close()
threading.Thread(target=serve, daemon=True).start()
pid, fd = pty.fork()
if pid == 0:
    os.environ["TERM"] = "xterm"
    os.execv(binary, [binary, "tui", "--port", str(port)])
out = b""
end = time.time() + 5
while time.time() < end and b"CPU" not in out:        # drawn once
    try:
        if select.select([fd], [], [], 0.1)[0]:
            out += os.read(fd, 65536)
    except OSError:
        break
os.kill(pid, signal.SIGINT)                          # Ctrl-C
status = None
end = time.time() + 5
while time.time() < end:
    got, st = os.waitpid(pid, os.WNOHANG)
    if got == pid:
        status = st
        break
    try:                                             # endwin() drains the terminal
        if select.select([fd], [], [], 0.1)[0]:
            os.read(fd, 65536)
    except OSError:
        time.sleep(0.1)
if status is None:
    os.kill(pid, signal.SIGKILL)
    os.waitpid(pid, 0)
    sys.exit("FAIL: the TUI did not end within 5 s of Ctrl-C")
if not os.WIFEXITED(status) or os.WEXITSTATUS(status) != 0:
    sys.exit("FAIL: the TUI ended on Ctrl-C with %s, not exit 0"
             % ("signal %d" % os.WTERMSIG(status) if os.WIFSIGNALED(status)
                else "exit %d" % os.WEXITSTATUS(status)))
print("ok   Ctrl-C ends the TUI as q does: exit 0")
EOF

if grep -qE 'ERROR: (Address|Leak)Sanitizer|ThreadSanitizer|runtime error:' "$DIR/log" "$DIR"/san.* 2>/dev/null; then
    echo "FAIL: sanitizer report"; cat "$DIR"/san.* 2>/dev/null; fail=1
fi
[ "$fail" -eq 0 ] && echo "cli smoke test: PASS" || { echo "cli smoke test: FAIL"; exit 1; }
