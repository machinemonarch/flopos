#!/usr/bin/env python3
"""Build dist/flopos.img: a real FAT12 1.44MB floppy that also boots.

The disk is formatted with mformat and the kernel is copied on with mcopy,
so what comes out is an ordinary DOS floppy -- you can mount it, read it in
Windows, and copy your own files into the free space. The boot sector is
then dropped into sector 0 with mformat's own BPB spliced into it, and the
placeholder words in the boot code are patched with the real locations of
SAFFRAN.PIX (the boot picture) and KERNEL.BIN as the filesystem laid them
out.

Nothing here hardcodes a cluster number: the FAT chain is walked and
checked, so if a future kernel grows past a track boundary or lands
somewhere unexpected the build fails loudly instead of shipping a floppy
that does not boot.
"""
import os
import struct
import subprocess
import sys

SECTOR = 512
TOTAL_SECTORS = 2880            # 1.44MB: 80 cylinders * 2 heads * 18 sectors

# Two files the OS writes back into, each a fixed size so that it only ever
# overwrites their data sectors and never has to touch the FAT or the
# directory. That is what lets the kernel do without a filesystem driver.
JOURNAL_BYTES = 8192
STATE_BYTES = 8192

BOOKMARK_BYTES = 8192

JOURNAL_HEADER = (
    "FLOPOS journal\r\n"
    "\r\n"
    "Everything below this line was written by the machine itself.\r\n"
    "Words it did not know are recorded here.\r\n"
    "\r\n"
)

# The kernel carries this signature in its data; the four numbers that
# follow it are where these two files ended up.
DISKMAP_MAGIC = b"FLOPOS-DISKMAP!\x00"

# The boot sector reserves these words for us; mkimage finds them by
# their placeholder values rather than by a hardcoded offset.  The first
# pair is the boot picture, loaded first; the second is the kernel.
MAGIC_LBA = 0xAA55
MAGIC_CNT = 0x55AA
MAGIC_KLBA = 0xBB66
MAGIC_KCNT = 0x66BB

# Saffran: 768 bytes of palette, then 320x200 pixels.  Made from a photo
# by tools/build/mksplash.py.
SAFFRAN = "data/saffran.pix"
SAFFRAN_BYTES = 768 + 64000


def run(*cmd):
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0:
        sys.exit("%s failed:\n%s%s" % (cmd[0], p.stdout, p.stderr))
    return p.stdout


def find_word(buf, value, lo, hi):
    """Offset of a little-endian u16 equal to value, searched in [lo, hi)."""
    want = struct.pack("<H", value)
    hits = [i for i in range(lo, hi - 1) if buf[i:i + 2] == want]
    if len(hits) != 1:
        sys.exit("expected exactly one 0x%04X placeholder in the boot sector, "
                 "found %d" % (value, len(hits)))
    return hits[0]


def main():
    if len(sys.argv) < 4:
        sys.exit("usage: mkimage.py boot.bin kernel.bin out.img [app.app ...]")
    boot_path, kern_path, out_path = sys.argv[1:4]
    app_paths = sys.argv[4:]

    boot = bytearray(open(boot_path, "rb").read())
    kern = open(kern_path, "rb").read()
    if len(boot) != SECTOR:
        sys.exit("boot sector is %d bytes, must be %d" % (len(boot), SECTOR))

    # ---- lay down a stock FAT12 floppy ---------------------------------
    if os.path.exists(out_path):
        os.remove(out_path)
    with open(out_path, "wb") as f:
        f.truncate(TOTAL_SECTORS * SECTOR)
    # A fixed volume serial. mformat randomises it otherwise, which makes
    # two builds of identical source differ at byte 40 -- and makes any
    # byte-for-byte verification of a written floppy meaningless.
    run("mformat", "-i", out_path, "-f", "1440", "-v", "FLOPOS",
        "-N", "5D0F05E5", "::")

    # The boot sector reads these two, in this order, so they go on first:
    # each lands in one piece, and the drive reads straight on from the
    # picture into the kernel without seeking back.
    if os.path.getsize(SAFFRAN) != SAFFRAN_BYTES:
        sys.exit("%s should be %d bytes" % (SAFFRAN, SAFFRAN_BYTES))
    run("mcopy", "-i", out_path, SAFFRAN, "::SAFFRAN.PIX")
    run("mcopy", "-i", out_path, kern_path, "::KERNEL.BIN")
    # The two writable files go on next, while the disk is still empty
    # enough that they are certain to land in one piece.
    import tempfile
    tmpdir = tempfile.mkdtemp()
    jpath = os.path.join(tmpdir, "JOURNAL.TXT")
    spath = os.path.join(tmpdir, "STATE.BIN")
    jdata = JOURNAL_HEADER.encode("ascii")
    jdata += b"\x1A" + b"\x00" * (JOURNAL_BYTES - len(jdata) - 1)
    open(jpath, "wb").write(jdata)
    open(spath, "wb").write(b"\x00" * STATE_BYTES)
    run("mcopy", "-i", out_path, jpath, "::JOURNAL.TXT")
    run("mcopy", "-i", out_path, spath, "::STATE.BIN")

    readme = "README.md"
    if os.path.exists(readme):
        run("mcopy", "-i", out_path, readme, "::README.TXT")

    # The authorities secure sites are checked against (tools/build/mkroots.py).
    run("mcopy", "-i", out_path, "data/roots.dat", "::ROOTS.DAT")

    # The wizard's way with words (tools/wizard), read once at boot.
    run("mcopy", "-i", out_path, "data/wizard.wiz", "::WIZARD.WIZ")
    # Ozzy, who sees the machine off on bye and reboot (src/kernel/arrive.c).
    if os.path.getsize("data/ozzy.pix") != SAFFRAN_BYTES:
        sys.exit("data/ozzy.pix should be %d bytes" % SAFFRAN_BYTES)
    run("mcopy", "-i", out_path, "data/ozzy.pix", "::OZZY.PIX")
    # The script room's engine (src/room/), for browzer's JavaScript.
    room = os.path.join(os.path.dirname(kern_path), "room.bin")
    if os.path.exists(room):
        run("mcopy", "-i", out_path, room, "::ROOM.BIN")

    # Apps, each its own file: SCRIBE.APP for `scribe`.  The kernel finds
    # them by name when they are asked for, so where they land is theirs.
    # A name over eight letters (INVENTORY.APP) goes on whole, as a long
    # name, and FAT12 gives it a short one beside it (INVENT~1.APP).
    for app in app_paths:
        name = os.path.splitext(os.path.basename(app))[0].upper() + ".APP"
        run("mcopy", "-i", out_path, app, "::" + name)

    # The inventory: the one folder that is the user's.  Everything they
    # make lives in it, as ordinary files any computer can read, and the
    # system's own files above it are never disturbed.  Empty to begin
    # with.  (Its name is nine letters, so the floppy also carries the
    # short name INVENT~1, which is the one FLOPOS looks for.)
    run("mmd", "-i", out_path, "::INVENTORY")

    # browzer's bookmarks: plain text, one a line, made at full size for
    # the kernel only ever writes it in place.
    bpath = os.path.join(tmpdir, "BOOKMARK.TXT")
    open(bpath, "wb").write(b"\x1A" + b"\x00" * (BOOKMARK_BYTES - 1))
    run("mcopy", "-i", out_path, bpath, "::BOOKMARK.TXT")

    # The sites browzer may run scripts on: none until one is allowed (j),
    # a name a line, padded with spaces; written in place like the rest.
    spath = os.path.join(tmpdir, "SITES.TXT")
    open(spath, "wb").write(b" " * 2048)
    run("mcopy", "-i", out_path, spath, "::SITES.TXT")

    img = bytearray(open(out_path, "rb").read())

    # ---- read back the geometry mformat chose --------------------------
    bps        = struct.unpack_from("<H", img, 11)[0]
    spc        = img[13]
    reserved   = struct.unpack_from("<H", img, 14)[0]
    num_fats   = img[16]
    root_ents  = struct.unpack_from("<H", img, 17)[0]
    sec_per_fat = struct.unpack_from("<H", img, 22)[0]

    if bps != SECTOR:
        sys.exit("unexpected sector size %d" % bps)

    fat_start  = reserved
    root_start = fat_start + num_fats * sec_per_fat
    root_sectors = (root_ents * 32 + SECTOR - 1) // SECTOR
    data_start = root_start + root_sectors

    def fat_entry(n):
        off = fat_start * SECTOR + (n * 3) // 2
        v = struct.unpack_from("<H", img, off)[0]
        return (v >> 4) if (n & 1) else (v & 0x0FFF)

    def locate(fatname, expect_size=None):
        """Starting sector and length of a file, insisting it is in one
        piece -- nothing here can follow a fragmented chain."""
        entry = None
        for i in range(root_ents):
            off = root_start * SECTOR + i * 32
            name = bytes(img[off:off + 11])
            if name[0] == 0x00:
                break
            if name[0] == 0xE5:
                continue
            if img[off + 11] & 0x08 and not (img[off + 11] & 0x10):
                continue                          # volume label
            if name == fatname:
                entry = off
                break
        if entry is None:
            sys.exit("%s is not in the root directory" % fatname.decode())

        first = struct.unpack_from("<H", img, entry + 26)[0]
        size  = struct.unpack_from("<I", img, entry + 28)[0]
        if expect_size is not None and size != expect_size:
            sys.exit("%s is %d bytes, expected %d"
                     % (fatname.decode(), size, expect_size))

        chain, c, guard = [], first, 0
        while 2 <= c < 0xFF0:
            chain.append(c)
            c = fat_entry(c)
            guard += 1
            if guard > TOTAL_SECTORS:
                sys.exit("FAT chain for %s does not terminate" % fatname.decode())

        want = (size + spc * SECTOR - 1) // (spc * SECTOR)
        if len(chain) != want:
            sys.exit("%s occupies %d clusters, expected %d"
                     % (fatname.decode(), len(chain), want))
        for a, b in zip(chain, chain[1:]):
            if b != a + 1:
                sys.exit("%s is fragmented (cluster %d -> %d); nothing here "
                         "can read it that way" % (fatname.decode(), a, b))

        return data_start + (chain[0] - 2) * spc, (size + SECTOR - 1) // SECTOR

    saf_lba,  saf_sectors  = locate(b"SAFFRAN PIX", SAFFRAN_BYTES)
    kern_lba, kern_sectors = locate(b"KERNEL  BIN", len(kern))
    jrn_lba,  jrn_sectors  = locate(b"JOURNAL TXT", JOURNAL_BYTES)
    st_lba,   st_sectors   = locate(b"STATE   BIN", STATE_BYTES)

    # ---- tell the kernel where they are -------------------------------
    at = img.find(DISKMAP_MAGIC)
    if at < 0:
        sys.exit("the kernel carries no disk map signature")
    if img.find(DISKMAP_MAGIC, at + 1) >= 0:
        sys.exit("the disk map signature appears more than once")
    struct.pack_into("<IIII", img, at + len(DISKMAP_MAGIC),
                     jrn_lba, jrn_sectors, st_lba, st_sectors)
    if kern_lba + kern_sectors > TOTAL_SECTORS:
        sys.exit("kernel runs off the end of the disk")

    # The boot sector reads the kernel into 1000:0000, and the picture into
    # 8000:0000 before that; the kernel must stay below both.
    if 0x10000 + kern_sectors * SECTOR > 0x80000:
        sys.exit("kernel is too big to load below the picture and the stack")

    # ---- splice mformat's BPB into our boot code and patch it ---------
    boot[3:62] = img[3:62]                        # keep the real BPB
    off_lba = find_word(boot, MAGIC_LBA, 62, 510)
    off_cnt = find_word(boot, MAGIC_CNT, 62, 510)
    off_klba = find_word(boot, MAGIC_KLBA, 62, 510)
    off_kcnt = find_word(boot, MAGIC_KCNT, 62, 510)
    struct.pack_into("<H", boot, off_lba, saf_lba)
    struct.pack_into("<H", boot, off_cnt, saf_sectors)
    struct.pack_into("<H", boot, off_klba, kern_lba)
    struct.pack_into("<H", boot, off_kcnt, kern_sectors)
    boot[510:512] = b"\x55\xAA"

    img[0:SECTOR] = boot
    with open(out_path, "wb") as f:
        f.write(img)

    used = (kern_sectors + jrn_sectors + st_sectors) * SECTOR
    # How much of the disk actually matters.  Everything past the last
    # allocated sector is empty filesystem: the FAT and the directory say
    # those clusters are free, so whatever is already on the diskette
    # there is unreferenced and harmless.  Writing it anyway costs about
    # forty-five seconds of a drive that manages 28 kilobytes a second,
    # twice, because it is verified afterwards as well.
    #
    # Rounded up to a whole track, because that is the unit a floppy
    # actually reads and writes.
    # Taken from the allocation table rather than from the three files we
    # happen to know about, because the image also carries a README and
    # anything else added later would be missed the same way.
    last = 1
    for n in range(2, (TOTAL_SECTORS - data_start) // spc + 2):
        if fat_entry(n):
            last = n
    used = data_start + (last - 1) * spc
    track = 18 * SECTOR
    used_bytes = ((used * SECTOR + track - 1) // track) * track
    with open(out_path + ".used", "w") as f:
        f.write("%d\n" % used_bytes)

    print("   %s" % out_path)
    print("   saffran     %6d bytes  (%d sectors) at LBA %d"
          % (SAFFRAN_BYTES, saf_sectors, saf_lba))
    print("   kernel      %6d bytes  (%d sectors) at LBA %d"
          % (len(kern), kern_sectors, kern_lba))
    print("   journal     %6d bytes  (%d sectors) at LBA %d"
          % (JOURNAL_BYTES, jrn_sectors, jrn_lba))
    print("   state       %6d bytes  (%d sectors) at LBA %d"
          % (STATE_BYTES, st_sectors, st_lba))
    print("   disk map patched into the kernel at image offset %d" % at)
    print("   boot sector patched: picture @%d/%d, kernel @%d/%d"
          % (off_lba, off_cnt, off_klba, off_kcnt))
    print("   in use      %6d bytes  (%d%% of the disk)"
          % (used_bytes, 100 * used_bytes // (TOTAL_SECTORS * SECTOR)))
    print("   FAT12 free  %6d bytes" %
          ((TOTAL_SECTORS * SECTOR) - (data_start * SECTOR) - used))


if __name__ == "__main__":
    main()
