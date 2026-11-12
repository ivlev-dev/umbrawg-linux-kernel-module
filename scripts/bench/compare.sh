#!/bin/sh
# Interleaved comparison: reference build (amneziawg) vs umbrawg build.
#
# usage: compare.sh <case> <cpu> <flows> [sec] [reps] [rounds]
# env:   UWG_MODULE (umbrawg .ko), UWG_OPTS_FILE, UWG_HP_KEY,
#        UWG_REF_TOOL, UWG_REF_KIND, UWG_REF_MODULE
set -eu

if [ "$(id -u)" -ne 0 ]; then
	echo "run as root" >&2
	exit 1
fi

DIR=$(cd "$(dirname "$0")" && pwd)
CASE=${1:?case}
CPU=${2:?cpu}
FLOWS=${3:?flows}
DUR=${4:-8}
REPS=${5:-2}
ROUNDS=${6:-2}

: "${UWG_REF_TOOL:?}" "${UWG_REF_KIND:?}" "${UWG_REF_MODULE:?}"
UWG_MODULE=${UWG_MODULE:-$DIR/../../src/umbrawg.ko}

i=1
while [ "$i" -le "$ROUNDS" ]; do
	UWG_LABEL=amneziawg FLOWS="$FLOWS" \
		"$DIR/bench.sh" ref "$CPU" "$CASE" "$DUR" "$REPS"
	sleep 1
	UWG_MODULE="$UWG_MODULE" UWG_LABEL=umbrawg FLOWS="$FLOWS" \
		"$DIR/bench.sh" umbrawg "$CPU" "$CASE" "$DUR" "$REPS"
	sleep 1
	i=$((i + 1))
done
