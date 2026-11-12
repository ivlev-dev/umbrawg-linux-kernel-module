#!/bin/sh
# Run the throughput matrix for the selected build and print TSV:
#   mode case cpu gbps pps busy%
#
# usage: matrix.sh <old|new> [seconds] [reps]
set -eu

if [ "$(id -u)" -ne 0 ]; then
	echo "run as root: sudo $0 <old|new>" >&2
	exit 1
fi

DIR=$(cd "$(dirname "$0")" && pwd)
MODE=${1:?mode}
DUR=${2:-10}
REPS=${3:-3}
CPUS=${CPUS:-"0"}

for cpu in $CPUS; do
	for case in tcp1420 udp1320 udp64; do
		"$DIR/bench.sh" "$MODE" "$cpu" "$case" "$DUR" "$REPS"
	done
done
