#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
#
# Linux: the network totals are the devices' traffic. A veth pair into a
# network namespace carries a ping flood, as a container's veth would:
# the totals must leave it out (on a container host that traffic also
# crosses the bridge and the uplink, each of which counted it again).
# The newest sample's totals must equal the sum of its interfaces that
# have /sys/class/net/<if>/device (less one enslaved to another device),
# while the veth moves hundreds of KB/s.
# Needs root or sudo without a password, ip(8) and ping(8); anywhere
# else it says so and passes.
#
#   tests/smoke/net_totals.sh path/to/budyk [port]

set -eu
BIN=${1:?usage: net_totals.sh path/to/budyk [port]}
PORT=${2:-18773}

skip() { echo "net_totals smoke test: skipped ($1)"; exit 0; }
[ "$(uname -s)" = "Linux" ] || skip "not Linux"
SUDO=
if [ "$(id -u)" -ne 0 ]; then
    if command -v sudo > /dev/null && sudo -n true 2> /dev/null; then SUDO=sudo; else skip "needs root or sudo"; fi
fi
command -v ip > /dev/null && command -v ping > /dev/null || skip "needs ip and ping"

DIR=$(mktemp -d)
NS=budyk$$
VETH=bk$$a
PID=
PING=
cleanup() {
    kill "$PID" 2> /dev/null || true
    [ -n "$PING" ] && $SUDO kill "$PING" 2> /dev/null || true
    $SUDO ip link del "$VETH" 2> /dev/null || true
    $SUDO ip netns del "$NS" 2> /dev/null || true
    rm -rf "$DIR"
}
trap cleanup EXIT

$SUDO ip netns add "$NS"
$SUDO ip link add "$VETH" type veth peer name bk$$b
$SUDO ip link set bk$$b netns "$NS"
$SUDO ip addr add 10.213.7.1/30 dev "$VETH"
$SUDO ip link set "$VETH" up
$SUDO ip netns exec "$NS" ip addr add 10.213.7.2/30 dev bk$$b
$SUDO ip netns exec "$NS" ip link set bk$$b up

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

# About 300 KB/s each way for 12 s.
$SUDO ping -q -i 0.005 -s 1400 -c 2400 10.213.7.2 > /dev/null 2>&1 &
PING=$!

python3 - "$PORT" "$VETH" <<'EOF'
import base64, json, os, socket, sys, time, urllib.request

port, veth = int(sys.argv[1]), sys.argv[2]
ws = socket.create_connection(("127.0.0.1", port))       # holds L3
ws.send(("GET /api/ws HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\n"
         "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\n"
         "Sec-WebSocket-Version: 13\r\n\r\n" % base64.b64encode(os.urandom(16)).decode()).encode())
deadline = time.time() + 10
while time.time() < deadline:
    d = json.loads(urllib.request.urlopen("http://127.0.0.1:%d/api/samples" % port, timeout=5).read())
    s, det = d["samples"][-1], d.get("detail", {})
    ifaces = det.get("interfaces", [])
    v = [i for i in ifaces if i["name"] == veth]
    if "net" in s["groups"] and v and v[0]["rx_bytes_per_sec"] > 20000:
        def device(n):
            return os.path.exists("/sys/class/net/%s/device" % n)
        def counts(n):           # a device, not enslaved to another (a VF under eth0)
            m = "/sys/class/net/%s/master" % n
            return device(n) and not (os.path.islink(m) and device(os.path.basename(os.readlink(m))))
        devs = [i for i in ifaces if counts(i["name"])]
        counted = devs if devs else ifaces
        fail = False
        for k in ("rx_bytes_per_sec", "tx_bytes_per_sec"):
            want = sum(i[k] for i in counted)
            if abs(s["net"][k] - want) <= len(ifaces):
                print("ok   net %s %d = the devices' (%s), %s moving %d" %
                      (k, s["net"][k], " ".join(i["name"] for i in counted), veth, v[0][k]))
            else:
                print("FAIL: net %s %d, the devices' (%s) %d, %s %d" %
                      (k, s["net"][k], " ".join(i["name"] for i in counted), want, veth, v[0][k]))
                fail = True
        sys.exit(1 if fail else 0)
    time.sleep(0.5)
print("FAIL: no sample with the veth moving within 10 s")
sys.exit(1)
EOF
echo "net_totals smoke test: PASS"
