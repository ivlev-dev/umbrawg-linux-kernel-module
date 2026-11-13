#!/bin/sh
# Throughput benchmark of Hysteria 2 for comparison with umbrawg.
#
# The topology mirrors bench.sh: two private network namespaces connected by a
# veth pair. The Hysteria 2 server runs in N2 and the client in N1; the client
# exposes the server's iperf3 endpoint with tcpForwarding/udpForwarding, so
# iperf3 itself is unaware of the tunnel.
#
# usage: hysteria.sh <cpu> <tcp1420|udp1340> [sec] [reps]
# env:   FLOWS=12             parallel iperf3 streams
#        UWG_NETEM="loss 5%"  netem on the client-side veth
#        UWG_UDP_BITRATE=4000 total UDP rate in Mbit/s (0 = unlimited)
#        HY_CC=bbr|brutal     congestion controller
#        HY_BW="1 gbps"       bandwidth for brutal (both directions)
#        HY_BIN=hysteria      binary to run
#        UWG_LABEL=name       label in the result line
set -eu
umask 077

if [ "$(id -u)" -ne 0 ]; then
	echo "run as root: sudo $0 ..." >&2
	exit 1
fi

CPU=${1:?cpu}
CASE=${2:?case}
DUR=${3:-10}
REPS=${4:-3}
FLOWS=${FLOWS:-1}
UWG_NETEM=${UWG_NETEM:-}
UWG_UDP_BITRATE=${UWG_UDP_BITRATE:-0}
HY_CC=${HY_CC:-bbr}
HY_BW=${HY_BW:-}
HY_BIN=${HY_BIN:-hysteria}
LABEL=${UWG_LABEL:-hysteria2-$HY_CC}

command -v "$HY_BIN" >/dev/null 2>&1 || {
	echo "binary not found: $HY_BIN" >&2
	exit 1
}

N1="hyb-$$-1"
N2="hyb-$$-2"
TMP=$(mktemp -d)
SPID=
CPID=
RSSPID=

cleanup() {
	[ -n "$RSSPID" ] && kill "$RSSPID" 2>/dev/null || true
	[ -n "$CPID" ] && kill "$CPID" 2>/dev/null || true
	[ -n "$SPID" ] && kill "$SPID" 2>/dev/null || true
	sleep 0.2
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
ip -n "$N1" link set uwv0 up
ip -n "$N2" link set uwv1 up
ip -n "$N1" link set lo up
ip -n "$N2" link set lo up

${HY_BIN} cert --host localhost,10.99.0.2 \
	--cert "$TMP/cert.crt" --key "$TMP/cert.key" >/dev/null 2>&1

P1=$((20000 + ($$ % 10000)))
P2=$((35000 + ($$ % 10000)))

BW_BLOCK=
if [ -n "$HY_BW" ]; then
	BW_BLOCK="bandwidth:
  up: $HY_BW
  down: $HY_BW"
fi

cat >"$TMP/server.yaml" <<EOF
listen: 10.99.0.2:$P1
tls:
  cert: $TMP/cert.crt
  key: $TMP/cert.key
auth:
  type: password
  password: umbra-bench
$BW_BLOCK
quic:
  initStreamReceiveWindow: 8388608
  maxStreamReceiveWindow: 8388608
  initConnReceiveWindow: 20971520
  maxConnReceiveWindow: 20971520
  maxIdleTimeout: 30s
  keepAlivePeriod: 10s
EOF

cat >"$TMP/client.yaml" <<EOF
server: 10.99.0.2:$P1
auth: umbra-bench
tls:
  sni: localhost
  insecure: true
fastOpen: true
$BW_BLOCK
congestion:
  type: ${HY_CONGESTION:-bbr}
  bbrProfile: standard
quic:
  initStreamReceiveWindow: 8388608
  maxStreamReceiveWindow: 8388608
  initConnReceiveWindow: 20971520
  maxConnReceiveWindow: 20971520
  maxIdleTimeout: 30s
  keepAlivePeriod: 10s
tcpForwarding:
  - listen: 127.0.0.1:5301
    remote: 10.99.0.2:5301
udpForwarding:
  - listen: 127.0.0.1:5301
    remote: 10.99.0.2:5301
    timeout: 60s
EOF

if [ -n "$UWG_NETEM" ]; then
	case "$UWG_NETEM" in
	*" "*) set -- $UWG_NETEM ;;
	*) set -- loss "$UWG_NETEM" ;;
	esac
	ip netns exec "$N1" tc qdisc add dev uwv0 root netem "$@"
fi

ip netns exec "$N2" taskset -c "$CPU" "$HY_BIN" server -c "$TMP/server.yaml" \
	>"$TMP/server.log" 2>&1 &
SPID=$!
sleep 1
ip netns exec "$N1" taskset -c "$CPU" "$HY_BIN" client -c "$TMP/client.yaml" \
	>"$TMP/client.log" 2>&1 &
CPID=$!
sleep 1

ip netns exec "$N2" taskset -c "$CPU" iperf3 -s -B 10.99.0.2 -p 5301 \
	>/dev/null 2>&1 &
IPID=$!
sleep 0.5

rss_sample() {
	for pid in "$SPID" "$CPID"; do
		awk '/^VmRSS:/{print $2}' "/proc/$pid/status" 2>/dev/null || echo 0
	done | awk '{s += $1} END {print s + 0}'
}

i=0
while [ "$i" -lt 10 ]; do
	ip netns exec "$N1" taskset -c "$CPU" iperf3 -c 127.0.0.1 -p 5301 -t 1 \
		>/dev/null 2>&1 && break
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
	idle0=$(cpu_idle)
	total0=$(cpu_total)
	loss=0

	( max=0
	  while :; do
		v=$(rss_sample)
		[ "$v" -gt "$max" ] && max=$v
		echo "$max" >"$TMP/rss"
		sleep 0.2
	  done ) &
	RSSPID=$!

	case "$CASE" in
	tcp1420)
		ip netns exec "$N1" taskset -c "$CPU" iperf3 -c 127.0.0.1 \
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
		ip netns exec "$N1" taskset -c "$CPU" iperf3 -c 127.0.0.1 \
			-p 5301 $PAR -u $RATE -l "$len" -t "$DUR" -O 2 -J >"$TMP/out.json"
		bps=$(jq -r '.end.sum_received.bits_per_second // .end.sum.bits_per_second' "$TMP/out.json")
		loss=$(jq -r '.end.sum_received.lost_percent // .end.sum.lost_percent // 0' "$TMP/out.json")
		;;
	*)
		echo "unknown case: $CASE" >&2
		exit 1
		;;
	esac

	idle1=$(cpu_idle)
	total1=$(cpu_total)

	kill "$RSSPID" 2>/dev/null || true
	RSSPID=
	rss=$(cat "$TMP/rss" 2>/dev/null || echo 0)

	pps=$(awk -v b="$bps" -v l="$len" 'BEGIN { printf "%.0f", b / (l * 8) }')
	busy=$(awk -v i0="$idle0" -v i1="$idle1" -v t0="$total0" -v t1="$total1" \
		'BEGIN { d = t1 - t0; printf "%.1f", (d > 0 ? 100 * (1 - (i1 - i0) / d) : 0) }')
	echo "$rep $bps $pps $busy $loss $rss" >>"$RESULT"
	rep=$((rep + 1))
done

kill "$IPID" 2>/dev/null || true

if [ -n "${UWG_STATS:-}" ]; then
	jq -c '.end.sum' "$TMP/out.json" 2>/dev/null || true
	printf -- '--- server ---\n'
	tail -5 "$TMP/server.log" 2>/dev/null || true
	printf -- '--- client ---\n'
	tail -5 "$TMP/client.log" 2>/dev/null || true
fi

awk -v mode="$LABEL" -v cname="$CASE" -v cpu="$CPU" '
{ b[NR] = $2; p[NR] = $3; u[NR] = $4; l[NR] = $5; r[NR] = $6 }
END {
	n = NR
	for (i = 1; i <= n; i++)
		for (j = i + 1; j <= n; j++)
			if (b[i] > b[j]) {
				t = b[i]; b[i] = b[j]; b[j] = t
				t = p[i]; p[i] = p[j]; p[j] = t
				t = u[i]; u[i] = u[j]; u[j] = t
				t = l[i]; l[i] = l[j]; l[j] = t
				t = r[i]; r[i] = r[j]; r[j] = t
			}
	m = int((n + 1) / 2)
	printf "%s\t%s\t%s\t%.3f\t%.0f\t%.1f\tlost %.2f%%\trss %.1f MiB\n", mode, cname, cpu, b[m] / 1e9, p[m], u[m], l[m], r[m] / 1024
}' "$RESULT"
