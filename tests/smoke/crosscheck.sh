#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
#
# budyk's readings against the system's own tools on the same host (spec
# M2 / M3: "matches top, vmstat, netstat, iostat"). A WebSocket client
# holds L3, so the newest sample is at most a second old; each figure is
# then read with the host's tool and compared within a tolerance that
# allows for what changes in between:
#
#   CPUs          sysconf(_SC_NPROCESSORS_ONLN), and one entry per core
#   memory        free -b (Linux); sysctl vm.stats.vm (FreeBSD)
#   swap          free -b (Linux); swapinfo -k (FreeBSD)
#   load, uptime  uptime(1); /proc/uptime or kern.boottime
#   processes     ps -A, and their threads
#   TCP           ss (Linux: established and close-wait, as CurrEstab
#                 counts); netstat -an (FreeBSD: established)
#   filesystems   df -P -k for each mount budyk lists
#   interfaces    /sys/class/net (Linux); ifconfig -l (FreeBSD); no loopback
#   disks         /sys/block (Linux, less loop / ram / zram / dm / md / fd /
#                 sr / nbd); kern.disks (FreeBSD, less cd)
#   a busy loop   one process spinning for 4 s: about 100 % of one CPU in
#                 budyk's top processes, its share in the CPU total
#   a write       64 MB of random bytes, fsync'ed, in the current
#                 directory: at least that much in the disk write rates
#
#   tests/smoke/crosscheck.sh path/to/budyk [port]

set -eu
BIN=${1:?usage: crosscheck.sh path/to/budyk [port]}
PORT=${2:-18769}
DIR=$(mktemp -d)
PID=
trap 'kill "$PID" 2>/dev/null || true; rm -rf "$DIR" "./.budyk-crosscheck.$$"' EXIT

cat > "$DIR/config.yaml" <<EOF
data_dir: $DIR/data
listen: 127.0.0.1
port: $PORT
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
python3 - "$PORT" "./.budyk-crosscheck.$$" <<'EOF' || fail=1
import base64, json, os, re, shutil, socket, subprocess, sys, time, urllib.request

port, scratch = int(sys.argv[1]), sys.argv[2]
FREEBSD = sys.platform.startswith("freebsd")
fail = False

def ok(msg):
    print("ok   " + msg)

def bad(msg):
    global fail
    print("FAIL: " + msg)
    fail = True

def check(name, got, want, tol):
    if abs(got - want) <= tol:
        ok("%s: budyk %s, the system %s" % (name, fmt(got), fmt(want)))
    else:
        bad("%s: budyk %s, the system %s (more than %s apart)"
            % (name, fmt(got), fmt(want), fmt(tol)))

def fmt(v):
    return ("%.2f" % v) if isinstance(v, float) else str(v)

def run(*argv):
    return subprocess.run(argv, capture_output=True, text=True, check=True).stdout

def sysctl(name):
    return run("sysctl", "-n", name).strip()

def newest():
    d = json.loads(urllib.request.urlopen(
        "http://127.0.0.1:%d/api/samples" % port, timeout=10).read())
    return d["samples"][-1], d.get("detail", {})

# A client keeps L3: one sample a second, with the per-entity detail.
ws = socket.create_connection(("127.0.0.1", port))
key = base64.b64encode(os.urandom(16)).decode()
ws.send(("GET /api/ws HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\n"
         "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\n"
         "Sec-WebSocket-Version: 13\r\n\r\n" % key).encode())
deadline = time.time() + 10
while True:
    s, d = newest()
    if s["level"] == 3 and "cpu.cores" in d.get("collected", []) and \
       "cpu" in s["groups"] and "disk" in s["groups"] and "net" in s["groups"]:
        break
    if time.time() > deadline:
        bad("no L3 sample with every group and list within 10 s")
        sys.exit(1)
    time.sleep(0.2)
time.sleep(1.2)
s, d = newest()

# --- CPUs --------------------------------------------------------------------
check("CPUs", s["cpu"]["count"], os.sysconf("SC_NPROCESSORS_ONLN"), 0)
check("per-core entries", len(d["cores"]), s["cpu"]["count"], 0)

# --- Memory and swap ---------------------------------------------------------
GB = 1 << 30
if FREEBSD:
    page = int(sysctl("vm.stats.vm.v_page_size"))
    total = int(sysctl("vm.stats.vm.v_page_count")) * page
    def cnt(n):
        try:
            return int(sysctl("vm.stats.vm." + n))
        except subprocess.CalledProcessError:
            return 0
    avail = (cnt("v_free_count") + cnt("v_inactive_count") +
             cnt("v_cache_count") + cnt("v_laundry_count")) * page
    # The ZFS ARC above its minimum, which the kernel gives back.
    try:
        arc = int(sysctl("kstat.zfs.misc.arcstats.size")) - int(sysctl("kstat.zfs.misc.arcstats.c_min"))
        avail = min(total, avail + max(0, arc))
    except subprocess.CalledProcessError:
        pass
    physmem = int(sysctl("hw.physmem"))
    if not 0.8 * physmem <= s["mem"]["total"] <= physmem:
        bad("memory total %d is not within 80-100 %% of hw.physmem %d"
            % (s["mem"]["total"], physmem))
    sw = [l.split() for l in run("swapinfo", "-k").splitlines()[1:] if l.strip()]
    sw_total = sum(int(f[1]) for f in sw if f[0] != "Total") * 1024
    sw_used = sum(int(f[2]) for f in sw if f[0] != "Total") * 1024
else:
    rows = {l.split()[0].rstrip(":"): l.split()[1:]
            for l in run("free", "-b").splitlines()[1:] if l.strip()}
    total, avail = int(rows["Mem"][0]), int(rows["Mem"][5])
    sw_total, sw_used = int(rows["Swap"][0]), int(rows["Swap"][1])
check("memory total", s["mem"]["total"], total, total // 1000)
check("memory available", s["mem"]["available"], avail, total // 50)
check("swap total", s["swap"]["total"], sw_total, sw_total // 100 + (1 << 20))
check("swap used", s["swap"]["used"], sw_used, sw_total // 50 + (16 << 20))

# --- Load and uptime ---------------------------------------------------------
# The kernel recomputes the averages every 5 s, and on a busy host the
# 1-minute one moves by more than the tolerance from one to the next
# (0.59 against 0.78 on a CI runner): uptime is read just before and
# just after a sample taken between the two, and budyk's average is held
# to the nearer.
def loadavg():
    m = re.search(r"load averages?: ([\d.]+),? ([\d.]+),? ([\d.]+)", run("uptime"))
    return [float(x) for x in m.groups()]
before = loadavg()
t_before = time.time()
deadline = t_before + 5
while True:
    ls, _ = newest()
    if ls["ts"] / 1e9 > t_before or time.time() > deadline:
        break
    time.sleep(0.1)
after = loadavg()
for i, (name, key) in enumerate((("1m", "avg_1m"), ("5m", "avg_5m"), ("15m", "avg_15m"))):
    got = ls["load"][key]
    want = min((before[i], after[i]), key=lambda w: abs(w - got))
    check("load " + name, got, want, max(0.15, want * 0.15))
age = time.time() - s["ts"] / 1e9
if FREEBSD:
    boot = int(re.search(r"sec = (\d+)", sysctl("kern.boottime")).group(1))
    up = time.time() - boot
else:
    up = float(open("/proc/uptime").read().split()[0])
check("uptime (s)", s["uptime_seconds"] + age, up, 2.0)

# --- Processes ---------------------------------------------------------------
nproc = len(run("ps", "-A", "-o", "pid=").split())
nthr = len(run("ps", "-A", "-H" if FREEBSD else "-L", "-o", "pid=").split())
check("processes", s["proc"]["total"], nproc, max(5, nproc // 20))
check("threads", s["proc"]["threads"], nthr, max(10, nthr // 20))

# --- TCP ---------------------------------------------------------------------
if FREEBSD:
    est = sum(1 for l in run("netstat", "-an", "-p", "tcp").splitlines()
              if l.rstrip().endswith("ESTABLISHED"))
elif shutil.which("ss"):
    est = len(run("ss", "-H", "-t", "-n", "state", "established").splitlines()) + \
          len(run("ss", "-H", "-t", "-n", "state", "close-wait").splitlines())
else:
    est = None
    ok("TCP established: no ss(8) here, not checked")
if est is not None:
    check("TCP established", s["tcp"]["established"], est, 3)

# --- Filesystems -------------------------------------------------------------
dfmounts = {l.split()[-1] for l in run("df", "-P", "-k").splitlines()[1:]}
for fs in d["filesystems"]:
    if fs["mount"] not in dfmounts:
        bad("filesystem %s is not in df -P" % fs["mount"])
        continue
    f = run("df", "-P", "-k", fs["mount"]).splitlines()[1].split()
    blocks, used = int(f[1]) * 1024, int(f[2]) * 1024
    check("%s total" % fs["mount"], fs["total_bytes"], blocks, blocks // 1000 + 1024)
    check("%s used" % fs["mount"], fs["used_bytes"], used, blocks // 100 + (64 << 20))

# --- Interfaces and disks ----------------------------------------------------
ifs = {i["name"] for i in d["interfaces"]}
if FREEBSD:
    sys_ifs = {n for n in run("ifconfig", "-l").split() if not n.startswith("lo")}
else:
    sys_ifs = set(os.listdir("/sys/class/net")) - {"lo"}
if ifs == sys_ifs:
    ok("interfaces: %s" % " ".join(sorted(ifs)))
else:
    bad("interfaces: budyk %s, the system %s" % (sorted(ifs), sorted(sys_ifs)))
if not FREEBSD:
    # The network totals are the devices' (/sys/class/net/<if>/device),
    # less one enslaved to another device (a VF under hv_netvsc's eth0):
    # a veth's or a bridge's traffic crossed a device too. With none,
    # every interface's. Same tick, so the per-interface rates add up
    # to the total, give or take a byte each for the rounding.
    def device(n):
        return os.path.exists("/sys/class/net/%s/device" % n)
    def counts(n):
        m = "/sys/class/net/%s/master" % n
        return device(n) and not (os.path.islink(m) and device(os.path.basename(os.readlink(m))))
    devs = [i for i in d["interfaces"] if counts(i["name"])]
    counted = devs if devs else d["interfaces"]
    for k in ("rx_bytes_per_sec", "tx_bytes_per_sec"):
        check("net %s = the devices' (%s)" % (k, " ".join(sorted(i["name"] for i in counted))),
              s["net"][k], sum(i[k] for i in counted), len(d["interfaces"]))
disks = {x["name"] for x in d["disks"]}
if FREEBSD:
    sys_disks = {n for n in sysctl("kern.disks").split() if not n.startswith("cd")}
    disks_cmp = {n for n in disks if not n.startswith("md")}
else:
    deny = ("loop", "ram", "zram", "dm-", "md", "fd", "sr", "nbd")
    sys_disks = {n for n in os.listdir("/sys/block") if not n.startswith(deny)}
    disks_cmp = disks
if disks_cmp == sys_disks:
    ok("disks: %s" % " ".join(sorted(disks)))
else:
    bad("disks: budyk %s, the system %s" % (sorted(disks), sorted(sys_disks)))

# --- A busy loop: one CPU ----------------------------------------------------
busy = subprocess.Popen([sys.executable, "-c", "while True: pass"])
time.sleep(3.5)
s, d = newest()
mine = [p for p in d["processes"] if p["pid"] == busy.pid]
busy.kill()
busy.wait()
if not mine:
    bad("the busy loop (pid %d) is not among the top processes" % busy.pid)
else:
    check("busy loop CPU %", mine[0]["cpu_percent"], 100.0, 25.0)
share = 100.0 / s["cpu"]["count"]
if s["cpu"]["total_percent"] >= 0.75 * share:
    ok("CPU total %.1f %% with one CPU busy (at least %.1f %%)"
       % (s["cpu"]["total_percent"], 0.75 * share))
else:
    bad("CPU total %.1f %% with one CPU busy, less than %.1f %%"
        % (s["cpu"]["total_percent"], 0.75 * share))

# --- A write: 64 MB to disk --------------------------------------------------
size = 64 << 20
t0 = time.time()
with open(scratch, "wb") as f:
    for _ in range(64):
        f.write(os.urandom(1 << 20))
    f.flush()
    os.fsync(f.fileno())
t1 = time.time()
time.sleep(2.5)
hist = json.loads(urllib.request.urlopen(
    "http://127.0.0.1:%d/api/samples" % port, timeout=10).read())["samples"]
os.unlink(scratch)
# Each sample's rate covers the second before it: from the one after the
# write began to the first one more than a second after it ended.
span = [x for x in hist if t0 < x["ts"] / 1e9 <= t1 + 2.0]
written, prev = 0.0, None
for x in hist:
    if prev is not None and x in span:
        written += x["disk"]["write_bytes_per_sec"] * (x["ts"] - prev["ts"]) / 1e9
    prev = x
if written >= 0.9 * size:
    ok("a 64 MB write: budyk saw %.0f MB written" % (written / (1 << 20)))
else:
    bad("a 64 MB write: budyk saw %.0f MB written" % (written / (1 << 20)))

sys.exit(1 if fail else 0)
EOF

kill -TERM "$PID"
wait "$PID" 2>/dev/null || true
PID=
if grep -q "ERROR: AddressSanitizer\|runtime error:" "$DIR/log"; then
    echo "FAIL: sanitizer report"; cat "$DIR/log"; fail=1
fi
[ "$fail" -eq 0 ] && echo "crosscheck smoke test: PASS" || { echo "crosscheck smoke test: FAIL"; exit 1; }
