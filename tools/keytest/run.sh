#!/usr/bin/env bash
# run.sh -- build the keyboard test for this PC (into dist/) and run it.
# Prints ok or FAILED.  See keytest.c.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
mkdir -p "$root/dist"
# glyph_index() alone, taken from kernel/draw.c each time so it cannot
# drift from the real one.
{ echo '#include "flopos.h"'; sed -n '/^int glyph_index(int c)/,/^}/p' "$root/kernel/draw.c"; } > "$root/dist/keytest-glyph.c"
gcc -O1 -g -Wall -fsanitize=address,undefined -I"$root/kernel" -o "$root/dist/keytest" \
    "$here/keytest.c" "$root/kernel/keymap.c" "$root/kernel/keymaps.c" "$root/kernel/font.c" "$root/dist/keytest-glyph.c"
exec "$root/dist/keytest" "$@"
