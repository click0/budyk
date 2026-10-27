#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
#
# The network totals are the devices' traffic. A link into another
# network stack carries a ping flood, as a container's or a jail's would:
# the totals must leave it out (on a container or jail host that traffic
# also crosses the bridge and the uplink, each of which counted it again).
#   Linux:   a veth pair into a network namespace.
#   FreeBSD: an epair into a VNET jail, the host's end renamed e0a_<jail>
#            as jib(8) and Bastille name it (its driver is still epair's).
# The newest sample's totals must equal the sum of its interfaces that
# are a device's (Linux: /sys/class/net/<if>/device, less one enslaved to
# another device; FreeBSD: Ethernet or 802.11, the driver no cloner's),
# while the link moves hundreds of KB/s and is not one of them.
# Needs root or sudo without a password, and ping(8) plus ip(8) on Linux,
# jail(8) with VNET on FreeBSD; anywhere else it says so and passes.
#
#   tests/smoke/net_totals.sh path/to/budyk [port]

set -eu
BIN=${1:?usage: net_totals.sh path/to/budyk [port]}
PORT=${2:-18773}

skip() { echo "net_totals smoke test: skipped ($1)"; exit 0; }
OS=$(uname -s)
[ "$OS" = Linux ] || [ "$OS" = FreeBSD ] || skip "not Linux or FreeBSD"
SUDO=
if [ "$(id -u)" -ne 0 ]; then
    if command -v sudo > /dev/null && sudo -n true 2> /dev/null; then SUDO=sudo; else skip "needs root or sudo"; fi
fi
command -v ping > /dev/null || skip "needs ping"

DIR=$(mktemp -d)
NS=budyk$$
PID=
PING=
LINK=
cleanup() {
    kill "$PID" 2> /dev/null || true
    [ -n "$PING" ] && $SUDO kill "$PING" 2> /dev/null || true
    if [ "$OS" = Linux ]; then
        [ -n "$LINK" ] && $SUDO ip link del "$LINK" 2> /dev/null || true
        $SUDO ip netns del "$NS" 2> /dev/null || true
    else
        $SUDO jail -r "$NS" 2> /dev/null || true
        [ -n "$LINK" ] && $SUDO ifconfig "$LINK" destroy 2> /dev/null || true
    fi
    rm -rf "$DIR"
}
trap cleanup EXIT

if [ "$OS" = Linux ]; then
    command -v ip > /dev/null || skip "needs ip"
    LINK=bk$$a
    $SUDO ip netns add "$NS"
    $SUDO ip link add "$LINK" type veth peer name bk$$b
    $SUDO ip link set bk$$b netns "$NS"
    $SUDO ip addr add 10.213.7.1/30 dev "$LINK"
    $SUDO ip link set "$LINK" up
    $SUDO ip netns exec "$NS" ip addr add 10.213.7.2/30 dev bk$$b
    $SUDO ip netns exec "$NS" ip link set bk$$b up
else
    command -v jail > /dev/null || skip "needs jail"
    EPA=$($SUDO ifconfig epair create 2> /dev/null) || skip "no epair(4)"
    EPB=${EPA%a}b
    LINK=e0a_bk$$
    $SUDO ifconfig "$EPA" name "$LINK" > /dev/null
    $SUDO jail -c name="$NS" vnet persist vnet.interface="$EPB" > /dev/null 2>&1 ||
        skip "no VNET jails"
    $SUDO jexec "$NS" ifconfig "$EPB" inet 10.213.7.2/30 up
    $SUDO ifconfig "$LINK" inet 10.213.7.1/30 up
fi

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

python3 - "$PORT" "$LINK" "$OS" <<'EOF'
import base64, json, os, re, socket, subprocess, sys, time, urllib.request

port, link, system = int(sys.argv[1]), sys.argv[2], sys.argv[3]

def linux_counts(n):         # a device, not enslaved to another (a VF under eth0)
    def device(x):
        return os.path.exists("/sys/class/net/%s/device" % x)
    m = "/sys/class/net/%s/master" % n
    return device(n) and not (os.path.islink(m) and device(os.path.basename(os.readlink(m))))

cloners = []
if system == "FreeBSD":
    cloners = subprocess.run(["ifconfig", "-C"], capture_output=True, text=True).stdout.split()

def freebsd_counts(n):       # Ethernet or 802.11, the driver no cloner's (wlan aside)
    out = subprocess.run(["ifconfig", "-D", n], capture_output=True, text=True).stdout
    m = re.search(r"drivername: (\S+)", out)
    driver = re.match(r"[a-z]*", m.group(1) if m else n).group(0)
    if not (re.search(r"^\s+ether ", out, re.M) or "802.11" in out):
        return False
    return driver == "wlan" or (driver != "ngeth" and driver not in cloners)

counts = linux_counts if system == "Linux" else freebsd_counts

ws = socket.create_connection(("127.0.0.1", port))       # holds L3
ws.send(("GET /api/ws HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\n"
         "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\n"
         "Sec-WebSocket-Version: 13\r\n\r\n" % base64.b64encode(os.urandom(16)).decode()).encode())
deadline = time.time() + 10
while time.time() < deadline:
    d = json.loads(urllib.request.urlopen("http://127.0.0.1:%d/api/samples" % port, timeout=5).read())
    s, det = d["samples"][-1], d.get("detail", {})
    ifaces = det.get("interfaces", [])
    v = [i for i in ifaces if i["name"] == link]
    if "net" in s["groups"] and v and v[0]["rx_bytes_per_sec"] > 20000:
        devs = [i for i in ifaces if counts(i["name"])]
        counted = devs if devs else ifaces
        fail = False
        if devs and link in [i["name"] for i in devs]:
            print("FAIL: %s is taken for a device's interface" % link)
            fail = True
        for k in ("rx_bytes_per_sec", "tx_bytes_per_sec"):
            want = sum(i[k] for i in counted)
            if abs(s["net"][k] - want) <= len(ifaces):
                print("ok   net %s %d = the devices' (%s), %s moving %d" %
                      (k, s["net"][k], " ".join(i["name"] for i in counted), link, v[0][k]))
            else:
                print("FAIL: net %s %d, the devices' (%s) %d, %s %d" %
                      (k, s["net"][k], " ".join(i["name"] for i in counted), want, link, v[0][k]))
                fail = True
        sys.exit(1 if fail else 0)
    time.sleep(0.5)
print("FAIL: no sample with %s moving within 10 s" % link)
sys.exit(1)
EOF
echo "net_totals smoke test: PASS"
