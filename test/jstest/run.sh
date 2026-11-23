#!/usr/bin/env bash
# run.sh -- build the script engine for this PC (into build/jstest/) and run every
# test in test/jstest/tests.  Prints ok or FAILED for each.  See host.c.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
mkdir -p "$root/build/jstest"
src=()
for f in "$root"/src/room/*.c; do
    case "$(basename "$f")" in room.c|dom.c) ;; *) src+=("$f") ;; esac
done
gcc -O1 -g -Wall -Wextra -Wno-unused-parameter -fno-omit-frame-pointer \
    -fsanitize=address,undefined -fno-sanitize=float-cast-overflow,signed-integer-overflow,shift \
    -o "$root/build/jstest/jstest" "$here/host.c" "${src[@]}" -lm
if [ "$#" -gt 0 ]; then exec "$root/build/jstest/jstest" "$@"; fi
"$root/build/jstest/jstest" -q "$here"/tests/*.js "$here"/tests/*.mjs
echo "   again, sweeping memory at every chance:"
exec "$root/build/jstest/jstest" -q -stress "$here"/tests/*.js "$here"/tests/*.mjs
