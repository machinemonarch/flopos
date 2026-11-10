# Building and releasing

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
| `make release PART=minor` | Makes a numbered release (below) |
| `make clean` | Removes what was built |

## What is in the project

| Path | What |
|---|---|
| `boot/boot.S` | The boot sector |
| `boot/saffran.pix` | Saffran, the boot picture: palette and pixels, made by `tools/mksplash.py` |
| `boot/roots.dat` | The 119 authorities secure sites are checked against, made by `tools/mkroots.py` |
| `kernel/` | The system: one file per job, each explaining itself at the top |
| `apps/` | Programs that live on the floppy beside the system (see [Making apps](making-apps.md)) |
| `apps/lib/` | Code apps share, built into each app that asks for it (`audio`: songs into samples) |
| `linker.ld` | Where the kernel sits in memory, with checks that it fits |
| `tools/mkimage.py` | Builds the floppy image: a real FAT12 disk that also boots |
| `tools/mkfont.py` | Generates the text faces from readable glyph art |
| `tools/mksplash.py` | Turns a photograph into the boot picture |
| `tools/mkaactab.py` | Makes `aactab.h`, the AAC codebooks, from the VisualOn AAC encoder's tables |
| `tools/mkroots.py` | Turns a Linux PC's list of trusted authorities into `boot/roots.dat` |
| `tools/writefloppy.sh` | Writes the image to a real floppy, safely |
| `tools/99-flopos-floppy.rules` | Lets you write floppies without a password |
| `tools/release.py`, `tools/changelog.py` | Version numbers and release notes |
| `tools/shots.py` | Screenshots from the emulator |
| `tools/wire.py` | Talks to FLOPOS over the serial line and keeps a transcript |
| `tools/wiretest.py`, `tools/persisttest.py` | Boot it in QEMU and check that the serial line works and that saved things survive a reboot. Like `shots.py`, they answer the wizard's riddle first ("a turnip"). |
| `docs/` | This documentation |
| `CHANGELOG.md` | Every release and what changed |
| `releases/` | Every released image and its notes (not in git) |
| `reference/` | Manuals and drivers consulted for facts (not in git, see [References](references.md)) |

## The floppy image

`tools/mkimage.py` builds a genuine 1.44MB FAT12 floppy with `mtools`, so it
can be read by DOS, Windows, Linux or a Mac. On it:

| File | What |
|---|---|
| `SAFFRAN.PIX` | The boot picture, read first. In one unbroken run of sectors. |
| `KERNEL.BIN` | The system, read second. Also in one unbroken run, and both positions are written into the boot sector. |
| `WRITE.APP` | The word processor |
| `BROWZER.APP` | The web browser, which `fetch` also runs |
| `BOOKMARK.TXT` | browzer's bookmarks, 8 KB |
| `ROOTS.DAT` | The authorities secure sites are checked against (55 KB) |
| `JOURNAL.TXT`, `STATE.BIN` | What FLOPOS remembers. Made at full size so they can be written in place. |
| `WRITE1.MD` … `WRITE8.MD` | Documents, 16KB each |
| `README.TXT` | The project's README |

Alongside the image it writes `flopos.img.used`, a list of which parts of
the disk have anything on them, so `writefloppy.sh` only writes those.

## Running it in the emulator

    make run

or by hand:

    qemu-system-i386 -machine pc -cpu pentium -m 32 \
      -drive file=dist/flopos.img,if=floppy,format=raw -boot a \
      -serial stdio

For the network, add an emulated NE2000 card on QEMU's built-in network:

    -netdev user,id=n0 -device ne2k_isa,netdev=n0,iobase=0x300,irq=9

The emulator has no PC Card slots, no 3Com card and no 65550 video chip,
so those parts can only be tested on the laptop. Everything else (boot, the
screen at both sizes, the shell, the disk, the serial line, the whole
network stack and the encryption) is tested in QEMU before a release.

The encryption code was also tested on the PC against OpenSSL and Python
across thousands of random inputs, and ML-KEM against NIST's official test
vectors, before it went into FLOPOS.

## Version numbers

`MAJOR.MINOR.PATCH`, kept in `VERSION`:

- **minor** (1.10.0 → 1.11.0) for new things
- **patch** (1.10.0 → 1.10.1) for fixes

`version` says which one is running.

## Making a release

1. Describe the changes under `## Unreleased` at the top of
   `CHANGELOG.md`. Write for the person using FLOPOS, not for a
   programmer.
2. Run `make release PART=minor` (or `PART=patch`).

The release refuses to start if `## Unreleased` is empty. Otherwise it:

1. bumps the version and regenerates `kernel/version.h`
2. builds everything from clean
3. copies the image to `releases/flopos-X.Y.Z.img`, with its `.used` file
4. moves the Unreleased notes under the new version number with today's
   date, and writes them beside the image as `releases/flopos-X.Y.Z.txt`

Any old version can be put back on a floppy from `releases/`.
