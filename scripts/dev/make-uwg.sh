#!/bin/sh
# Build a local development copy of the userspace tool for the "umbrawg"
# genetlink family by patching a WireGuard-compatible binary in place.
#
# The source binary and its legacy family name must be provided explicitly:
#   UWG_SOURCE=/path/to/tool UWG_LEGACY_FAMILY=<family> ./make-uwg.sh
# This keeps the repository itself free of any legacy naming.
set -eu

src="${UWG_SOURCE:-}"
legacy="${UWG_LEGACY_FAMILY:-}"
out="${1:-$(cd "$(dirname "$0")" && pwd)/uwg}"

if [ -z "$src" ] || [ -z "$legacy" ]; then
	echo "set UWG_SOURCE and UWG_LEGACY_FAMILY" >&2
	exit 1
fi
if [ ! -x "$src" ]; then
	echo "source tool not found: $src" >&2
	exit 1
fi

python3 - "$src" "$out" "$legacy" <<'PY'
import sys

src, out, legacy = sys.argv[1], sys.argv[2], sys.argv[3]
needle = legacy.encode()
repl = b"umbrawg"
if len(repl) > len(needle):
    sys.exit("legacy family name is shorter than the new one")
repl = repl + b"\x00" * (len(needle) - len(repl))
data = open(src, "rb").read()
if needle not in data:
    sys.exit("family string not found in %s" % src)
open(out, "wb").write(data.replace(needle, repl))
PY

chmod 0755 "$out"
printf 'patched tool: %s\n' "$out"
