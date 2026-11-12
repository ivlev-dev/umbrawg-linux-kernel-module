#!/bin/sh
# Reference vs umbrawg matrix for the 4-CPU stand.
#
# usage: matrix4.sh [rounds]
# env:   UWG_MODULE, UWG_HP_KEY, UWG_REF_TOOL, UWG_REF_KIND, UWG_REF_MODULE,
#        UWG_DUR (default 8), UWG_REPS (default 2), UWG_PROFILES (default "default obfs")
set -eu

DIR=$(cd "$(dirname "$0")" && pwd)
ROUNDS=${1:-2}
DUR=${UWG_DUR:-8}
REPS=${UWG_REPS:-2}
PROFILES=${UWG_PROFILES:-"default obfs"}

for prof in $PROFILES; do
	opts=
	hp=
	if [ "$prof" = obfs ]; then
		opts="$DIR/profiles/obfs-full.opts"
		hp=${UWG_HP_KEY:-}
	fi
	for spec in "tcp1420 0 1" "tcp1420 0-3 4" "udp1340 0 1" "udp1340 0-3 4"; do
		set -- $spec
		UWG_OPTS_FILE="$opts" UWG_HP_KEY="$hp" \
			"$DIR/compare.sh" "$1" "$2" "$3" "$DUR" "$REPS" "$ROUNDS"
	done
done
