#!/bin/sh
# Build a dev copy of amneziawg-tools that talks to the "umbra-wg" genetlink
# family instead of "amneziawg". The family string is patched in place with a
# same-length substitution, so the ELF layout is preserved.
set -eu

src="${1:-/usr/bin/awg}"
out="${2:-$(cd "$(dirname "$0")" && pwd)/awg-umbra}"

if [ ! -x "$src" ]; then
	echo "awg binary not found: $src" >&2
	exit 1
fi

python3 - "$src" "$out" <<'PY'
import sys

src, out = sys.argv[1], sys.argv[2]
data = open(src, "rb").read()
needle = b"amneziawg"
repl = b"umbra-wg\x00"
if needle not in data:
    sys.exit("family string not found in %s" % src)
if len(repl) != len(needle):
    sys.exit("replacement must keep the string length")
open(out, "wb").write(data.replace(needle, repl))
PY

chmod 0755 "$out"
printf 'patched tool: %s\n' "$out"
