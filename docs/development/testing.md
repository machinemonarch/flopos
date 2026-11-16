# Testing

Every test lives in `test/`, one folder each, and prints `ok` or
`FAILED`. Each builds into its own folder under `build/`. Reuse and extend them;
never write a throwaway harness.

## What is tested where

QEMU has no PC Card slots, 3Com card or 65550 video chip, so those are
tested on the laptop. Everything else (boot, both screen sizes, shell,
disk, serial, network, encryption) is tested in QEMU.

On the laptop you need floppies and a drive to write them, a null-modem
serial cable with a USB adapter (`make wire`), the 3Com card and a cable
to a router, and for maelcum a cassette recorder, two audio cables and a
printer.

## Running at the laptop's speed

    qemu-system-i386 ... -icount shift=4,sleep=on,align=on

holds the emulator to about the laptop's speed; `align=on` keeps its clock
with the real one, or real-world radio arrives ten times too slowly.
`ip drop 10` makes FLOPOS drop every tenth incoming frame, as a real card
might; `ip drop 0` stops it.

## On this PC

- **`test/jstest/run.sh`:** The JavaScript engine, on every test in `test/jstest/tests`, then again under memory stress.
- **`test/fuzz/run.py [--runs N]`:** Damaged inputs for the readers, under the compiler's memory checker: `audio`, `tracker`, `x509`, `tls` (recorded conversations played back damaged) and `browzer` (the whole app on a pretend system). Finds are kept in `build/fuzz/finds`. Songs come from `~/.cache/flopos-songs`, recordings from `~/.cache/flopos-fuzz/audio`. Run after changing a decoder, page reading, or TLS.
- **`test/fstest/run.py`:** `src/kernel/fs.c` on a copy of the image: thousands of random changes judged by `fsck.fat` and mtools, then random power cuts. Run after changing `fs.c`.
- **`test/keytest/run.sh`:** Every keyboard layout, dead keys, Alt+Shift, Ctrl shortcuts, that the font has every letter, and Hebrew's drawing order.
- **`test/tapetest/run.sh`:** Cassette sound through a pretend tape (off speed, wavering, quiet, noisy, dull, upside down, a dropout) and back.
- **`test/keeptest.sh`:** What `writefloppy.sh` does to carry the user's files to a new release.
- **`test/doccheck.py`:** Every link in the Markdown leads to a page and heading.
- **`test/audiotest/`:** `render.c` plays a tracker song through FLOPOS's tracker; `compare.py` checks it against libopenmpt; `fetch.py` fetches the songs in `songs.txt` (never kept here).
- **`tools/wizard/enginetest.py`:** The wizard's engine against the Python version.

## In the emulator

- **`test/bench/run.py IMAGE OUT STEP ...`:** Boots the image, types and presses keys, and checks each step against what FLOPOS says on its serial line (`key:ctrl-shift-f10`, `type:whoami\n=>alright`). It waits on FLOPOS's messages, with the clock running ahead; a full check takes about half a minute. `--slow` holds it to the laptop's speed. It records everything the Sound Blaster played.
- **`test/bench/site.py`:** A small local site imitating pages browzer has had trouble with; `--site SONGS` starts it.
- **`test/bench/tlstest.py IMAGE WORKDIR`:** A test authority and six local OpenSSL servers, every TLS form FLOPOS takes and one it must refuse.
- **`test/roomtest/run.py`, `test/scripttest/run.py`:** The script room, and scripts on the test site's pages.
- **`test/walltest/run.py`:** An app that breaks each of the wall's rules (`rogue.c`); FLOPOS must close it and carry on.
- **`test/wiretest.py`, `test/persisttest.py`:** The serial line, and that saved things survive a reboot.
- **`test/shots.py`:** Screenshots (`make shot`).

maelcum is tried in a test build (`make EXTRA_CFLAGS=-DFLOPOS_TEST`), whose
sound driver plays back what it recorded: with `run.py`,
`type:maelcum save\n`, `key:ret=>\[tape\] saved files 3`, `key:esc`,
`type:maelcum load\n=>\[tape\] same STATE.BIN`. Delete the bench's large
`sound.wav` after.
