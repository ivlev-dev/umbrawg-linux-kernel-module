#!/bin/sh
# Throughput measurement for umbrawg. Everything runs inside two private
# network namespaces; the host network configuration is never touched.
#
# usage: bench.sh <umbrawg> <cpu> <tcp1420|udp1320|udp64> [sec] [reps]
# env:   FLOWS=12                 parallel iperf3 streams
#        UWG_DEV_OPTS="s1 50 ..." extra device options for both sides
#        UWG_HP_KEY="<base64>"    header protection key for both sides
set -eu
umask 077

if [ "$(id -u)" -ne 0 ]; then
	echo "run as root: sudo $0 ..." >&2
	exit 1
fi

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
MODE=${1:?mode}
CPU=${2:?cpu}
CASE=${3:?case}
DUR=${4:-10}
REPS=${5:-3}
FLOWS=${FLOWS:-1}
UWG_DEV_OPTS=${UWG_DEV_OPTS:-}
UWG_HP_KEY=${UWG_HP_KEY:-}
UWG_MODULE_OPTS=${UWG_MODULE_OPTS:-}
UWG_OPTS_FILE=${UWG_OPTS_FILE:-}
UWG_UDP_BITRATE=${UWG_UDP_BITRATE:-0}

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

N1="uwb-$$-1"
N2="uwb-$$-2"
TMP=$(mktemp -d)
LOADED=0
SPID=

cleanup() {
	[ -n "$SPID" ] && kill "$SPID" 2>/dev/null || true
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

cpu_idle() {
	awk -v c="cpu$CPU" '$1 == c { print $5 + $6 }' /proc/stat
}
cpu_total() {
	awk -v c="cpu$CPU" '$1 == c { s = 0; for (i = 2; i <= NF; i++) s += $i; print s }' /proc/stat
}

PAR=""
[ "$FLOWS" -gt 1 ] && PAR="-P $FLOWS"

RESULT="$TMP/result"
: >"$RESULT"

rep=1
while [ "$rep" -le "$REPS" ]; do
	ip netns exec "$N2" taskset -c "$CPU" iperf3 -s -1 -B 192.168.199.2 -p 5301 \
		>/dev/null 2>&1 &
	SPID=$!
	sleep 0.5

	idle0=$(cpu_idle)
	total0=$(cpu_total)
	loss=0

	case "$CASE" in
	tcp1420)
		ip netns exec "$N1" taskset -c "$CPU" iperf3 -c 192.168.199.2 \
			-p 5301 $PAR -t "$DUR" -O 2 -J >"$TMP/out.json"
		bps=$(jq -r '.end.sum_received.bits_per_second' "$TMP/out.json")
		len=1420
		;;
	udp1320|udp1340|udp64)
		case "$CASE" in
		udp1320) len=1320 ;;
		udp1340) len=1340 ;;
		*) len=64 ;;
		esac
		RATE="-b 0"
		if [ "$UWG_UDP_BITRATE" != 0 ]; then
			per=$(awk -v r="$UWG_UDP_BITRATE" -v f="$FLOWS" \
				'BEGIN { printf "%.0f", r / f }')
			RATE="-b ${per}M"
		fi
		ip netns exec "$N1" taskset -c "$CPU" iperf3 -c 192.168.199.2 \
			-p 5301 $PAR -u $RATE -l "$len" -t "$DUR" -O 2 -J >"$TMP/out.json"
		bps=$(jq -r '.end.sum.bits_per_second' "$TMP/out.json")
		loss=$(jq -r '.end.sum.lost_percent // 0' "$TMP/out.json")
		;;
	*)
		echo "unknown case: $CASE" >&2
		exit 1
		;;
	esac

	idle1=$(cpu_idle)
	total1=$(cpu_total)

	wait "$SPID" 2>/dev/null || true
	SPID=

	pps=$(awk -v b="$bps" -v l="$len" 'BEGIN { printf "%.0f", b / (l * 8) }')
	busy=$(awk -v i0="$idle0" -v i1="$idle1" -v t0="$total0" -v t1="$total1" \
		'BEGIN { d = t1 - t0; printf "%.1f", (d > 0 ? 100 * (1 - (i1 - i0) / d) : 0) }')
	echo "$rep $bps $pps $busy $loss" >>"$RESULT"
	rep=$((rep + 1))
	sleep 0.3
done

if [ -n "${UWG_STATS:-}" ]; then
	ip netns exec "$N1" "$TOOL" show uw0 | sed -n '/peer/,/^$/p' | head -12
fi

awk -v mode="$LABEL" -v cname="$CASE" -v cpu="$CPU" '
{ b[NR] = $2; p[NR] = $3; u[NR] = $4; l[NR] = $5 }
END {
	n = NR
	for (i = 1; i <= n; i++)
		for (j = i + 1; j <= n; j++)
			if (b[i] > b[j]) {
				t = b[i]; b[i] = b[j]; b[j] = t
				t = p[i]; p[i] = p[j]; p[j] = t
				t = u[i]; u[i] = u[j]; u[j] = t
				t = l[i]; l[i] = l[j]; l[j] = t
			}
	m = int((n + 1) / 2)
	printf "%s\t%s\t%s\t%.3f\t%.0f\t%.1f\tlost %.2f%%\n", mode, cname, cpu, b[m] / 1e9, p[m], u[m], l[m]
}' "$RESULT"
