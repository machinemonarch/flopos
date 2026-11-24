#!/usr/bin/env bash
# keepfiles.sh OLD NEW DIR
#
# Carry what is the user's from an old FLOPOS floppy into a new image,
# before the new image is written over the floppy.  OLD is the floppy (a
# device) or an image of one; NEW is the image about to be written; DIR is
# an empty folder to work in, which is left holding a copy of everything
# that was carried (the caller removes it once the floppy is verified).
#
#   - the journal, the settings and the bookmarks, where the new image
#     keeps them at the same size;
#   - everything in the INVENTORY folder, folders and all;
#   - from floppies older than the inventory: the documents WRITE1.MD to
#     WRITE8.MD that have something in them, which go into the inventory
#     under the same names.
#
# Prints one line for each thing kept.  Exits 1, having said why, if
# something of the user's could not be carried: the caller must then not
# write the floppy.  Used by writefloppy.sh; tried by test/keeptest.sh.
set -euo pipefail

[ $# -eq 3 ] || { echo "keepfiles.sh OLD NEW DIR" >&2; exit 2; }
OLD="$1"; NEW="$2"; DIR="$3"
[ -d "$DIR" ] || { echo "keepfiles: $DIR is not a folder" >&2; exit 2; }
[ -z "$(ls -A "$DIR")" ] || { echo "keepfiles: $DIR is not empty" >&2; exit 2; }
export MTOOLS_SKIP_CHECK=1

for f in JOURNAL.TXT STATE.BIN BOOKMARK.TXT SITES.TXT; do
    if mcopy -n -i "$OLD" "::$f" "$DIR/$f" 2>/dev/null; then
        want=$( { mcopy -n -i "$NEW" "::$f" - 2>/dev/null || true; } | wc -c)
        have=$(stat -c%s "$DIR/$f")
        if [ "$want" -gt 0 ] && [ "$want" = "$have" ]; then
            mcopy -o -n -i "$NEW" "$DIR/$f" "::$f"
            echo "$f"
        elif [ "$f" = STATE.BIN ] && [ "$want" -ge 512 ] && [ "$have" -gt "$want" ]; then
            # The 8 KB STATE.BIN from before 1.49: its header (the boot count)
            # and its settings, which live at its far end, into the new size.
            { head -c 24 "$DIR/$f"; head -c 4096 /dev/zero | head -c $((want - 24 - 448)); tail -c 448 "$DIR/$f"; } > "$DIR/$f.new"
            mv -- "$DIR/$f.new" "$DIR/$f"
            mcopy -o -n -i "$NEW" "$DIR/$f" "::$f"
            echo "$f"
        fi
    fi
done

# The inventory only exists from 1.31.0 on.  An image without one keeps
# its documents the old way, and there is nothing more to carry into it.
if ! mdir -i "$NEW" ::INVENTORY >/dev/null 2>&1; then
    for n in 1 2 3 4 5 6 7 8; do
        f="WRITE$n.MD"
        if mcopy -n -i "$OLD" "::$f" "$DIR/$f" 2>/dev/null; then
            want=$( { mcopy -n -i "$NEW" "::$f" - 2>/dev/null || true; } | wc -c)
            have=$(stat -c%s "$DIR/$f")
            if [ "$want" -gt 0 ] && [ "$want" = "$have" ]; then
                mcopy -o -n -i "$NEW" "$DIR/$f" "::$f"
                echo "$f"
            fi
        fi
    done
    exit 0
fi

mkdir "$DIR/inv"
if mdir -i "$OLD" ::INVENTORY >/dev/null 2>&1; then
    mcopy -s -n -i "$OLD" ::INVENTORY "$DIR/inv/" \
        || { echo "The old floppy's inventory will not read. Nothing has been written." >&2; exit 1; }
fi
mkdir -p "$DIR/inv/INVENTORY"

# Documents from before the inventory: fixed at 16 KB, text up to an end
# mark and then filling.  Only the text is kept.
for n in 1 2 3 4 5 6 7 8; do
    f="WRITE$n.MD"
    if mcopy -n -i "$OLD" "::$f" "$DIR/old-$f" 2>/dev/null; then
        if [ ! -e "$DIR/inv/INVENTORY/$f" ]; then
            python3 -I - "$DIR/old-$f" "$DIR/inv/INVENTORY/$f" <<'PY'
import sys
data = open(sys.argv[1], "rb").read()
for mark in (b"\x1a", b"\x00"):
    at = data.find(mark)
    if at >= 0: data = data[:at]
if data.strip():
    open(sys.argv[2], "wb").write(data)
PY
        fi
    fi
done

if [ -n "$(ls -A "$DIR/inv/INVENTORY")" ]; then
    mcopy -s -n -o -i "$NEW" "$DIR/inv/INVENTORY"/* ::INVENTORY/ \
        || { echo "What is in the inventory does not fit beside this release. Nothing has been written." >&2; exit 1; }
    ( cd "$DIR/inv" && find INVENTORY -mindepth 1 | sort )
fi
