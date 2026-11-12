#!/bin/sh
# Final comparison matrix: reference build (optional) vs umbrawg.
#
# usage:
#   UWG_REF_TOOL=/path/to/tool UWG_REF_KIND=<kind> UWG_REF_MODULE=<module> \
#   UWG_HP_KEY=<base64> ./final-matrix.sh
#
# env: UWG_DUR (default 10), UWG_REPS (default 3),
#      UWG_PROFILES (default "default obfs")
set -eu

DIR=$(cd "$(dirname "$0")" && pwd)
DUR=${UWG_DUR:-10}
REPS=${UWG_REPS:-3}
PROFILES=${UWG_PROFILES:-"default obfs"}

run_one() {
	mode=$1
	label=$2
	prof=$3
	case_name=$4
	cpu=$5
	flows=$6
	opts=
	hp=

	if [ "$prof" = obfs ]; then
		opts="$DIR/profiles/obfs-full.opts"
		hp=${UWG_HP_KEY:-}
	fi

	UWG_LABEL="$label" FLOWS="$flows" UWG_OPTS_FILE="$opts" \
		UWG_HP_KEY="$hp" "$DIR/bench.sh" "$mode" "$cpu" "$case_name" \
		"$DUR" "$REPS"
}

for prof in $PROFILES; do
	for spec in "tcp1420 0 1" "udp1340 0 1" "tcp1420 0-11 12" "udp1340 0-11 12"; do
		set -- $spec
		case_name=$1
		cpu=$2
		flows=$3

		if [ -n "${UWG_REF_TOOL:-}" ] && [ -n "${UWG_REF_KIND:-}" ] && \
		   [ -n "${UWG_REF_MODULE:-}" ]; then
			run_one ref "ref-$prof-$case_name-$cpu" "$prof" \
				"$case_name" "$cpu" "$flows"
		fi
		run_one umbrawg "new-$prof-$case_name-$cpu" "$prof" \
			"$case_name" "$cpu" "$flows"
	done
done
