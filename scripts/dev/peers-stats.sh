#!/bin/sh
# Sanity test for per-CPU peer statistics: creates many peers, prints the
# per-CPU allocator footprint, then removes the device and checks that the
# memory comes back.
set -eu
umask 077

if [ "$(id -u)" -ne 0 ]; then
	echo "run as root" >&2
	exit 1
fi

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
MOD=${UWG_MODULE:-$ROOT/src/umbrawg.ko}
TOOL="$ROOT/scripts/dev/uwg"
NS="uw-stats-$$"
TMP=$(mktemp -d)
LOADED=0
PEERS=${PEERS:-200}

cleanup() {
	ip netns del "$NS" 2>/dev/null || true
	rm -rf "$TMP"
	[ "$LOADED" -eq 1 ] && rmmod umbrawg 2>/dev/null || true
}
trap cleanup EXIT INT TERM

for mod in udp_tunnel ip6_udp_tunnel libcurve25519; do
	modprobe "$mod" 2>/dev/null || true
done
insmod "$MOD"
LOADED=1

percpu() { awk '/^Percpu:/ { print $2 }' /proc/meminfo; }

ip netns add "$NS"
ip -n "$NS" link add uw0 type umbrawg
"$TOOL" genkey >"$TMP/k"
ip netns exec "$NS" "$TOOL" set uw0 private-key "$TMP/k"

sleep 1
before=$(percpu)

i=0
while [ "$i" -lt "$PEERS" ]; do
	"$TOOL" genkey | "$TOOL" pubkey >"$TMP/p"
	ip netns exec "$NS" "$TOOL" set uw0 peer "$(cat "$TMP/p")" \
		allowed-ips "10.$((i / 256)).$((i % 256)).1/32"
	i=$((i + 1))
done

sleep 1
after=$(percpu)
count=$(ip netns exec "$NS" "$TOOL" show uw0 | grep -c 'peer:' || true)
echo "peers=$count percpu_delta_kb=$((after - before))"

ip -n "$NS" link del uw0
ip netns del "$NS"
sleep 2
echo "percpu_after_free_kb=$(percpu) before_kb=$before"
