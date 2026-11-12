#!/bin/sh
# Run a command on exclusively reserved CPUs using a cgroup v2 isolated
# cpuset partition. The partition is created before the run and removed after.
#
# usage: vm.sh <command...>
# env:   BENCH_CPUS (default 8-11)
set -u

CPUS=${BENCH_CPUS:-8-11}
CG=/sys/fs/cgroup/umbra-bench

if [ "$(id -u)" -ne 0 ]; then
	echo "run as root" >&2
	exit 1
fi

cleanup() {
	echo $$ >/sys/fs/cgroup/cgroup.procs 2>/dev/null || true
	sleep 0.2
	rmdir "$CG" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

set -e
mkdir -p "$CG"
echo "$CPUS" >"$CG/cpuset.cpus"
if [ -f "$CG/cpuset.cpus.exclusive" ]; then
	echo "$CPUS" >"$CG/cpuset.cpus.exclusive"
fi
echo isolated >"$CG/cpuset.cpus.partition"
echo $$ >"$CG/cgroup.procs"
set +e

"$@"
rc=$?

cleanup
trap - EXIT INT TERM
exit "$rc"
