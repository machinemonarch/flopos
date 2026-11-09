#!/usr/bin/env bash
#
# writefloppy.sh -- put FLOPOS on a real floppy disk, safely.
#
#   tools/writefloppy.sh                 find the floppy, confirm, write
#   tools/writefloppy.sh /dev/sde        name the device yourself
#   tools/writefloppy.sh -y              skip the confirmation
#   tools/writefloppy.sh --lowlevel      low-level format first (slow)
#   tools/writefloppy.sh --full          write all 1.44MB, not just the part
#                                        that is used
#   tools/writefloppy.sh --fresh         do not carry documents, the journal
#                                        and taught words over from the disk
#
# Upgrading keeps what you wrote: documents, the journal and taught words
# already on the floppy are copied into the new image before it goes down.
#
# It only asks for a password if it has to. Finding the drive and checking
# that it really is a floppy needs no privilege at all; writing to it does,
# unless the drive belongs to you. To make it belong to you, once:
#
#     sudo cp tools/99-flopos-floppy.rules /etc/udev/rules.d/
#     sudo udevadm control --reload
#
# then unplug the drive and plug it back in. After that it never asks
# again.
#
# On "formatting": the image already contains a complete, freshly made
# FAT12 filesystem, so writing it IS formatting the disk -- there is no
# separate step and nothing of the old contents survives. --lowlevel is
# only for when the magnetic media itself is suspect, and rewrites the
# sector headers before the image goes down.
#
# The safety rule that matters: this refuses to write to anything that is
# not exactly 1,474,560 bytes. Your 2.7TB arrays cannot be hit by a typo.

set -euo pipefail

FLOPPY_BYTES=1474560
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Prefer the newest archived release over dist/flopos.img. Releases never
# change once made; dist/flopos.img is rebuilt constantly, and writing a
# file that is being rewritten underneath you is how you end up verifying
# a floppy against an image that is no longer the one you wrote.
IMAGE_DEFAULT="$ROOT/dist/flopos.img"
_newest="$(ls -1t "$ROOT"/releases/flopos-*.img 2>/dev/null | head -1 || true)"
if [ -n "$_newest" ]; then IMAGE_DEFAULT="$_newest"; fi

DEVICE=""
IMAGE="$IMAGE_DEFAULT"
ASSUME_YES=0
LOWLEVEL=0
FULL=0
FRESH=0

die()  { printf '\n  %s\n\n' "$*" >&2; exit 1; }
note() { printf '  %s\n' "$*"; }
step() { printf '\n  %s\n' "$*"; }

while [ $# -gt 0 ]; do
    case "$1" in
        -y|--yes)      ASSUME_YES=1 ;;
        --lowlevel)    LOWLEVEL=1 ;;
        --full)        FULL=1 ;;
        --fresh)       FRESH=1 ;;
        -i|--image)    shift; IMAGE="${1:-}" ;;
        -h|--help)     sed -n '2,20p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        /dev/*)        DEVICE="$1" ;;
        *)             die "I don't understand '$1'. Try --help." ;;
    esac
    shift
done

# Asking for root is put off until we know it is needed: finding the drive
# and checking it needs none, and with the udev rule installed, writing to
# it needs none either.
escalate() {
    local args=()
    [ -n "$DEVICE" ]        && args+=("$DEVICE")
    [ "$ASSUME_YES" -eq 1 ] && args+=(-y)
    [ "$LOWLEVEL" -eq 1 ]   && args+=(--lowlevel)
    [ "$FULL" -eq 1 ]       && args+=(--full)
    [ "$FRESH" -eq 1 ]      && args+=(--fresh)
    args+=(--image "$IMAGE")
    note "$1"
    exec sudo -- "${BASH_SOURCE[0]}" "${args[@]}"
}

# --- the image ------------------------------------------------------------
[ -f "$IMAGE" ] || die "No image at $IMAGE. Run 'make' first."

img_size=$(stat -c%s "$IMAGE")
[ "$img_size" -eq "$FLOPPY_BYTES" ] \
    || die "$IMAGE is $img_size bytes, not $FLOPPY_BYTES. That is not a floppy image."

# How much of it is worth writing.
#
# A floppy drive manages about 28 kilobytes a second, so the whole disk is
# roughly fifty seconds to write and fifty more to read back -- and about
# seven eighths of what it spends that time on is empty filesystem.  The
# allocation table says those clusters are free, so whatever is already on
# the diskette out there is unreferenced and harmless; mkimage records how
# far the real content reaches and we stop at a track boundary past it.
#
# --full writes all of it, for when the diskette's far end is suspect or
# when there is something on it you would rather was gone.
BLOCK=36864                              # two cylinders, the dd block size
WRITE_BYTES=$FLOPPY_BYTES
if [ "$FULL" -ne 1 ] && [ -f "$IMAGE.used" ]; then
    used=$(tr -dc '0-9' < "$IMAGE.used")
    if [ -n "$used" ] && [ "$used" -gt 0 ] && [ "$used" -le "$FLOPPY_BYTES" ]; then
        WRITE_BYTES=$(( (used + BLOCK - 1) / BLOCK * BLOCK ))
    fi
fi

# Warn if the tree has moved on since the image was built.
# Only the things that actually end up inside the image count here --
# editing this script is not a reason to tell you the disk is stale.
root="$(cd "$(dirname "$IMAGE")/.." && pwd)"
newer=""
for d in boot kernel; do
    if [ -d "$root/$d" ]; then
        found_newer=$(find "$root/$d" -type f -newer "$IMAGE" 2>/dev/null | head -1 || true)
        if [ -n "$found_newer" ]; then newer="$found_newer"; break; fi
    fi
done
if [ -z "$newer" ]; then
    for f in "$root/tools/mkimage.py" "$root/tools/mkfont.py" "$root/README.md"; do
        if [ -f "$f" ] && [ "$f" -nt "$IMAGE" ]; then newer="$f"; break; fi
    done
fi
if [ -n "$newer" ]; then
    note ""
    note "NOTE: $(basename "$newer") is newer than the image."
    note "      You may want to run 'make' before writing."
fi

# --- find the disk --------------------------------------------------------
if [ -z "$DEVICE" ]; then
    step "Looking for a floppy drive..."
    found=""
    while read -r name size type; do
        [ "$type" = "disk" ] || continue
        [ "$size" = "$FLOPPY_BYTES" ] || continue
        found="/dev/$name"
        break
    done < <(lsblk -b -d -n -o NAME,SIZE,TYPE 2>/dev/null)
    [ -n "$found" ] || die "No 1.44MB device found. Is the drive plugged in with a disk in it?
  Name it yourself if you are sure: $(basename "${BASH_SOURCE[0]}") /dev/sdX"
    DEVICE="$found"
fi

[ -b "$DEVICE" ] || die "$DEVICE is not a block device."

dev_size=$(blockdev --getsize64 "$DEVICE" 2>/dev/null || echo 0)
if [ "$dev_size" -ne "$FLOPPY_BYTES" ]; then
    human=$(numfmt --to=iec --suffix=B "$dev_size" 2>/dev/null || echo "$dev_size bytes")
    die "REFUSING: $DEVICE is $human, not a 1.44MB floppy.
  This script only ever writes to a device of exactly $FLOPPY_BYTES bytes,
  so that a mistyped letter cannot reach a hard disk."
fi

# Belt and braces: never touch something that is part of an array or has
# partitions hanging off it.
if lsblk -n -o NAME "$DEVICE" | tail -n +2 | grep -q .; then
    die "REFUSING: $DEVICE has partitions or holders. That is not a bare floppy."
fi

# Everything above here needed no privilege, which is the point: the
# checks that decide whether this is safe run as you, every time, whether
# or not root is ever involved.
if [ ! -w "$DEVICE" ] && [ "$(id -u)" -ne 0 ]; then
    note ""
    note "$DEVICE is not yours to write, so this needs root."
    note "To stop being asked, once:"
    note "    sudo cp tools/99-flopos-floppy.rules /etc/udev/rules.d/"
    note "    sudo udevadm control --reload"
    note "  then unplug the drive and plug it back in."
    escalate ""
fi

model=$(lsblk -d -n -o MODEL,TRAN "$DEVICE" 2>/dev/null | xargs || true)

# --- say so, loudly -------------------------------------------------------
# Everything above this point is a refusal waiting to happen: the wrong
# size, not a block device, anything with a partition or a holder hanging
# off it, and the script is already dead. So reaching this line means the
# target is a bare device of exactly 1,474,560 bytes and nothing else,
# which is a floppy and cannot be a disk you care about.
#
# It is printed big because the person reading it is tired and the cost of
# skimming past a quiet line is somebody's data.
shout() {
    local green reset
    if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
        green=$'\033[1;92m'; reset=$'\033[0m'
    else
        green=""; reset=""
    fi
    printf '\n%s' "$green"
    if command -v figlet >/dev/null 2>&1; then
        figlet -w "$(tput cols 2>/dev/null || echo 80)" -f big "YES" 2>/dev/null \
            || figlet "YES"
    else
        printf '  YES\n'
    fi
    printf '%s\n' "$reset"
    printf '%s' "$green"
    printf '  THE DISCOVERED DEVICE IS TOTALLY A FLOPPY DISK!\n'
    printf '  Typing "YES" to write to it is TOTALLY SAFE, BOSS.\n'
    printf '%s\n' "$reset"
}
shout

# --- confirm --------------------------------------------------------------
step "About to write FLOPOS to a floppy disk"
note ""
note "    image    $IMAGE"
note "    built    $(date -r "$IMAGE" '+%Y-%m-%d %H:%M')"
note "    device   $DEVICE  (${model:-unknown drive})"
note "    size     $dev_size bytes -- correct for 1.44MB"
if [ "$WRITE_BYTES" -lt "$FLOPPY_BYTES" ]; then
    note "    writing  $WRITE_BYTES bytes -- the rest of the disk is empty"
    note "             (--full writes all $FLOPPY_BYTES)"
fi
note ""
note "  Everything currently on that disk will be gone. The image carries"
note "  its own fresh FAT12 filesystem, so this formats it as it writes."
note ""

if [ "$ASSUME_YES" -ne 1 ]; then
    read -r -p "  Type yes to go ahead: " answer
    [ "$answer" = "yes" ] || die "Nothing written."
fi

# --- take a snapshot, and write from that --------------------------------
# Everything from here uses the copy, so nothing can change the bytes
# half way through a two-minute write.
SNAP="$(mktemp /tmp/flopos-write.XXXXXX.img)"
trap 'rm -f "$SNAP"' EXIT
cp -- "$IMAGE" "$SNAP"
if [ "$(stat -c%s "$SNAP")" -ne "$FLOPPY_BYTES" ]; then
    die "The snapshot is the wrong size. Something is rewriting the image."
fi

# --- unmount, in case the desktop grabbed it ------------------------------
if findmnt -n "$DEVICE" >/dev/null 2>&1; then
    step "Unmounting $DEVICE"
    umount "$DEVICE" || die "Could not unmount $DEVICE. Close anything using it."
fi

# --- keep what was written on the old disk ----------------------------------
# A new image carries empty documents, an empty journal and no taught
# words, so writing it as it is would wipe everything written on the
# floppy since the last upgrade.  Anything of those that is on the floppy
# now, at the size the new image expects, is copied into the snapshot
# first.  A floppy that is not a FLOPOS disk simply has none of them.
# --fresh skips this and writes the image exactly as built.
#
# The temporary folder is only ever used if mktemp really made it, and its
# contents are removed one named file at a time -- never with a wildcard
# on a variable, which with an empty variable is "rm -f /*".
KEEP_FILES="JOURNAL.TXT STATE.BIN WRITE1.MD WRITE2.MD WRITE3.MD WRITE4.MD
            WRITE5.MD WRITE6.MD WRITE7.MD WRITE8.MD BOOKMARK.TXT"
KEEPDIR=""
if [ "$FRESH" -ne 1 ] && command -v mcopy >/dev/null 2>&1; then
    KEEPDIR="$(mktemp -d "${TMPDIR:-/tmp}/flopos-keep.XXXXXX" 2>/dev/null)" || KEEPDIR=""
fi
if [ -n "$KEEPDIR" ] && [ -d "$KEEPDIR" ] && [ "$KEEPDIR" != "/" ]; then
    kept=""
    for f in $KEEP_FILES; do
        if mcopy -n -i "$DEVICE" "::$f" "$KEEPDIR/$f" 2>/dev/null; then
            want=$( { mcopy -n -i "$SNAP" "::$f" - 2>/dev/null || true; } | wc -c)
            have=$(stat -c%s "$KEEPDIR/$f")
            if [ "$want" -gt 0 ] && [ "$want" = "$have" ]; then
                mcopy -o -n -i "$SNAP" "$KEEPDIR/$f" "::$f" && kept="$kept $f"
            fi
        fi
    done
    for f in $KEEP_FILES; do
        rm -f -- "$KEEPDIR/$f"
    done
    rmdir -- "$KEEPDIR" 2>/dev/null || true
    if [ -n "$kept" ]; then
        step "Keeping what is already on the floppy:"
        note "  $kept"
    fi
fi

# --- optional low-level format --------------------------------------------
if [ "$LOWLEVEL" -eq 1 ]; then
    if command -v ufiformat >/dev/null 2>&1; then
        step "Low-level formatting (this takes a couple of minutes)"
        ufiformat -f 1440 "$DEVICE" || die "Low-level format failed. The media may be dead."
    else
        note "ufiformat is not installed; skipping the low-level format."
        note "Install it with: sudo apt install ufiformat"
    fi
fi

# --- write ----------------------------------------------------------------
# bs=36k is two full cylinders per USB transaction. At bs=512 a USB floppy
# spends all its time on per-command latency and takes about four times
# as long for exactly the same result.
step "Writing $WRITE_BYTES bytes"
dd if="$SNAP" of="$DEVICE" bs=$BLOCK count=$((WRITE_BYTES / BLOCK)) \
   conv=fsync status=progress

# --- verify ---------------------------------------------------------------
step "Reading it back to check every sector written"
if cmp -n "$WRITE_BYTES" "$SNAP" "$DEVICE"; then
    note ""
    note "  VERIFIED. All $WRITE_BYTES bytes match."
else
    die "MISMATCH. The disk does not read back the same as what was written.
  The image was snapshotted before the write, so this is the media or the
  drive, not a changing file. Try again; if it fails at the same byte
  twice, use a different diskette."
fi

# --- done -----------------------------------------------------------------
eject "$DEVICE" 2>/dev/null || true
step "Done. Safe to take the disk out."
note ""
