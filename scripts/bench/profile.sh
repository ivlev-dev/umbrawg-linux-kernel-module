#!/bin/sh
# Profiling wrapper: runs one bench case under perf record inside the VM.
#
# usage: profile.sh <module|ref> <cpu> <case> <flows> [sec]
# env:   PROFILE_CPU=<n>   restrict perf to one CPU
#        PROFILE_SYM=<pat> module name to keep loaded for symbols
set -eu

if [ "$(id -u)" -ne 0 ]; then
	echo "run as root: sudo $0 ..." >&2
	exit 1
fi

DIR=$(cd "$(dirname "$0")" && pwd)
TARGET=${1:?module or ref}
CPU=${2:?cpu}
CASE=${3:?case}
FLOWS=${4:?flows}
DUR=${5:-8}
PROFILE_CPU=${PROFILE_CPU:-0-3}
PROFILE_SYM=${PROFILE_SYM:-umbrawg}
PROFILE_DELAY=${PROFILE_DELAY:-8}

LOG=/tmp/bench-profile.log
DATA=/tmp/perf.data
PERF_CPU=
[ -n "$PROFILE_CPU" ] && PERF_CPU="-C $PROFILE_CPU"

if [ "$TARGET" = ref ]; then
	: "${UWG_REF_TOOL:?}" "${UWG_REF_KIND:?}" "${UWG_REF_MODULE:?}"
	MODE=ref
	KEEP=0
else
	MODE=umbrawg
	export UWG_MODULE="$TARGET"
	KEEP=1
fi

sysctl -w kernel.perf_event_paranoid=-1 >/dev/null 2>&1 || true
rm -f "$DATA"

if [ "$CASE" = udp-split ]; then
	UWG_KEEP_MODULE=$KEEP "$DIR/udp-split.sh" "$MODE" "$CPU" "$FLOWS" "$DUR" 1 >"$LOG" 2>&1 &
else
	UWG_KEEP_MODULE=$KEEP UWG_LABEL=profile FLOWS="$FLOWS" \
		"$DIR/bench.sh" "$MODE" "$CPU" "$CASE" "$DUR" 1 >"$LOG" 2>&1 &
fi
BPID=$!

sleep "$PROFILE_DELAY"
# shellcheck disable=SC2086
perf record -F 1999 -g -a $PERF_CPU -o "$DATA" -- sleep "$DUR" >/dev/null 2>&1 || true
wait "$BPID" || true

cat "$LOG" 2>/dev/null || true
dmesg | grep -iE 'BUG|Oops|WARNING|watchdog|soft lockup|stall' | tail -5 || true
echo "=== perf.data: $(ls -l "$DATA" 2>/dev/null | awk '{print $5}') bytes, cpu: ${PROFILE_CPU:-all} ==="
if [ -s "$DATA" ]; then
	KALL=
	[ "$KEEP" = 1 ] && [ -r /proc/kallsyms ] && KALL="--kallsyms=/proc/kallsyms"
	echo '=== top self ==='
	# shellcheck disable=SC2086
	perf report -i "$DATA" --stdio --no-children --sort dso,symbol \
		$KALL --percent-limit 0.3 2>/dev/null | sed -n '/Overhead/,$p' | head -30
	echo '=== top children ==='
	# shellcheck disable=SC2086
	perf report -i "$DATA" --stdio -g --sort symbol \
		$KALL --percent-limit 1 2>/dev/null | sed -n '/Overhead/,$p' | head -40
fi

if [ "$KEEP" = 1 ]; then
	rmmod "$PROFILE_SYM" 2>/dev/null || true
fi
