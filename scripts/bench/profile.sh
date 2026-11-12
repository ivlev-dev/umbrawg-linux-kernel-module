#!/bin/sh
# Profiling wrapper: runs one bench case under perf record inside the VM.
#
# usage: profile.sh <module|ref> <cpu> <case> <flows> [sec]
# env:   PROFILE_CPU=<n>   restrict perf to one CPU
#        PROFILE_SYM=<pat> kallsyms marker of the module under test
set -eu

if [ "$(id -u)" -ne 0 ]; then
	echo "run as root" >&2
	exit 1
fi

DIR=$(cd "$(dirname "$0")" && pwd)
TARGET=${1:?module or ref}
CPU=${2:?cpu}
CASE=${3:?case}
FLOWS=${4:?flows}
DUR=${5:-8}
PROFILE_CPU=${PROFILE_CPU:-}
PROFILE_SYM=${PROFILE_SYM:-umbrawg}

LOG=/tmp/bench-profile.log
KALL=/tmp/profile-kallsyms
DATA=/tmp/perf.data
PERF_CPU=
[ -n "$PROFILE_CPU" ] && PERF_CPU="-C $PROFILE_CPU"

if [ "$TARGET" = ref ]; then
	: "${UWG_REF_TOOL:?}" "${UWG_REF_KIND:?}" "${UWG_REF_MODULE:?}"
	MODE=ref
else
	MODE=umbrawg
	export UWG_MODULE="$TARGET"
fi

UWG_LABEL=profile FLOWS="$FLOWS" \
	"$DIR/bench.sh" "$MODE" "$CPU" "$CASE" "$DUR" 1 >"$LOG" 2>&1 &
BPID=$!

(i=0; while [ "$i" -lt 150 ]; do
	if grep -q "$PROFILE_SYM" /proc/kallsyms 2>/dev/null; then
		cat /proc/kallsyms >"$KALL"
		break
	fi
	sleep 0.1
	i=$((i + 1))
done) &

sleep 3
# shellcheck disable=SC2086
perf record -F 2999 -g -a $PERF_CPU -o "$DATA" -- sleep "$DUR" 2>/dev/null || true
wait "$BPID" || true

cat "$LOG"
echo "=== kallsyms: $(wc -l <"$KALL" 2>/dev/null || echo 0) lines, perf cpu: ${PROFILE_CPU:-all} ==="
echo '=== top self ==='
perf report -i "$DATA" --kallsyms="$KALL" --stdio --no-children \
	--percent-limit 0.8 --sort dso,symbol 2>/dev/null \
	| grep -E '^ +[0-9]+\.[0-9]+%' | head -30
echo '=== top children ==='
perf report -i "$DATA" --kallsyms="$KALL" --stdio --percent-limit 2 \
	--sort symbol 2>/dev/null \
	| grep -E '^ +[0-9]+\.[0-9]+%' | head -30
echo '=== spinlock callers ==='
perf report -i "$DATA" --kallsyms="$KALL" --stdio -g graph,0.5,caller \
	-q --percent-limit 0.4 2>/dev/null \
	| grep -A6 -E 'spin_lock|queued_spin' | head -60
echo '=== call tree ==='
perf report -i "$DATA" --kallsyms="$KALL" --stdio -g graph,0.5,callee \
	-q --percent-limit 0.3 2>/dev/null \
	| grep -A12 -E "${PROFILE_TREE:-wg_xmit|queue_work}" | head -120
