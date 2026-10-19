#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
#
# L2 -> L3 escalation on a sustained anomaly (spec §3.4.1). The CPU
# threshold is set below zero, so every sample is an anomaly: the daemon
# collects at L2 at first and, once the anomaly has held for
# escalate_after (3 s), at L3, and says so in its log. No client is
# connected: the levels are read through /api/range, which (unlike
# /api/samples) does not count as one.
#
#   tests/smoke/escalate.sh path/to/budyk [port]

set -eu
# Tenths of a second to wait for the daemon to answer: SMOKE_START_WAIT
# seconds (default 5); an emulated VM needs more.
START_TRIES=$(( ${SMOKE_START_WAIT:-5} * 10 ))
BIN=${1:?usage: escalate.sh path/to/budyk [port]}
PORT=${2:-18768}
DIR=$(mktemp -d)
PID=
trap 'kill "$PID" 2>/dev/null || true; rm -rf "$DIR"' EXIT

cat > "$DIR/config.yaml" <<EOF
data_dir: $DIR
listen: 127.0.0.1
port: $PORT
collection:
  l1: { interval: 30 }
  l2:
    interval: 1
    escalate_after: 3
    escalation_thresholds: { cpu_percent: -1 }
  l3: { interval: 1, grace_period: 2 }
EOF

"$BIN" serve --config "$DIR/config.yaml" > "$DIR/log" 2>&1 &
PID=$!
i=0
until curl -s -o /dev/null "http://127.0.0.1:$PORT/api/health"; do
    i=$((i + 1))
    [ "$i" -lt "$START_TRIES" ] || { echo "daemon did not start"; cat "$DIR/log"; exit 1; }
    sleep 0.1
done

fail=0
has_level() {   # has_level <level name>: the ring for that level has a record
    curl -s "http://127.0.0.1:$PORT/api/range?level=$1" | grep -q '"ts":'
}

# The anomaly is there from the first tick: L2, not yet L3.
sleep 1.5
if has_level L2 && ! has_level L3; then
    echo "ok   an anomaly starts at L2"
else
    echo "FAIL: expected L2 records and no L3 ones after 1.5 s"; fail=1
fi

# After escalate_after (3 s) of it: L3, and a log line saying why.
i=0
until has_level L3; do
    i=$((i + 1))
    [ "$i" -lt 60 ] || break
    sleep 0.1
done
if has_level L3; then
    echo "ok   the sustained anomaly escalated to L3"
else
    echo "FAIL: no L3 record 7.5 s into a sustained anomaly"; fail=1
fi
if grep -q "anomaly held for 3 s .*collecting at L3 until it clears" "$DIR/log"; then
    echo "ok   the escalation is logged"
else
    echo "FAIL: no escalation line in the log"; fail=1
fi

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
echo "escalate smoke test: PASS"
