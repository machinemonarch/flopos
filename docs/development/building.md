# Building

How to build FLOPOS and run it without the laptop. Tests are in
Testing[^1], releases in Releasing[^2].

**If the build fails, please tell Max:** open an issue on GitHub, or write
to hello@machinemonarch.com with a subject starting with FLOPOS, and
include the last lines the build printed.

## What you need

On a Linux PC:

- `gcc` able to build 32-bit code (Debian and Ubuntu: `gcc-multilib`), GNU `as` and `ld`.
- `mtools`, which builds the floppy image without mounting anything.
- `python3`.
- `qemu-system-i386`, to run FLOPOS without the laptop.
- Optional: Pillow for screenshots, `openssl` for the TLS tests.

## Make

- **`make`:** Builds `dist/flopos.img`.
- **`make run`:** Boots it in QEMU in a window, with sound.
- **`make shot`:** Boots it without a window and saves screenshots to `build/shots/`.
- **`make write`:** Writes it to a floppy (as `tools/floppy/writefloppy.sh`).
- **`make wire`:** Talks to the laptop over the serial line, adding to `logs/serial.log`.
- **`make size`:** Where the kernel's bytes went.
- **`make release PART=minor`:** A numbered release.
- **`make clean`:** Removes what was built.

## What is in the project

Every file has one home, by what it is:

- **`src/`**: The code that runs on the laptop: `boot/` (the boot sector), `kernel/` (the system, one file per job, and `linker.ld`), `room/` (the JavaScript engine) and `apps/` (one folder per program, see Making apps[^3]).
- **`src/apps/sdk/`**: What every app is built with: `app.h`, `crt.c`, `app.ld`.
- **`src/apps/lib/`**: Code apps share (`audio`, `dsp`, `bits`, `tape`).
- **`gen/`**: Beside the code it belongs to, what a tool made: faces and keyboard layouts (`src/kernel/gen/`), the apps' glyphs (`src/apps/sdk/gen/`), the decoders' tables (`src/apps/lib/audio/gen/`). Never edited by hand.
- **`data/`**: What goes on the floppy beside the system: Saffran and Ozzy, the trusted authorities, the wizard's words, and the help text.
- **`tools/build/`**: What the build runs and what makes `gen/` and `data/`: `mkimage.py`, `mkfont.py` and `fontwide.py`, `mkkeymap.py`, `mksplash.py`, `mkroots.py`, `mkaactab.py`, `mkmp3tab.py`.
- **`tools/floppy/`**: Writing a real floppy: `writefloppy.sh`, `keepfiles.sh` (carries your files over), and the udev rule.
- **`tools/laptop/`**: `wire.py`, the serial line.
- **`tools/release/`**: Version numbers and release notes.
- **`tools/wizard/`**: Training the wizard[^4].
- **`test/`**: Every test, one folder each, and the emulator helpers (`shots.py`, `bench/`).
- **`build/`**: Everything compiled, the tests' too, one folder per test (not in git).
- **`dist/`**: The finished floppy image (not in git).
- **`releases/`**: Every released image (not in git).
- **`logs/`**: The serial log (not in git).

## The floppy image

`tools/build/mkimage.py` builds a real FAT12 floppy with `mtools`, readable
by any computer:

- **`SAFFRAN.PIX`, `KERNEL.BIN`:** The boot picture and the system, each in one unbroken run of sectors whose position is written into the boot sector.
- **`OZZY.PIX`:** Ozzy, shown on `bye` and `reboot`.
- **`*.APP`:** The programs.
- **`ROOM.BIN`:** The JavaScript engine.
- **`WIZARD.WIZ`:** The wizard's model.
- **`ROOTS.DAT`:** The trusted authorities.
- **`JOURNAL.TXT`, `STATE.BIN`, `BOOKMARK.TXT`, `SITES.TXT`:** What FLOPOS remembers, made at full size to be written in place.
- **`INVENTORY`:** The user's folder, empty in a new image.
- **`HELP.TXT`:** What `WORD help` shows, from `data/help.txt`.
- **`README.TXT`:** The project's README.

Beside the image, `flopos.img.used` says which parts of the disk are in
use, so `writefloppy.sh` writes only those.

## Running it in the emulator

    make run

or by hand, with the emulator's network card:

    qemu-system-i386 -machine pc -cpu pentium -m 32 \
      -drive file=dist/flopos.img,if=floppy,format=raw -boot a \
      -serial stdio \
      -netdev user,id=n0 -device ne2k_isa,netdev=n0,iobase=0x300,irq=9

QEMU has no PC Card slots, so FLOPOS drives an NE2000 there instead.

[^1]: [Testing](testing.md)
[^2]: [Releasing](releasing.md)
[^3]: [Making apps](making-apps.md)
[^4]: [Training the wizard](training-the-wizard.md)
