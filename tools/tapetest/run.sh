#!/usr/bin/env bash
# run.sh -- build the cassette test for this PC (into dist/) and run it.
# Prints ok or FAILED.  See tapetest.c.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
mkdir -p "$root/dist"
gcc -O2 -Wall -o "$root/dist/tapetest" "$here/tapetest.c" "$root/apps/lib/tape/tape.c" -lm
exec "$root/dist/tapetest" "$@"
