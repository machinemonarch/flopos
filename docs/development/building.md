# Building

How to build FLOPOS and run it without the laptop. Tests are in
Testing[^1], numbered releases in Releasing[^2].

**If the build fails, please tell Max:** open an issue on GitHub, or write
to hello@machinemonarch.com with a subject that starts with FLOPOS, and
include the last lines the build printed. So far FLOPOS has been built on
one Linux PC only, so your report is likely the first of its kind.

## What you need

On a Linux PC:

- `gcc` able to build 32-bit code (on Debian and Ubuntu: `gcc-multilib`),
  and GNU `as` and `ld`
- `mtools`, which builds the FAT12 floppy image without mounting anything
- `python3`
- `qemu-system-i386`, to run FLOPOS without the laptop
- optional: `picocom` for the serial line, Python's Pillow for screenshots,
  `openssl` for testing secure connections locally

## Make

| Command | What it does |
|---|---|
| `make` | Builds `dist/flopos.img`: boot sector, kernel, apps, and the files that are kept |
| `make run` | Boots it in QEMU in a window, with sound |
| `make shot` | Boots it without a window and saves screenshots to `dist/shots/` |
| `make size` | Where the kernel's bytes went |
| `make write` | Writes it to a floppy (same as `tools/writefloppy.sh`) |
| `make release PART=minor` | Makes a numbered release (Releasing[^2]) |
| `make clean` | Removes what was built |

## What is in the project

- **`boot/boot.S`**: The boot sector
- **`boot/saffran.pix`**: Saffran, the boot picture: palette and pixels, made by `tools/mksplash.py`
- **`boot/roots.dat`**: The 119 authorities secure sites are checked against, made by `tools/mkroots.py`
- **`kernel/`**: The system: one file per job, each explaining itself at the top
- **`apps/`**: Programs that live on the floppy beside the system (see Making apps[^3])
- **`apps/lib/`**: Code apps share, built into each app that asks for it (`audio`: songs into samples)
- **`linker.ld`**: Where the kernel sits in memory, with checks that it fits
- **`tools/mkimage.py`**: Builds the floppy image: a real FAT12 disk that also boots
- **`tools/mkfont.py`, `tools/fontwide.py`**: Generate the text faces from readable glyph art (`fontwide.py` holds the Romanian, Cyrillic, Hebrew and Yiddish letters), and `apps/app_glyphs.h` for the apps
- **`tools/mkkeymap.py`**: Generates the keyboard layouts (`kernel/keymaps.c`) from the keys written out row by row
- **`tools/mksplash.py`**: Turns a photograph into the boot picture
- **`tools/mkaactab.py`**: Makes `aactab.h`, the AAC codebooks, from the VisualOn AAC encoder's tables
- **`tools/mkroots.py`**: Turns a Linux PC's list of trusted authorities into `boot/roots.dat`
- **`tools/writefloppy.sh`**: Writes the image to a real floppy, safely
- **`tools/99-flopos-floppy.rules`**: Lets you write floppies without a password
- **`tools/release.py`, `tools/changelog.py`**: Version numbers and release notes
- **`tools/shots.py`**: Screenshots from the emulator
- **`tools/wire.py`**: Talks to FLOPOS over the serial line and keeps a transcript
- **`tools/wiretest.py`, `tools/persisttest.py`**: Boot it in QEMU and check that the serial line works and that saved things survive a reboot. Like `shots.py`, they answer the wizard's riddle first ("a turnip").
- **`docs/`**: This documentation
- **`CHANGELOG.md`**: Every release and what changed
- **`releases/`**: Every released image and its notes (not in git)

## The floppy image

`tools/mkimage.py` builds a genuine 1.44MB FAT12 floppy with `mtools`, so it
can be read by DOS, Windows, Linux or a Mac. On it:

- **`SAFFRAN.PIX`**: The boot picture, read first. In one unbroken run of sectors.
- **`KERNEL.BIN`**: The system, read second. Also in one unbroken run, and both positions are written into the boot sector.
- **`SCRIBE.APP`**: The word processor
- **`BROWZER.APP`**: The web browser
- **`BOOKMARK.TXT`**: browzer's bookmarks, 8 KB
- **`ROOTS.DAT`**: The authorities secure sites are checked against (55 KB)
- **`JOURNAL.TXT`, `STATE.BIN`**: What FLOPOS remembers. Made at full size so they can be written in place.
- **`INVENTORY` (folder)**: Everything the user makes. Empty in a new image.
- **`INVENTORY.APP`, `MAELCUM.APP`**: The inventory and the cassette backup
- **`README.TXT`**: The project's README

Alongside the image it writes `flopos.img.used`, a list of which parts of
the disk have anything on them, so `writefloppy.sh` only writes those.

## Running it in the emulator

    make run

or by hand:

    qemu-system-i386 -machine pc -cpu pentium -m 32 \
      -drive file=dist/flopos.img,if=floppy,format=raw -boot a \
      -serial stdio

In the QEMU emulator there are no PC Card slots, so FLOPOS also has a driver
for an emulated **NE2000** card. It is only looked for when the machine has
no PC Card adapter. For the network, add one on QEMU's built-in network:

    -netdev user,id=n0 -device ne2k_isa,netdev=n0,iobase=0x300,irq=9

[^1]: [Testing](testing.md)
[^2]: [Releasing](releasing.md)
[^3]: [Making apps](making-apps.md)
