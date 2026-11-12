#!/bin/sh
# Interleaved A/B comparison of two umbrawg builds inside one VM.
#
# usage: ab.sh <label-a> <module-a> <label-b> <module-b> <case> <cpu> <flows> [sec] [reps] [rounds]
# env:   UWG_OPTS_FILE, UWG_HP_KEY, UWG_MODULE_OPTS, UWG_CPU_ONLINE
set -eu

if [ "$(id -u)" -ne 0 ]; then
	echo "run as root" >&2
	exit 1
fi

DIR=$(cd "$(dirname "$0")" && pwd)
LA=${1:?label a}
MA=${2:?module a}
LB=${3:?label b}
MB=${4:?module b}
CASE=${5:?case}
CPU=${6:?cpu}
FLOWS=${7:?flows}
DUR=${8:-8}
REPS=${9:-2}
ROUNDS=${10:-2}

i=1
while [ "$i" -le "$ROUNDS" ]; do
	UWG_MODULE="$MA" UWG_LABEL="$LA" FLOWS="$FLOWS" \
		UWG_MODULE_OPTS="${UWG_OPTS_A:-${UWG_MODULE_OPTS:-}}" \
		"$DIR/bench.sh" umbrawg "$CPU" "$CASE" "$DUR" "$REPS"
	sleep 1
	UWG_MODULE="$MB" UWG_LABEL="$LB" FLOWS="$FLOWS" \
		UWG_MODULE_OPTS="${UWG_OPTS_B:-${UWG_MODULE_OPTS:-}}" \
		"$DIR/bench.sh" umbrawg "$CPU" "$CASE" "$DUR" "$REPS"
	sleep 1
	i=$((i + 1))
done
