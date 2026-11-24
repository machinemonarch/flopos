#!/bin/bash
# JOURNAL.TXT deleted on another computer, then a document saved in FLOPOS
# and "remember": the document must come back whole.  Before 1.49 the
# journal was written where the build had put it, over the document.
#   test/fstest/moved.sh [IMAGE]          prints ok or FAILED
set -euo pipefail
cd "$(dirname "$0")/../.."
work=$(mktemp -d); trap 'rm -rf "${work:?}"' EXIT
cp "${1:-dist/flopos.img}" "$work/x.img"
mdel -i "$work/x.img" ::JOURNAL.TXT
python3 test/bench/run.py --keep "$work/x.img" "$work/out" 'type:scribe note.md\n' 'type:hello there' \
    'key:ctrl-s=>saved' 'key:esc' 'type:remember\n=>Kept|nowhere|would not|Already' > "$work/bench.txt"
if [ "$(mtype -i "$work/x.img" ::INVENTORY/NOTE.MD | head -c 11)" = "hello there" ]; then echo ok; else echo FAILED; fi
