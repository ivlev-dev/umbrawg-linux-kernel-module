#!/bin/sh
# Control measurement: plain veth, no tunnel, same netns topology and CPU
# pinning as bench.sh.
# usage: plain.sh <cpu> [tcp1420|udp1320|udp64] [seconds] [reps]
set -eu
umask 077

if [ "$(id -u)" -ne 0 ]; then
	echo "run as root: sudo $0 ..." >&2
	exit 1
fi

CPU=${1:?cpu}
CASE=${2:-tcp1420}
DUR=${3:-10}
REPS=${4:-3}

N1="uwp-$$-1"
N2="uwp-$$-2"
TMP=$(mktemp -d)
SPID=

cleanup() {
	[ -n "$SPID" ] && kill "$SPID" 2>/dev/null || true
	ip netns del "$N1" 2>/dev/null || true
	ip netns del "$N2" 2>/dev/null || true
	rm -rf "$TMP"
}
trap cleanup EXIT INT TERM

ip netns add "$N1"
ip netns add "$N2"
ip -n "$N1" link add uwv0 type veth peer name uwv1
ip -n "$N1" link set uwv1 netns "$N2"
ip -n "$N1" addr add 10.99.0.1/24 dev uwv0
ip -n "$N2" addr add 10.99.0.2/24 dev uwv1
ip -n "$N1" link set uwv0 mtu 1420 up
ip -n "$N2" link set uwv1 mtu 1420 up
ip -n "$N1" link set lo up
ip -n "$N2" link set lo up

cpu_idle() { awk -v c="cpu$CPU" '$1 == c { print $5 + $6 }' /proc/stat; }
cpu_total() { awk -v c="cpu$CPU" '$1 == c { s = 0; for (i = 2; i <= NF; i++) s += $i; print s }' /proc/stat; }

: >"$TMP/result"
rep=1
while [ "$rep" -le "$REPS" ]; do
	ip netns exec "$N2" taskset -c "$CPU" iperf3 -s -1 -B 10.99.0.2 -p 5302 \
		>/dev/null 2>&1 &
	SPID=$!
	sleep 0.5
	idle0=$(cpu_idle)
	total0=$(cpu_total)
	case "$CASE" in
	tcp1420)
		ip netns exec "$N1" taskset -c "$CPU" iperf3 -c 10.99.0.2 \
			-p 5302 -t "$DUR" -O 2 -J >"$TMP/out.json"
		bps=$(jq -r '.end.sum_received.bits_per_second' "$TMP/out.json")
		len=1420
		;;
	udp1320|udp64)
		if [ "$CASE" = udp1320 ]; then len=1320; else len=64; fi
		ip netns exec "$N1" taskset -c "$CPU" iperf3 -c 10.99.0.2 \
			-p 5302 -u -b 0 -l "$len" -t "$DUR" -O 2 -J >"$TMP/out.json"
		bps=$(jq -r '.end.sum.bits_per_second' "$TMP/out.json")
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
	echo "$rep $bps $pps $busy" >>"$TMP/result"
	rep=$((rep + 1))
	sleep 0.3
done

awk -v cname="$CASE" -v cpu="$CPU" '
{ b[NR] = $2; p[NR] = $3; u[NR] = $4 }
END {
	n = NR
	for (i = 1; i <= n; i++)
		for (j = i + 1; j <= n; j++)
			if (b[i] > b[j]) {
				t = b[i]; b[i] = b[j]; b[j] = t
				t = p[i]; p[i] = p[j]; p[j] = t
				t = u[i]; u[i] = u[j]; u[j] = t
			}
	m = int((n + 1) / 2)
	printf "plain\t%s\t%s\t%.3f\t%.0f\t%.1f\n", cname, cpu, b[m] / 1e9, p[m], u[m]
}' "$TMP/result"
