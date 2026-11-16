#!/usr/bin/env bash
# run.sh -- build the cassette test for this PC (into build/tapetest/) and run it.
# Prints ok or FAILED.  See tapetest.c.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
mkdir -p "$root/build/tapetest"
gcc -O2 -g -Wall -fsanitize=address,undefined -o "$root/build/tapetest/tapetest" "$here/tapetest.c" "$root/src/apps/lib/tape/tape.c" "$root/src/apps/lib/tape/lz.c" -lm
exec "$root/build/tapetest/tapetest" "$@"
