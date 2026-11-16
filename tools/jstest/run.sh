#!/usr/bin/env bash
# run.sh -- build the script engine for this PC (into dist/) and run every
# test in tools/jstest/tests.  Prints ok or FAILED for each.  See host.c.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
mkdir -p "$root/dist"
src=()
for f in "$root"/room/*.c; do
    case "$(basename "$f")" in room.c|dom.c) ;; *) src+=("$f") ;; esac
done
gcc -O1 -g -Wall -Wextra -Wno-unused-parameter -fno-omit-frame-pointer \
    -fsanitize=address,undefined -fno-sanitize=float-cast-overflow,signed-integer-overflow,shift \
    -o "$root/dist/jstest" "$here/host.c" "${src[@]}" -lm
if [ "$#" -gt 0 ]; then exec "$root/dist/jstest" "$@"; fi
"$root/dist/jstest" -q "$here"/tests/*.js
echo "   again, sweeping memory at every chance:"
exec "$root/dist/jstest" -q -stress "$here"/tests/*.js
