#!/bin/sh
# Interleaved comparison of all implementations on one scenario.
#
# Variants: amneziawg, umb-none, umb-bbr, umb-brutal, umb-bbr-fec,
#           hy-bbr, hy-brutal.
#
# usage: suite.sh <tcp1420|udp1340> [sec] [reps] [rounds]
# env:   VARIANTS="umb-none hy-bbr"    subset of variants
#        UWG_NETEM="loss 5%"           netem on the client-side veth
#        UWG_UDP_BITRATE=4000          total UDP rate in Mbit/s
#        UWG_BRUTAL_BPS=4500           configured brutal rate in Mbit/s
#        HY_BW="1 gbps"                configured hysteria2 brutal bandwidth
#        FLOWS=12                      parallel iperf3 streams
#        UWG_REF_TOOL/UWG_REF_KIND/UWG_REF_MODULE for the amneziawg variant
set -eu

if [ "$(id -u)" -ne 0 ]; then
	echo "run as root" >&2
	exit 1
fi

DIR=$(cd "$(dirname "$0")" && pwd)
CASE=${1:?case}
DUR=${2:-3}
REPS=${3:-2}
ROUNDS=${4:-2}
FLOWS=${FLOWS:-1}
VARIANTS=${VARIANTS:-"amneziawg umb-none umb-bbr umb-brutal umb-bbr-fec hy-bbr hy-brutal"}
UWG_BRUTAL_BPS=${UWG_BRUTAL_BPS:-4500}
HY_BW=${HY_BW:-"1 gbps"}

run_variant() {
	v=$1
	case "$v" in
	amneziawg)
		: "${UWG_REF_TOOL:?}" "${UWG_REF_KIND:?}" "${UWG_REF_MODULE:?}"
		UWG_LABEL=amneziawg FLOWS="$FLOWS" \
			"$DIR/bench.sh" ref 0 "$CASE" "$DUR" "$REPS" ;;
	umb-*)
		opts=
		label=$v
		case "$v" in
		umb-none)
			opts="all off" ;;
		umb-bbr)
			opts="all bbr" ;;
		umb-brutal)
			opts="all brutal $UWG_BRUTAL_BPS" ;;
		umb-bbr-fec)
			opts="all bbr" ;;
		esac
		UWG_CC="$opts" \
			UWG_FEC=$([ "$v" = umb-bbr-fec ] && echo "16 1") \
			UWG_LABEL="$label" FLOWS="$FLOWS" \
			"$DIR/bench.sh" umbrawg 0 "$CASE" "$DUR" "$REPS" ;;
	hy-*)
		cc=${v#hy-}
		bw=
		[ "$cc" = brutal ] && bw=$HY_BW
		HY_CC=$cc HY_BW=$bw UWG_LABEL="$v" FLOWS="$FLOWS" \
			"$DIR/hysteria.sh" 0 "$CASE" "$DUR" "$REPS" ;;
	*)
		echo "unknown variant: $v" >&2
		exit 1 ;;
	esac
}

round=1
while [ "$round" -le "$ROUNDS" ]; do
	for v in $VARIANTS; do
		run_variant "$v"
		sleep 1
	done
	round=$((round + 1))
done
