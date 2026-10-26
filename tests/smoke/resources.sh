#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
#
# The resource figures of spec §5 on the running daemon:
#
#   item 2  idle (adaptive, no client) for 60 s: RSS <= 10 MB, CPU about
#           0 (at most 0.5 % of one CPU), disk < 1 KB/min (the records
#           written in those 60 s, at their size);
#   item 3  one WebSocket client at L3 (1 Hz) for 30 s: RSS <= 25 MB, CPU
#           <= 1 % of one CPU;
#   item 4  L1 -> L3: <= 2 s from the client's connect to its first L3
#           sample;
#   item 5  catch-up: a client gets the whole hot buffer (300 records)
#           within 100 ms of its connect;
#   item 6  the binary, static and stripped: <= 5 MB on amd64 (reported
#           elsewhere).
#
# The idle figures need the daemon at L1: when the host's own load or CPU
# puts it at L2 (critical thresholds, not configurable), they are
# reported and not checked. For a release build: sanitizer and coverage
# builds are several times larger and slower than these figures allow.
#
#   tests/smoke/resources.sh path/to/budyk [port]

set -eu
BIN=${1:?usage: resources.sh path/to/budyk [port]}
PORT=${2:-18768}
DIR=$(mktemp -d)
PID=
trap 'kill "$PID" 2>/dev/null || true; rm -rf "$DIR"' EXIT
fail=0

start() {   # start <config>
    "$BIN" serve --config "$1" > "$DIR/log" 2>&1 &
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
    wait "$PID" 2>/dev/null || true
    PID=
}

# --- item 6: the binary ---------------------------------------------------
if file "$BIN" | grep -q "statically linked"; then
    strip -o "$DIR/budyk.stripped" "$BIN"
    size=$(wc -c < "$DIR/budyk.stripped" | tr -d ' ')
    machine=$(uname -m)
    case "$machine" in
        amd64|x86_64)
            if [ "$size" -le 5242880 ]; then
                echo "ok   the stripped static binary is $size bytes (<= 5 MB)"
            else
                echo "FAIL: the stripped static binary is $size bytes (> 5 MB)"; fail=1
            fi ;;
        *)  echo "ok   the stripped static binary is $size bytes ($machine: not checked)" ;;
    esac
else
    echo "ok   not a static binary: its size is not checked"
fi

# The measurements, against the daemon's pid: idle, then one client.
cat > "$DIR/measure.py" <<'EOF'
import base64, json, os, socket, struct, subprocess, sys, time, urllib.request

port, pid, phase = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3]
fail = False

def bad(msg):
    global fail
    print("FAIL: " + msg)
    fail = True

def get(path):
    return json.loads(urllib.request.urlopen(
        "http://127.0.0.1:%d%s" % (port, path), timeout=10).read())

def cpu_seconds():
    # Linux: utime + stime of the whole process; FreeBSD: ps(1) time,
    # [[h:]m:]ss.cc.
    try:
        with open("/proc/%d/stat" % pid) as f:
            st = f.read()
        fields = st[st.rindex(")") + 2:].split()
        return (int(fields[11]) + int(fields[12])) / os.sysconf("SC_CLK_TCK")
    except OSError:
        out = subprocess.run(["ps", "-o", "time=", "-p", str(pid)],
                             capture_output=True, text=True).stdout.strip()
        secs = 0.0
        for part in out.split(":"):
            secs = secs * 60 + float(part)
        return secs

def rss_kb():
    return int(subprocess.run(["ps", "-o", "rss=", "-p", str(pid)],
                              capture_output=True, text=True).stdout.strip())

def stored():
    return [get("/api/range?tier=%d" % t)["samples"] for t in (1, 2, 3)]

def connect():
    s = socket.create_connection(("127.0.0.1", port))
    key = base64.b64encode(os.urandom(16)).decode()
    s.send(("GET /api/ws HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\n"
            "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n" % key).encode())
    return s

class Frames:
    def __init__(self, s):
        self.s, self.buf, self.head = s, b"", False
    def frame(self):
        # One whole frame's JSON off the buffer, or None.
        if not self.head:
            if b"\r\n\r\n" not in self.buf:
                return None
            self.buf, self.head = self.buf.split(b"\r\n\r\n", 1)[1], True
        if len(self.buf) < 2:
            return None
        n, o = self.buf[1] & 127, 2
        if n == 126:
            if len(self.buf) < 4: return None
            n, o = struct.unpack(">H", self.buf[2:4])[0], 4
        elif n == 127:
            if len(self.buf) < 10: return None
            n, o = struct.unpack(">Q", self.buf[2:10])[0], 10
        if len(self.buf) < o + n:
            return None
        payload, self.buf = self.buf[o:o + n], self.buf[o + n:]
        return json.loads(payload)
    def next(self, deadline):
        # The next text frame's JSON, or None at the deadline.
        while True:
            m = self.frame()
            if m is not None:
                return m
            left = deadline - time.time()
            if left <= 0:
                return None
            self.s.settimeout(left)
            try:
                d = self.s.recv(65536)
            except socket.timeout:
                return None
            if not d:
                return None
            self.buf += d

if phase == "idle":
    time.sleep(2)
    before, c0, t0 = stored(), cpu_seconds(), time.time()
    time.sleep(60)
    after, c1, t1 = stored(), cpu_seconds(), time.time()
    rss = rss_kb()
    new = [smp for b, a in zip(before, after) for smp in a[len(b):]]
    if any(smp["level"] != 1 for smp in new):
        print("ok   idle: the host put the daemon above L1 (load %.1f): not checked"
              % new[-1]["load"]["avg_1m"])
    else:
        cpu = (c1 - c0) / (t1 - t0) * 100
        per_min = len(new) * 542 / ((t1 - t0) / 60)
        print("ok   idle: RSS %d KB, CPU %.2f %% of one CPU, %d record(s) in %.0f s"
              % (rss, cpu, len(new), t1 - t0))
        if rss > 10 * 1024: bad("idle RSS %d KB > 10 MB" % rss)
        if cpu > 0.5:       bad("idle CPU %.2f %% > 0.5 %%" % cpu)
        if per_min >= 1024: bad("idle disk %.0f B/min >= 1 KB/min" % per_min)

elif phase == "active":
    t0 = time.time()
    f = Frames(connect())
    first = None
    while first is None:
        m = f.next(t0 + 10)
        if m is None:
            break
        if any(smp["level"] == 3 for smp in m.get("samples", [])):
            first = time.time() - t0
    if first is None:
        bad("no L3 sample within 10 s of the connect")
    else:
        print("ok   L1 -> L3: the first L3 sample %.0f ms after the connect" % (first * 1000))
        if first > 2: bad("L1 -> L3 took %.2f s > 2 s" % first)
    c0, t1 = cpu_seconds(), time.time()
    n3 = 0
    while time.time() < t1 + 30:
        m = f.next(t1 + 30)
        if m is None:
            break
        n3 += sum(1 for smp in m.get("samples", []) if smp["level"] == 3)
    cpu = (cpu_seconds() - c0) / (time.time() - t1) * 100
    rss = rss_kb()
    print("ok   one client at L3: RSS %d KB, CPU %.2f %% of one CPU, %d L3 sample(s) in 30 s"
          % (rss, cpu, n3))
    if n3 < 25:         bad("%d L3 samples in 30 s: not 1 Hz" % n3)
    if rss > 25 * 1024: bad("active RSS %d KB > 25 MB" % rss)
    if cpu > 1:         bad("active CPU %.2f %% > 1 %%" % cpu)

elif phase == "catchup":
    deadline = time.time() + 60
    while get("/api/samples")["count"] < 300 and time.time() < deadline:
        time.sleep(0.5)
    t0 = time.time()
    m = Frames(connect()).next(t0 + 5)
    dt = time.time() - t0
    n = len(m["samples"]) if m else 0
    print("ok   catch-up: %d record(s) %.1f ms after the connect" % (n, dt * 1000))
    if n != 300: bad("the catch-up frame holds %d records, not 300" % n)
    if dt > 0.1: bad("the catch-up took %.1f ms > 100 ms" % (dt * 1000))

sys.exit(1 if fail else 0)
EOF

# --- items 2, 3, 4: idle, then one client at L3 ---------------------------
# Thresholds out of reach, so the host's own load does not raise L2 (the
# critical ones are fixed; see above).
cat > "$DIR/idle.yaml" <<EOF
data_dir: $DIR/idle
listen: 127.0.0.1
port: $PORT
collection:
  l2:
    escalate_after: 0
    escalation_thresholds: { load_1m: 1000, cpu_percent: 100, swap_used_percent: 100 }
EOF
start "$DIR/idle.yaml"
python3 "$DIR/measure.py" "$PORT" "$PID" idle || fail=1
python3 "$DIR/measure.py" "$PORT" "$PID" active || fail=1
stop

# --- item 5: the catch-up of a full hot buffer ---------------------------
# A level at 10 Hz fills the 300 records in 30 s.
cat > "$DIR/catchup.yaml" <<EOF
data_dir: $DIR/catchup
listen: 127.0.0.1
port: $PORT
collection:
  hot_buffer: { capacity: 300 }
  levels:
    - { name: fill, interval: 0.1, priority: 40, when: "true", storage_mb: 1 }
EOF
start "$DIR/catchup.yaml"
python3 "$DIR/measure.py" "$PORT" "$PID" catchup || fail=1
stop

if grep -q "ERROR: AddressSanitizer\|runtime error:" "$DIR/log"; then
    echo "FAIL: sanitizer report"; cat "$DIR/log"; fail=1
fi
[ "$fail" -eq 0 ] && echo "resources smoke test: PASS" || { echo "resources smoke test: FAIL"; exit 1; }
