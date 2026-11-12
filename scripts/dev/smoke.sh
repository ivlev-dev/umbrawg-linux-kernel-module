#!/bin/sh
# Load the development module and verify it in a private network namespace.
# Never touches the host network configuration, the live umbrawg module or
# any existing interface.
set -eu
umask 077

if [ "$(id -u)" -ne 0 ]; then
	echo "run as root: sudo $0" >&2
	exit 1
fi

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
MOD="$ROOT/src/umbrawg.ko"
TOOL="$ROOT/scripts/dev/uwg"
NS="uw-smoke-$$"
TMP=$(mktemp -d)
LOADED=0

say() { printf '\033[1;32m==>\033[0m %s\n' "$*"; }
fail() { printf '\033[1;31mFAIL:\033[0m %s\n' "$*" >&2; exit 1; }

cleanup() {
	ip netns del "$NS" 2>/dev/null || true
	rm -rf "$TMP"
	if [ "$LOADED" -eq 1 ]; then
		rmmod umbrawg 2>/dev/null || true
	fi
}
trap cleanup EXIT INT TERM

[ -f "$MOD" ] || fail "module not built: $MOD"
[ -x "$TOOL" ] || fail "patched tool missing, run scripts/dev/make-uwg.sh"

say "dependencies"
for mod in udp_tunnel ip6_udp_tunnel libcurve25519; do
	modprobe "$mod" 2>/dev/null || true
done

if grep -q '^umbrawg ' /proc/modules; then
	say "module already loaded"
else
	say "loading $MOD"
	insmod "$MOD"
	LOADED=1
fi

grep -q '^umbrawg ' /proc/modules || fail "module did not load"

say "creating netns $NS"
ip netns add "$NS"

say "creating umbrawg interface"
ip -n "$NS" link add uw0 type umbrawg || fail "ip link add type umbrawg failed"
ip -n "$NS" -d link show uw0

say "configuring via genetlink family umbrawg"
"$TOOL" genkey >"$TMP/priv"
"$TOOL" pubkey <"$TMP/priv" >"$TMP/pub"
PORT=$((30000 + ($$ % 20000)))
ip netns exec "$NS" "$TOOL" set uw0 private-key "$TMP/priv" listen-port "$PORT"
ip netns exec "$NS" "$TOOL" show uw0

say "bringing interface up"
ip -n "$NS" link set uw0 up
ip -n "$NS" -d link show uw0 | grep -q 'umbrawg' || fail "unexpected devtype"

ip netns exec "$NS" "$TOOL" show uw0 | grep -q "listening port: $PORT" || fail "listen port not applied"

say "PASS: module coexists with umbrawg, genl and rtnl work"
