#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
#
# Per-level metric sets and collection.mode (spec §3.4.4), in three runs:
#
#   1. adaptive, L1 collecting `minimal` and L3 a list: the L1 records
#      hold only the minimal groups, a rule that reads disk is skipped at
#      L1 (logged once) and fires at L3; /api/levels names each set.
#   2. always: L3 with no client, its samples in the L3 ring and folded
#      into the L1 one.
#   3. on-demand: nothing collected until a client polls, then samples in
#      the hot buffer but none in the rings; collection stops again once
#      the client is gone (poller window + grace_period).
#   4. SIGHUP between ticks reloads the rules and does not end the sleep.
#
# A rule that logs on every tick counts the ticks collected.
#
#   tests/smoke/modes.sh path/to/budyk [port]

set -eu
BIN=${1:?usage: modes.sh path/to/budyk [port]}
PORT=${2:-18770}
DIR=$(mktemp -d)
PID=
trap 'kill "$PID" 2>/dev/null || true; rm -rf "$DIR"' EXIT
URL="http://127.0.0.1:$PORT"
fail=0

start() {   # start <run>: the daemon on $DIR/<run>.yaml, log in $DIR/<run>.log
    "$BIN" serve --config "$DIR/$1.yaml" > "$DIR/$1.log" 2>&1 &
    PID=$!
    i=0
    until curl -s -o /dev/null "$URL/api/health"; do
        i=$((i + 1))
        [ "$i" -lt 50 ] || { echo "daemon did not start"; cat "$DIR/$1.log"; exit 1; }
        sleep 0.1
    done
}
stop() {    # stop <run>
    kill -TERM "$PID"
    if ! wait "$PID"; then
        echo "FAIL: daemon exited non-zero ($1)"; fail=1
    fi
    PID=
    if grep -qE 'ERROR: (Address|Leak)Sanitizer|ThreadSanitizer|runtime error:' "$DIR/$1.log"; then
        echo "FAIL: sanitizer report ($1)"; fail=1
    fi
}
check() {   # check <description> <python condition on doc> <url path>
    if curl -s "$URL$3" > "$DIR/check.json" &&
       python3 -c "import json, sys; doc = json.load(open(sys.argv[1])); sys.exit(0 if ($2) else 1)" \
           "$DIR/check.json"; then
        echo "ok   $1"
    else
        echo "FAIL: $1 (GET $3: $(cut -c1-600 "$DIR/check.json"))"; fail=1
    fi
}
ticks() { grep -c '^\[budyk\] tick$' "$DIR/$1.log" || true; }

cat > "$DIR/rules.lua" <<'EOF'
watch("tick", { when = function() return true end, action = "log", message = "tick" })
watch("io", { when = function() return disk.read_bytes_per_sec >= 0 end,
              action = "log", message = "io" })
EOF

# 1. adaptive with metric sets ---------------------------------------------
cat > "$DIR/sets.yaml" <<EOF
data_dir: $DIR/sets
listen: 127.0.0.1
port: $PORT
rules: { path: $DIR/rules.lua }
security: { file_watch: { enabled: true, paths: [$DIR/watched] } }
collection:
  l1: { interval: 1, metrics: minimal }
  l3: { interval: 1, grace_period: 1, metrics: [disk, net.interfaces] }
EOF
: > "$DIR/watched"
start sets
# Three L1 ticks (a loaded CI runner may stall a tick or two).
i=0
until [ "$(curl -s "$URL/api/range?level=L1" | grep -o '"ts":' | wc -l)" -ge 3 ]; do
    i=$((i + 1))
    [ "$i" -lt 150 ] || break
    sleep 0.1
done
MIN='["cpu", "mem", "swap", "load", "uptime"]'
check "/api/levels: L1 collects the minimal set" \
      "doc[0]['name'] == 'L1' and doc[0]['metrics'] == $MIN and doc[0]['collects']" /api/levels
check "/api/levels: L3 the list, with its groups" \
      "doc[2]['metrics'] == ['cpu', 'mem', 'swap', 'load', 'disk', 'net', 'uptime', 'net.interfaces']" \
      /api/levels
# file_watch, when enabled, is in every sample whatever the level.
MINFW='["cpu", "mem", "swap", "load", "file_watch", "uptime"]'
check "the L1 records hold the minimal groups (and file_watch) only" \
      "len(doc['samples']) >= 2 and all(set(s['groups']) <= set($MINFW) for s in doc['samples']) \
       and doc['samples'][-1]['groups'] == $MINFW" "/api/range?level=L1"
n=$(grep -c "rule 'io' skipped while the level does not collect what it reads: .*disk is not collected at this level" "$DIR/sets.log" || true)
if [ "$n" = 1 ] && ! grep -q '^\[budyk\] io$' "$DIR/sets.log"; then
    echo "ok   the rule reading disk is skipped at L1, logged once"
else
    echo "FAIL: expected one skip line and no fire at L1, got $n skip line(s)"; fail=1
fi
# A dashboard polls /api/samples and opens a WebSocket a moment later:
# two wake-ups, but no two ticks closer than the minimum gap (a rate over
# a few ms read 100% for a core).
python3 - "$PORT" <<'EOF2'
import base64, os, socket, sys, time, urllib.request
port = int(sys.argv[1])
urllib.request.urlopen("http://127.0.0.1:%d/api/samples" % port).read()
s = socket.create_connection(("127.0.0.1", port))
s.send(("GET /api/ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n"
        % base64.b64encode(os.urandom(16)).decode()).encode())
time.sleep(2)
s.close()
EOF2
ts=$(for l in L1 L3; do curl -s "$URL/api/range?level=$l"; echo; done)
if echo "$ts" | python3 -c '
import json, sys
t = sorted(int(s["ts"]) for line in sys.stdin if line.strip()
           for s in json.loads(line)["samples"] if not s.get("folded"))
gaps = [b - a for a, b in zip(t, t[1:])]
sys.exit(0 if len(t) >= 4 and min(gaps) >= 200000000 else ("gaps (ns): %s" % gaps))'; then
    echo "ok   two wake-ups at once: no ticks closer than the minimum gap"
else
    echo "FAIL: ticks too close together"; fail=1
fi
# The tick a client woke collected L3's set, not that of the L1 it slept
# at: its disk and net are a baseline.
check "the first L3 sample has L3's set, disk and net as a baseline" \
      "[s.get('baseline') for s in doc['samples'] if s['level'] == 3][:1] == [['disk', 'net']]" \
      /api/samples
# A poller holds L3; its second tick has disk (the first read is a baseline).
for _ in 1 2 3; do curl -s -o /dev/null "$URL/api/samples"; sleep 1; done
check "at L3 the samples have the L3 list's groups, the detail its list" \
      "doc['samples'][-1]['level'] == 3 and \
       doc['samples'][-1]['groups'] == ['cpu', 'mem', 'swap', 'load', 'disk', 'net', 'file_watch', 'uptime'] and \
       doc['detail']['collected'] == ['net.interfaces'] and doc['detail']['cores'] == []" /api/samples
if grep -q '^\[budyk\] io$' "$DIR/sets.log"; then
    echo "ok   the rule fires at L3"
else
    echo "FAIL: the rule reading disk never fired at L3"; fail=1
fi
stop sets

# 2. always -----------------------------------------------------------------
cat > "$DIR/always.yaml" <<EOF
data_dir: $DIR/always
listen: 127.0.0.1
port: $PORT
rules: { path: $DIR/rules.lua }
collection:
  mode: always
  l1: { interval: 2 }
  l3: { interval: 1 }
EOF
start always
i=0
until curl -s "$URL/api/range?level=L1" | grep -q '"folded":true'; do
    i=$((i + 1))
    [ "$i" -lt 60 ] || break
    sleep 0.1
done
check "always: L3 samples with no client" \
      "len(doc['samples']) >= 2 and all(s['level'] == 3 for s in doc['samples'])" "/api/range?level=L3"
check "always: folded into the L1 ring" \
      "len(doc['samples']) >= 1 and all(s.get('folded') for s in doc['samples'])" "/api/range?level=L1"
grep -q "collection.mode always: collecting at L3 all the time" "$DIR/always.log" ||
    { echo "FAIL: the mode is not logged"; fail=1; }
stop always

# 3. on-demand --------------------------------------------------------------
cat > "$DIR/ondemand.yaml" <<EOF
data_dir: $DIR/ondemand
listen: 127.0.0.1
port: $PORT
rules: { path: $DIR/rules.lua }
collection:
  mode: on-demand
  l1: { interval: 1 }
  l3: { interval: 1, grace_period: 1 }
EOF
start ondemand
sleep 2
if [ "$(ticks ondemand)" = 0 ]; then
    echo "ok   on-demand: nothing collected without a client"
else
    echo "FAIL: on-demand collected $(ticks ondemand) tick(s) with no client"; fail=1
fi
check "on-demand: L1 and L2 do not collect" \
      "[l['collects'] for l in doc[:3]] == [False, False, True]" /api/levels
# The poll wakes the loop as it is answered: a tick may just make it in.
check "on-demand: the first poll finds at most the tick it woke" "doc['count'] <= 1" /api/samples
for _ in 1 2 3; do sleep 1; curl -s -o /dev/null "$URL/api/samples"; done
check "on-demand: a poller gets L3 samples" \
      "doc['count'] >= 2 and doc['samples'][-1]['level'] == 3" /api/samples
check "on-demand: nothing in the rings" "doc['count'] == 0" "/api/range?level=all"
# The poller window (5 s) and grace_period (1 s) after the last poll.
sleep 8
before=$(ticks ondemand)
sleep 2.5
after=$(ticks ondemand)
if [ "$before" -ge 3 ] && [ "$before" = "$after" ]; then
    echo "ok   on-demand: collection stops once the client is gone ($before ticks)"
else
    echo "FAIL: on-demand ticks: $before, then $after 2.5 s later"; fail=1
fi
# Back after the gap: the first read is a baseline, not CPU, disk and net
# rates averaged over the idle minutes. This poll is answered with what
# the hot buffer held before it, and wakes the loop.
last_ts=$(curl -s "$URL/api/samples" | python3 -c 'import json, sys; s = json.load(sys.stdin)["samples"]; print(s[-1]["ts"] if s else 0)')
sleep 1.5
check "on-demand: the first sample after a gap is a baseline" \
      "[s for s in doc['samples'] if s['ts'] > $last_ts][0].get('baseline', []).count('cpu') == 1" /api/samples
grep -q "collection.mode on-demand: collecting only while a client is connected" "$DIR/ondemand.log" ||
    { echo "FAIL: the mode is not logged"; fail=1; }
stop ondemand

# 4. SIGHUP between ticks ---------------------------------------------------
# The rules are reloaded at once, and the sleep goes on: SIGHUP used to end
# it, and the tick right after the last one stored rates over a few ms.
cat > "$DIR/hup.yaml" <<EOF
data_dir: $DIR/hup
listen: 127.0.0.1
port: $PORT
rules: { path: $DIR/hup.lua }
collection:
  l1: { interval: 4 }
EOF
echo 'watch("tick", { when = function() return true end, action = "log", message = "tick", cooldown = 0 })' \
    > "$DIR/hup.lua"
start hup
sleep 1                                      # the first L1 tick is in
before=$(ticks hup)
kill -HUP "$PID"
sleep 1.5
after=$(ticks hup)
if grep -q 'rules reloaded' "$DIR/hup.log" && [ "$before" -ge 1 ] && [ "$after" = "$before" ]; then
    echo "ok   SIGHUP reloads the rules without an extra tick ($before tick(s))"
else
    echo "FAIL: SIGHUP: $before tick(s) before, $after 1.5 s after (or no reload)"; fail=1
fi
stop hup

if [ "$fail" -ne 0 ]; then
    for f in sets always ondemand hup; do echo "--- $f.log ---"; cat "$DIR/$f.log"; done
    exit 1
fi
echo "modes smoke test: PASS"
