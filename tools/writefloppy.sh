#!/usr/bin/env bash
#
# writefloppy.sh -- put FLOPOS on a real floppy disk, safely.
#
#   tools/writefloppy.sh                 find the floppy, confirm, write
#   tools/writefloppy.sh /dev/sde        name the device yourself
#   tools/writefloppy.sh -y              skip the confirmation
#   tools/writefloppy.sh --lowlevel      low-level format first (slow)
#
# It asks for your password once, at the start, and does everything else
# itself.
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

die()  { printf '\n  %s\n\n' "$*" >&2; exit 1; }
note() { printf '  %s\n' "$*"; }
step() { printf '\n  %s\n' "$*"; }

while [ $# -gt 0 ]; do
    case "$1" in
        -y|--yes)      ASSUME_YES=1 ;;
        --lowlevel)    LOWLEVEL=1 ;;
        -i|--image)    shift; IMAGE="${1:-}" ;;
        -h|--help)     sed -n '2,20p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        /dev/*)        DEVICE="$1" ;;
        *)             die "I don't understand '$1'. Try --help." ;;
    esac
    shift
done

# --- one password prompt, at the top -------------------------------------
if [ "$(id -u)" -ne 0 ]; then
    args=()
    if [ -n "$DEVICE" ];        then args+=("$DEVICE");   fi
    if [ "$ASSUME_YES" -eq 1 ]; then args+=(-y);          fi
    if [ "$LOWLEVEL" -eq 1 ];   then args+=(--lowlevel);  fi
    args+=(--image "$IMAGE")
    note "This needs root to write to a disk device. Asking once."
    exec sudo -- "${BASH_SOURCE[0]}" "${args[@]}"
fi

# --- the image ------------------------------------------------------------
[ -f "$IMAGE" ] || die "No image at $IMAGE. Run 'make' first."

img_size=$(stat -c%s "$IMAGE")
[ "$img_size" -eq "$FLOPPY_BYTES" ] \
    || die "$IMAGE is $img_size bytes, not $FLOPPY_BYTES. That is not a floppy image."

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
    note "Found $DEVICE"
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

model=$(lsblk -d -n -o MODEL,TRAN "$DEVICE" 2>/dev/null | xargs || true)

# --- confirm --------------------------------------------------------------
step "About to write FLOPOS to a floppy disk"
note ""
note "    image    $IMAGE"
note "    built    $(date -r "$IMAGE" '+%Y-%m-%d %H:%M')"
note "    device   $DEVICE  (${model:-unknown drive})"
note "    size     $dev_size bytes -- correct for 1.44MB"
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
step "Writing $dev_size bytes"
dd if="$SNAP" of="$DEVICE" bs=36k conv=fsync status=progress

# --- verify ---------------------------------------------------------------
step "Reading it back to check every sector"
if cmp "$SNAP" "$DEVICE"; then
    note ""
    note "  VERIFIED. All $dev_size bytes match."
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
