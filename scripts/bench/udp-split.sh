#!/bin/sh
# UDP multi-flow throughput with one iperf3 server per flow, each server and
# client pinned to its own CPU. Needed because a single iperf3 UDP server is
# limited to roughly 400-500 kpps and hides the tunnel capacity.
#
# usage: udp-split.sh <umbrawg|ref> <cpu-list> <flows> [sec] [reps]
# env:   UWG_UDP_BITRATE (total Mbps, split per flow; 0 = unlimited),
#        UWG_MODULE, UWG_LABEL, UWG_OPTS_FILE, UWG_HP_KEY,
#        UWG_REF_TOOL, UWG_REF_KIND, UWG_REF_MODULE
set -eu
umask 077

if [ "$(id -u)" -ne 0 ]; then
	echo "run as root" >&2
	exit 1
fi

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
MODE=${1:?mode}
CPUS=${2:?cpu-list}
FLOWS=${3:?flows}
DUR=${4:-8}
REPS=${5:-2}
RATE=${UWG_UDP_BITRATE:-0}
LEN=1340
UWG_DEV_OPTS=${UWG_DEV_OPTS:-}
UWG_HP_KEY=${UWG_HP_KEY:-}
UWG_MODULE_OPTS=${UWG_MODULE_OPTS:-}
UWG_OPTS_FILE=${UWG_OPTS_FILE:-}

if [ "$MODE" = umbrawg ]; then
	TOOL="$ROOT/scripts/dev/uwg"
	KIND=umbrawg
	MODULE=umbrawg
	MODULE_PATH=${UWG_MODULE:-$ROOT/src/umbrawg.ko}
	REF=0
elif [ "$MODE" = ref ]; then
	TOOL=${UWG_REF_TOOL:?set UWG_REF_TOOL}
	KIND=${UWG_REF_KIND:?set UWG_REF_KIND}
	MODULE=${UWG_REF_MODULE:?set UWG_REF_MODULE}
	MODULE_PATH=
	REF=1
else
	echo "unknown mode: $MODE" >&2
	exit 1
fi

LABEL=${UWG_LABEL:-$MODE}
[ -x "$TOOL" ] || { echo "tool not found: $TOOL" >&2; exit 1; }

N1="uws-$$-1"
N2="uws-$$-2"
TMP=$(mktemp -d)
LOADED=0
PIDS=

cleanup() {
	for p in $PIDS; do kill "$p" 2>/dev/null || true; done
	if [ -n "${UWG_KEEP:-}" ]; then cp "$TMP"/out-*.json "$ROOT"/ 2>/dev/null || true; fi
	ip netns del "$N1" 2>/dev/null || true
	ip netns del "$N2" 2>/dev/null || true
	rm -rf "$TMP"
	[ "$LOADED" -eq 1 ] && rmmod "$MODULE" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

if ! grep -q "^$MODULE " /proc/modules; then
	if [ "$REF" -eq 0 ]; then
		for mod in udp_tunnel ip6_udp_tunnel libcurve25519; do
			modprobe "$mod" 2>/dev/null || true
		done
		insmod "$MODULE_PATH" $UWG_MODULE_OPTS
		LOADED=1
	else
		modprobe "$MODULE"
	fi
fi

ip netns add "$N1"
ip netns add "$N2"
ip -n "$N1" link add uwv0 type veth peer name uwv1
ip -n "$N1" link set uwv1 netns "$N2"
ip -n "$N1" addr add 10.99.0.1/24 dev uwv0
ip -n "$N2" addr add 10.99.0.2/24 dev uwv1
ip -n "$N1" link set uwv0 up
ip -n "$N2" link set uwv1 up
ip -n "$N1" link set lo up
ip -n "$N2" link set lo up
ip -n "$N1" link add uw0 type "$KIND"
ip -n "$N2" link add uw0 type "$KIND"

"$TOOL" genkey >"$TMP/k1"
"$TOOL" pubkey <"$TMP/k1" >"$TMP/p1"
"$TOOL" genkey >"$TMP/k2"
"$TOOL" pubkey <"$TMP/k2" >"$TMP/p2"
P1=$((20000 + ($$ % 10000)))
P2=$((35000 + ($$ % 10000)))

HP_FILE=
if [ -n "$UWG_HP_KEY" ]; then
	printf '%s\n' "$UWG_HP_KEY" >"$TMP/hp"
	HP_FILE="$TMP/hp"
fi

# shellcheck disable=SC2086
set -- private-key "$TMP/k1" listen-port "$P1" $UWG_DEV_OPTS
if [ -n "$UWG_OPTS_FILE" ]; then
	while IFS= read -r line; do
		[ -n "$line" ] && set -- "$@" "$line"
	done <"$UWG_OPTS_FILE"
fi
[ -n "$HP_FILE" ] && set -- "$@" header-protection-key "$HP_FILE"
ip netns exec "$N1" "$TOOL" set uw0 "$@"

# shellcheck disable=SC2086
set -- private-key "$TMP/k2" listen-port "$P2" $UWG_DEV_OPTS
if [ -n "$UWG_OPTS_FILE" ]; then
	while IFS= read -r line; do
		[ -n "$line" ] && set -- "$@" "$line"
	done <"$UWG_OPTS_FILE"
fi
[ -n "$HP_FILE" ] && set -- "$@" header-protection-key "$HP_FILE"
ip netns exec "$N2" "$TOOL" set uw0 "$@"

ip netns exec "$N1" "$TOOL" set uw0 peer "$(cat "$TMP/p2")" \
	allowed-ips 192.168.199.2/32 endpoint 10.99.0.2:"$P2"
ip netns exec "$N2" "$TOOL" set uw0 peer "$(cat "$TMP/p1")" \
	allowed-ips 192.168.199.1/32 endpoint 10.99.0.1:"$P1"
ip -n "$N1" link set uw0 up
ip -n "$N2" link set uw0 up
ip -n "$N1" addr add 192.168.199.1/32 dev uw0
ip -n "$N2" addr add 192.168.199.2/32 dev uw0
ip -n "$N1" route add 192.168.199.2/32 dev uw0
ip -n "$N2" route add 192.168.199.1/32 dev uw0

i=0
while [ "$i" -lt 10 ]; do
	ip netns exec "$N1" ping -c1 -W1 192.168.199.2 >/dev/null 2>&1 && break
	i=$((i + 1))
done

CPU_LIST=$(awk -v s="$CPUS" 'BEGIN {
	n = split(s, parts, ",")
	out = ""
	for (i = 1; i <= n; i++) {
		if (parts[i] ~ "-") {
			split(parts[i], r, "-")
			for (c = r[1] + 0; c <= r[2] + 0; c++) out = out " " c
		} else {
			out = out " " parts[i]
		}
	}
	print out
}')

NC=$(echo "$CPU_LIST" | wc -w)
if [ "$FLOWS" -gt "$NC" ]; then
	echo "flows ($FLOWS) exceed cpus ($NC)" >&2
	exit 1
fi

PER=0
[ "$RATE" != 0 ] && PER=$(awk -v r="$RATE" -v f="$FLOWS" 'BEGIN { printf "%.0f", r / f }')

RESULT="$TMP/result"
: >"$RESULT"

rep=1
while [ "$rep" -le "$REPS" ]; do
	i=1
	PORT=5301
	for cpu in $CPU_LIST; do
		[ "$i" -gt "$FLOWS" ] && break
		ip netns exec "$N2" taskset -c "$cpu" iperf3 -s -1 \
			-B 192.168.199.2 -p "$PORT" >/dev/null 2>&1 &
		PIDS="$PIDS $!"
		PORT=$((PORT + 1))
		i=$((i + 1))
	done
	sleep 0.7

	i=1
	PORT=5301
	CLIENT_PIDS=
	for cpu in $CPU_LIST; do
		[ "$i" -gt "$FLOWS" ] && break
		BR="-b 0"
		[ "$PER" -gt 0 ] && BR="-b ${PER}M"
		ip netns exec "$N1" taskset -c "$cpu" iperf3 -c 192.168.199.2 \
			-p "$PORT" -u $BR -l "$LEN" -t "$DUR" -O 2 -J \
			>"$TMP/out-$i.json" 2>/dev/null &
		CLIENT_PIDS="$CLIENT_PIDS $!"
		PORT=$((PORT + 1))
		i=$((i + 1))
	done
	for p in $CLIENT_PIDS; do wait "$p" 2>/dev/null || true; done
	PIDS=
	if [ -n "${UWG_KEEP:-}" ]; then
		i=1
		while [ "$i" -le "$FLOWS" ]; do
			echo "flow $i: $(jq -c '.end.sum | {bps:.bits_per_second,pk:.packets,lost:.lost_packets,lp:.lost_percent}' "$TMP/out-$i.json" 2>/dev/null)"
			i=$((i + 1))
		done
	fi

	i=1
	total=0
	lost=0
	sent=0
	while [ "$i" -le "$FLOWS" ]; do
		b=$(jq -r '.end.sum.bits_per_second // 0' "$TMP/out-$i.json")
		l=$(jq -r '.end.sum.lost_percent // 0' "$TMP/out-$i.json")
		pk=$(jq -r '.end.sum.packets // 0' "$TMP/out-$i.json")
		total=$(awk -v t="$total" -v b="$b" 'BEGIN { print t + b }')
		lost=$(awk -v t="$lost" -v l="$l" -v p="$pk" 'BEGIN { print t + l * p }')
		sent=$(awk -v t="$sent" -v p="$pk" 'BEGIN { print t + p }')
		i=$((i + 1))
	done
	echo "$rep $total $lost $sent" >>"$RESULT"
	rep=$((rep + 1))
	sleep 0.5
done

awk -v mode="$LABEL" -v cpu="$CPUS" -v flows="$FLOWS" '
{ b[NR] = $2; l[NR] = $3; p[NR] = $4 }
END {
	n = NR
	for (i = 1; i <= n; i++)
		for (j = i + 1; j <= n; j++)
			if (b[i] > b[j]) {
				t = b[i]; b[i] = b[j]; b[j] = t
				t = l[i]; l[i] = l[j]; l[j] = t
				t = p[i]; p[i] = p[j]; p[j] = t
			}
	m = int((n + 1) / 2)
	pct = (p[m] > 0 ? l[m] / p[m] : 0)
	pps = b[m] / (1340 * 8)
	printf "%s\tudp-split-%s\t%s\t%.3f\t%.0f\t-\tlost %.2f%%\n",
		mode, flows, cpu, b[m] / 1e9, pps, pct
}' "$RESULT"
