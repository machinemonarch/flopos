#!/usr/bin/env bash
# keeptest.sh [NEW_IMAGE] [OLD_IMAGE]
#
# Tries tools/floppy/keepfiles.sh, which carries the user's files from an old
# floppy into a new release, on images instead of a real floppy:
#   1. a floppy from before the inventory, with a document in WRITE2.MD
#   2. a floppy with an inventory of folders and files
#   3. a disk that is not a FLOPOS floppy at all
#   4. an inventory too big to fit beside the release (must be refused)
# NEW_IMAGE is dist/flopos.img unless named; OLD_IMAGE the 1.30.0 release.
# Prints ok or FAILED and what was wrong.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NEW="${1:-$ROOT/dist/flopos.img}"
OLD="${2:-$ROOT/releases/flopos-1.30.0.img}"
W="$(mktemp -d "${TMPDIR:-/tmp}/flopos-keeptest.XXXXXX")" || exit 2
done_() { case "$W" in */flopos-keeptest.??????) rm -rf -- "$W" ;; esac; }
trap done_ EXIT
bad() { echo "FAILED"; echo "   $*"; exit 1; }
export MTOOLS_SKIP_CHECK=1

# 1. before the inventory
cp "$OLD" "$W/old.img"; cp "$NEW" "$W/new.img"
python3 -I -c 'import sys; open(sys.argv[1],"wb").write(b"# hello\n\nfrom before\n\x1a" + b"\0"*(16384-21))' "$W/doc"
mcopy -o -n -i "$W/old.img" "$W/doc" ::WRITE2.MD || bad "could not prepare the old image"
mcopy -n -i "$W/old.img" ::JOURNAL.TXT "$W/journal" || bad "the old image has no journal"
python3 -I -c 'import sys; d=bytearray(open(sys.argv[1],"rb").read()); d[200:206]=b"marked"; open(sys.argv[1],"wb").write(d)' "$W/journal"
mcopy -o -n -i "$W/old.img" "$W/journal" ::JOURNAL.TXT
mkdir "$W/k1"
"$ROOT/tools/floppy/keepfiles.sh" "$W/old.img" "$W/new.img" "$W/k1" > "$W/out1" 2> "$W/err1" || bad "an old floppy was refused: $(cat "$W/err1")"
[ "$(mtype -i "$W/new.img" ::INVENTORY/WRITE2.MD 2>/dev/null)" = "$(printf '# hello\n\nfrom before')" ] || bad "WRITE2.MD did not arrive in the inventory as it was"
mdir -i "$W/new.img" ::INVENTORY/WRITE1.MD >/dev/null 2>&1 && bad "an empty document was carried"
mcopy -n -i "$W/new.img" ::JOURNAL.TXT - | grep -q marked || bad "the journal was not carried"
fsck.fat -n "$W/new.img" >/dev/null 2>&1 || bad "the new image is damaged after carrying an old floppy"

# 2. an inventory with folders
cp "$W/new.img" "$W/old2.img"; cp "$NEW" "$W/new2.img"
head -c 70000 /dev/urandom > "$W/SONG.MOD"
mmd -i "$W/old2.img" ::INVENTORY/BOX ::INVENTORY/BOX/DEEP || bad "could not prepare the second image"
mcopy -n -i "$W/old2.img" "$W/SONG.MOD" ::INVENTORY/BOX/DEEP/SONG.MOD
mkdir "$W/k2"
"$ROOT/tools/floppy/keepfiles.sh" "$W/old2.img" "$W/new2.img" "$W/k2" > "$W/out2" 2> "$W/err2" || bad "an inventory was refused: $(cat "$W/err2")"
mcopy -n -i "$W/new2.img" ::INVENTORY/BOX/DEEP/SONG.MOD "$W/back" 2>/dev/null && cmp -s "$W/back" "$W/SONG.MOD" || bad "a file in a folder did not come across whole"
[ "$(mtype -i "$W/new2.img" ::INVENTORY/WRITE2.MD 2>/dev/null)" = "$(printf '# hello\n\nfrom before')" ] || bad "a document in the inventory was lost"
grep -q '^INVENTORY/BOX/DEEP/SONG.MOD$' "$W/out2" || bad "what was kept was not listed"
fsck.fat -n "$W/new2.img" >/dev/null 2>&1 || bad "the new image is damaged after carrying an inventory"
for f in KERNEL.BIN WIZARD.WIZ BROWZER.APP; do
    cmp -s <(mcopy -n -i "$W/new2.img" "::$f" -) <(mcopy -n -i "$NEW" "::$f" -) || bad "$f changed"
done

# 3. not a FLOPOS disk
head -c 1474560 /dev/zero > "$W/blank.img"; cp "$NEW" "$W/new3.img"; mkdir "$W/k3"
"$ROOT/tools/floppy/keepfiles.sh" "$W/blank.img" "$W/new3.img" "$W/k3" > "$W/out3" 2>/dev/null || bad "a blank disk stopped the write"
[ -s "$W/out3" ] && bad "something was kept from a blank disk"
cmp -s "$W/new3.img" "$NEW" || bad "a blank disk changed the image"

# 4. too much to fit
cp "$W/new.img" "$W/old4.img"; cp "$NEW" "$W/new4.img"; mkdir "$W/k4"
mdel -i "$W/old4.img" ::WIZARD.WIZ ::BROWZER.APP 2>/dev/null
head -c 200000 /dev/urandom > "$W/A.BIN"; head -c 200000 /dev/urandom > "$W/B.BIN"
mcopy -n -i "$W/old4.img" "$W/A.BIN" "$W/B.BIN" ::INVENTORY/ || bad "could not prepare the fourth image"
"$ROOT/tools/floppy/keepfiles.sh" "$W/old4.img" "$W/new4.img" "$W/k4" >/dev/null 2>&1 && bad "an inventory that cannot fit was not refused"

echo "ok"
