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
| `BROWZER.APP` | The web browser |
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

### As slow as the laptop

The emulator normally runs far faster than a 120 MHz Pentium, which hides
anything that only goes wrong on a slow machine. To hold it to about the
laptop's speed:

    qemu-system-i386 ... -icount shift=4,sleep=on,align=on

`align=on` keeps the emulated clock with the real one. Without it, a radio
stream from the real world arrives ten times too slowly.

The emulator's network card never loses anything, and a real 3Com does.
`net drop 10` makes FLOPOS throw away every tenth frame that arrives, to
test recovery; `net drop 0` stops it. While sound plays, the serial cable
reports every ten seconds what decoding costs, the screen's frame rate and
how much music is in hand (see [techniques.md](techniques.md)).

## The test tools

Kept here, so they are never written again:

- `tools/audiotest/render.c` plays a tracker song on this PC through
  FLOPOS's own tracker; `compare.py` holds the result up against
  libopenmpt. `songs.txt` lists the songs from The Mod Archive that test
  it, and `fetch.py` fetches them (they are their authors', so they are
  never kept here).
- `tools/bench/site.py` is a small web site that imitates the sites
  browzer has had trouble with: The Mod Archive's downloads, DuckDuckGo's
  front page, ERICADE Radio, 72fm.com (and the Radio Browser directory
  its country pages ask) and an AzuraCast station.
- `tools/bench/run.py` boots the image in the emulator, opens pages and
  presses keys, and checks each step against what FLOPOS says on its
  serial wire. It prints a line per step: ok, or what was wrong.
  browzer writes a line there for each page it gets (`[page] ...`) and the
  HUD for each time it closes (`[hud] ...`). It also records everything
  the Sound Blaster played, which `findsong.py` can find a song in.
- `tools/bench/tlstest.py` makes a test authority, an image that trusts it,
  and six local OpenSSL servers (TLS 1.2 in each form FLOPOS takes, TLS
  1.3, and a TLS 1.1 one that must be refused), and checks them all.

- `tools/tapetest/run.sh` turns a file into cassette sound with FLOPOS's
  own code (`apps/lib/tape`), runs it through a pretend tape (wrong
  speed, wavering, quiet, noisy, dull, upside down, a dropout) and reads
  it back. The `tape` app itself is tried in a test build
  (`make EXTRA_CFLAGS=-DFLOPOS_TEST`), where the sound driver keeps what
  it plays and plays it back when asked to listen: with `tools/bench/run.py`,
  `type:tape save\n`, `key:ret=>\[tape\] saved files 11`, `key:esc`,
  `type:tape load\n=>\[tape\] same STATE.BIN`. Delete the bench's
  `sound.wav` afterwards; it is large.

`run.py` waits for FLOPOS's own messages rather than pausing, and lets the
emulated clock run ahead of the wall's. A full check takes about half a
minute. `--slow` holds it to the laptop's speed, for timing:

    tools/bench/run.py dist/flopos.img /tmp/out --site SONGS \
        'm/201549=>octet-stream' 'key:ctrl-shift-f10' 'type:whoami\n=>alright'

## Teaching the wizard

The wizard is to answer in his own words one day ([roadmap](roadmap.md)).
His model is trained on this PC, not on the laptop; only the finished
file, `boot/wizard.wiz`, goes on the floppy, as `WIZARD.WIZ`. It is kept
in the repository, so building FLOPOS needs none of what follows; this is
only for teaching him again.

    W=~/.cache/flopos-wizard
    tools/wizard/corpus.py $W/corpus          # fetch and clean the two books
    tools/wizard/voice.py  $W/corpus          # write his own lines
    $W/venv/bin/python tools/wizard/train.py $W/corpus $W/wizard.bin
    tools/wizard/check.py  $W/wizard.bin $W/corpus
    tools/wizard/speak.py  $W/wizard.bin 10   # hear him
    tools/wizard/enginetest.py $W/wizard.bin  # FLOPOS's engine agrees?
    cp $W/wizard.bin boot/wizard.wiz          # onto the next floppy

- **What he reads.** Two public-domain books from Project Gutenberg,
  Shakespeare's complete works and the Jargon File 4.2.2, cut into single
  sentences; and six sets of lines written for FLOPOS in `voice.py`: the
  tired wizard himself, a bright land of sweets and quests, a neighbour
  with a scheme, the pirates of the phone lines, a singing fellow of the
  woods, and a game master. Those six are flavours only: no line is taken
  from a show, a film or a book. The books and the model stay outside the
  repository, in `~/.cache/flopos-wizard`.
- **The mix.** His own six flavours share the training evenly. The two
  books get a twentieth of it between them (`--books 0.05`): a model this
  small can't learn them and makes up words in their manner, so those
  lines are kept as a rare oddity.
- **The model.** A transformer of about 240,000 weights: 512 word pieces,
  3 layers, 96 wide, 64 pieces of memory. Every weight is one byte with a
  scale for each row, and the last fifth of the training is done with the
  weights already rounded so, to lose nothing when the file is written.
  The file is held under 256 KB.
- **`@`** in his lines stands for the word that was typed; FLOPOS will put
  the real word there.
- **`speak.py`** does the sums exactly as the engine on FLOPOS does
  (`kernel/wizsay.c`), from the same file. `enginetest.py` builds that
  engine for this PC and compares the two, score by score.
- **`check.py`** has him say 300 lines and measures them: that they end,
  that the words are real, that neighbouring words have met before (a
  rough test of grammar), that he doesn't repeat himself or stutter, and
  that he isn't reciting the books. It prints ok or FAILED.
- **In the emulator:** `tools/bench/run.py dist/flopos.img OUT
  'type:blorp\n=>\[wizard\]'`; he writes `[wizard] N pieces` on the serial
  wire for each answer.

`train.py` needs PyTorch, which lives in its own environment:

    python3 -m venv ~/.cache/flopos-wizard/venv
    ~/.cache/flopos-wizard/venv/bin/python -m pip install torch numpy

Training takes about a quarter of an hour on a graphics card. It is not
the same twice: each run gives a slightly different wizard.

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
