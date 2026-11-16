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
  and GNU `as` and `ld`.
- `mtools`, which builds the FAT12 floppy image without mounting anything.
- `python3`
- `qemu-system-i386`, to run FLOPOS without the laptop.
- Optional: `picocom` for the serial line, Python's Pillow for screenshots,
  `openssl` for testing secure connections locally.

## Make

| Command | What it does |
|---|---|
| `make` | Builds `dist/flopos.img`: boot sector, kernel, apps, and the files that are kept |
| `make run` | Boots it in QEMU in a window, with sound |
| `make shot` | Boots it without a window and saves screenshots to `dist/shots/` |
| `make size` | Where the kernel's bytes went |
| `make write` | Writes it to a floppy (same as `tools/floppy/writefloppy.sh`) |
| `make release PART=minor` | Makes a numbered release (Releasing[^2]) |
| `make clean` | Removes what was built |

**Build example:**

    make                      build dist/flopos.img
    make run                  boot it in QEMU
    tools/floppy/writefloppy.sh   put it on a real floppy


## What is in the project

Every file has one home, by what it is:

- **`src/`**: The code that runs on the laptop: `boot/` (the boot sector), `kernel/` (the system, one file per job, and `linker.ld`), `room/` (the script room's engine) and `apps/` (one folder per program, see Making apps[^3]).
- **`src/apps/sdk/`**: What every app is built with: `app.h`, `crt.c`, `app.ld`.
- **`src/apps/lib/`**: Code apps share, built into each app that asks for it (`audio`, `dsp`, `bits`, `tape`).
- **`gen/`**: Beside the code it belongs to, what a tool made: the faces and keyboard layouts (`src/kernel/gen/`), the apps' glyphs (`src/apps/sdk/gen/`), the decoders' tables (`src/apps/lib/audio/gen/`). Never edited by hand; each file names the tool that made it.
- **`data/`**: What goes on the floppy beside the system: Saffran and Ozzy, the 119 authorities secure sites are checked against, and the wizard's words.
- **`tools/build/`**: What the build runs, and what makes `gen/` and `data/`: `mkimage.py` (the floppy image), `mkfont.py` and `fontwide.py` (the faces), `mkkeymap.py`, `mksplash.py`, `mkroots.py`, `mkaactab.py`, `mkmp3tab.py`.
- **`tools/floppy/`**: Writing a real floppy: `writefloppy.sh`, `keepfiles.sh` (carries your files over), and the udev rule that lets you write without a password.
- **`tools/release/`**: Version numbers and release notes.
- **`tools/wizard/`**: Training the wizard (Training the wizard[^4]).
- **`test/`**: Every test, one folder each, and the emulator helpers: `shots.py` (screenshots), `wire.py` (the serial line), `bench/` (a scripted QEMU run and its test site). See Testing[^1].
- **`docs/`**: This documentation.
- **`CHANGELOG.md`**: Every release and what changed.
- **`dist/`**, **`releases/`**: What was built, and every released image (not in git).

## The floppy image

`tools/build/mkimage.py` builds a genuine 1.44MB FAT12 floppy with `mtools`, so it
can be read by DOS, Windows, Linux or a Mac. On it:

- **`SAFFRAN.PIX`**: The boot picture, read first. In one unbroken run of sectors.
- **`OZZY.PIX`**: Ozzy, shown on `bye` and `reboot`, made like Saffran's picture by `tools/build/mksplash.py`.
- **`KERNEL.BIN`**: The system, read second. Also in one unbroken run, and both positions are written into the boot sector.
- **`SCRIBE.APP`**: The word processor.
- **`BROWZER.APP`**: The web browser.
- **`BOOKMARK.TXT`**: Browzer's bookmarks, 8 KB.
- **`ROOTS.DAT`**: The authorities secure sites are checked against (55 KB).
- **`JOURNAL.TXT`, `STATE.BIN`**: What FLOPOS remembers. Made at full size so they can be written in place.
- **`INVENTORY` (folder)**: Everything the user makes. Empty in a new image.
- **`INVENTORY.APP`, `MAELCUM.APP`**: The inventory and the cassette backup.
- **`README.TXT`**: The project's README.

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
[^4]: [Training the wizard](training-the-wizard.md)
