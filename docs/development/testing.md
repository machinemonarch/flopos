# Testing

What is tested where, and the tools that do it. They are kept in
`tools/`, so they are never written again.

## What is tested where

The emulator has no PC Card slots, no 3Com card and no 65550 video chip,
so those parts can only be tested on the laptop. Everything else (boot, the
screen at both sizes, the shell, the disk, the serial line, the whole
network stack and the encryption) is tested in QEMU before a release.

The encryption code was also tested on the PC against OpenSSL and Python
across thousands of random inputs, and ML-KEM against NIST's official test
vectors, before it went into FLOPOS.

## Hardware for testing on the laptop

What the parts the emulator can't stand in for are tested with:

- **The laptop:** A Toshiba Satellite Pro 430CDT, or a machine of its age
  with a Pentium and a Sound Blaster compatible sound chip.
- **Floppies:** 1.44MB (HD) diskettes, a USB floppy drive on the PC to
  write them, and the laptop's own external floppy drive to boot them.
- **A serial cable:** A null-modem cable and a USB serial adapter on the
  PC, for `report` and for watching everything FLOPOS prints
  (`picocom -b 115200`).
- **The network:** The 3Com 3C589D PC Card with its cable adapter, and a
  network cable to a router.
- **Cassettes:** A cassette data recorder or any tape recorder with a
  volume knob, ordinary audio cassettes, and two audio cables: one from
  the laptop's headphone socket to the recorder's input, one from the
  recorder's output to the laptop's line-in (or its microphone socket).
- **Paper:** Any printer, for maelcum's tape sleeve.
- **A phone camera,** for photos of the screen when something goes wrong
  before the serial line is up.

## Running at the laptop's speed

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
how much music is in hand (see Techniques[^1]).

## The test tools


- `test/audiotest/render.c` plays a tracker song on this PC through
  FLOPOS's own tracker; `compare.py` holds the result up against
  libopenmpt. `songs.txt` lists the songs from The Mod Archive that test
  it, and `fetch.py` fetches them (they are their authors', so they are
  never kept here).
- `test/bench/site.py` is a small web site that imitates the sites
  browzer has had trouble with: The Mod Archive's downloads, DuckDuckGo's
  front page, ERICADE Radio, 72fm.com (and the Radio Browser directory
  its country pages ask) and an AzuraCast station.
- `test/bench/run.py` boots the image in the emulator, opens pages and
  presses keys, and checks each step against what FLOPOS says on its
  serial wire. It prints a line per step: ok, or what was wrong.
  browzer writes a line there for each page it gets (`[page] ...`) and the
  HUD for each time it closes (`[hud] ...`). It also records everything
  the Sound Blaster played, which `findsong.py` can find a song in.
- `test/bench/tlstest.py` makes a test authority, an image that trusts it,
  and six local OpenSSL servers (TLS 1.2 in each form FLOPOS takes, TLS
  1.3, and a TLS 1.1 one that must be refused), and checks them all.

- `test/fstest/run.py` builds FLOPOS's own file code (`src/kernel/fs.c`) for
  this PC and runs it on a copy of the image: thousands of random makes,
  changes, moves and removals in the inventory, then `fsck.fat` and
  mtools must agree with every one; then the same with the power cut
  after a random number of sectors. Run it after any change to `fs.c`.

- `test/walltest/run.py` builds an app that breaks each of the wall's
  rules on purpose (`test/walltest/rogue.c`), runs it in the emulator and
  checks that FLOPOS closes it and still answers afterwards.

- `test/fuzz/run.py` feeds FLOPOS's readers garbage. The code that
  reads what comes from outside is built for this PC with the compiler's
  memory checker, and given real inputs damaged at random: bits flipped,
  sizes changed, pieces cut out, moved and repeated. The targets are
  `audio` (mp3, Ogg Vorbis, FLAC, AAC, WAV), `tracker` (MOD, XM, S3M,
  IT), `x509` (certificate chains and signatures), `tls` (whole secure
  conversations, recorded from openssl servers on this PC and played
  back damaged, with the seals opened whatever they say so the damage
  reaches the code behind them) and `browzer` (the whole browser, on a
  pretend system whose every connection answers with the input).
  `test/fuzz/run.py --runs 60000` runs them all; an input that broke
  something is kept in `dist/fuzz/finds`. Songs come from
  `~/.cache/flopos-songs`, and recordings from `~/.cache/flopos-fuzz/audio`
  (the first 96 KB of any mp3, ogg or flac; never kept in the project).
  Run it after any change to a decoder, the browser's page reading, or
  the certificate and TLS code.

- `test/keeptest.sh` tries what `writefloppy.sh` does to carry the
  user's files to a new release (`tools/floppy/keepfiles.sh`), on images: an old
  floppy with `WRITE2.MD`, an inventory with folders, a blank disk, and
  one too full to carry.

- `test/doccheck.py` checks that every link in the project's Markdown
  leads to a page, and every `#section` to a heading in it. Run it after
  moving or renaming a page or a heading.

- `test/keytest/run.sh` builds the keyboard code (`src/kernel/keymap.c` and
  the layouts) for this PC and presses keys on every layout: the signs,
  the accent keys, Caps Lock, Alt+Shift between alphabets, Ctrl shortcuts
  by letter, and that the font can draw everything any key types. It also
  checks the order Hebrew is drawn in (right to left, numbers and
  brackets).

- `test/tapetest/run.sh` turns a file into cassette sound with FLOPOS's
  own code (`src/apps/lib/tape`), runs it through a pretend tape (wrong
  speed, wavering, quiet, noisy, dull, upside down, a dropout) and reads
  it back. The `maelcum` app itself is tried in a test build
  (`make EXTRA_CFLAGS=-DFLOPOS_TEST`), where the sound driver keeps what
  it plays and plays it back when asked to listen: with `test/bench/run.py`,
  `type:maelcum save\n`, `key:ret=>\[tape\] saved files 3`, `key:esc`,
  `type:maelcum load\n=>\[tape\] same STATE.BIN`. Delete the bench's
  `sound.wav` afterwards; it is large.

`run.py` waits for FLOPOS's own messages rather than pausing, and lets the
emulated clock run ahead of the wall's. A full check takes about half a
minute. `--slow` holds it to the laptop's speed, for timing:

    test/bench/run.py dist/flopos.img /tmp/out --site SONGS \
        'm/201549=>octet-stream' 'key:ctrl-shift-f10' 'type:whoami\n=>alright'

### The whole browser on a pretend system, fed damaged pages

Apps reach the system only through one table of functions (`src/apps/sdk/app.h`).
So an app can be built for an ordinary PC and handed a different table:
one whose network answers with whatever a test gives it, and whose
keyboard is a script. `test/fuzz/browzer.c` does that with browzer, and
a memory checker watches every byte it touches while it reads damaged
web answers, lays them out, follows links and plays what it is given.
Nothing in the app is changed for the test.

The secure connection is tested by playing back real conversations.
FLOPOS's random numbers are replaced by ones that are the same every
time, so it makes the same keys as when the conversation was recorded
and the playback fits. Damage inside a sealed record would only fail the
seal and be thrown away unread; so in the test the seal is opened
whatever it says, and since these ciphers work by flipping bits, a
flipped bit in the sealed text comes out as the same flipped bit in the
message behind it.

[^1]: [Techniques](../design/techniques.md#measuring-before-anything-else)
