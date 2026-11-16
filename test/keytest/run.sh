#!/usr/bin/env bash
# run.sh -- build the keyboard test for this PC (into build/keytest/) and run it.
# Prints ok or FAILED.  See keytest.c.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
mkdir -p "$root/build/keytest"
gcc -O1 -g -Wall -fsanitize=address,undefined -I"$root/src/kernel" -o "$root/build/keytest/keytest" \
    "$here/keytest.c" "$root/src/kernel/keymap.c" "$root/src/kernel/gen/keymaps.c" "$root/src/kernel/gen/font.c" "$root/src/kernel/draw.c"
exec "$root/build/keytest/keytest" "$@"
