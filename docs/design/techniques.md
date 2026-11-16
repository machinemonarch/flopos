# Techniques

How FLOPOS keeps radio playing on a 120 MHz Pentium while the screen runs
at 30 frames a second, and the other tricks worth reusing. None is new;
each is a known technique, credited in the references. They are written
as shared pieces so later work can use them.

## Measuring

Changes are judged in an emulator held to about the laptop's speed (see
Testing[^1]). While sound plays, FLOPOS reports to the serial line every
ten seconds: the share of the machine decoding takes, frames drawn, and
seconds of music in hand.

## Sound

- **Refilled at interrupt time** (`audio_feeder` in `src/kernel/audio.c`). An app hands the system a refill routine, as the Mac's Sound Manager did. The timer calls it whenever the queue has room, with interrupts on and the FPU saved, in slices of at most 4 ms. The app can be busy drawing or waiting on the network and the music never stops.
- **Songs in memory.** A file is fetched whole into browzer's memory (2 MB) and played from there; a stream passes through a piece at a time. Nothing goes to the floppy.
- **Noise shaping to 8 bits** (`decode.c`). Each sample is rounded with the last one's error added in, which turns the 8-bit card's grit into a faint hiss in the treble.

## Decoding

In `src/apps/lib/bits`, `dsp` and `audio`:

- **Bits read 25 at a time** with one unaligned load, in either bit order.
- **Huffman by table:** a codebook becomes a table on the next nine bits, one or two lookups a codeword.
- **mp3's synthesis as a fast DCT** (B. G. Lee, 1984): 80 multiplications instead of 2048, and no 8 KB table to thrash the cache. Its inverse MDCT is an 18-point DCT-IV.
- **Silence skipped:** zero subbands and coefficients are never transformed.
- **Tables instead of division and sines** (AAC's codewords, Vorbis's window).
- **Quick rounding** (`dsp_round`): adding 1.5 × 2²³ instead of the x87's slow cast.

Every decoder is checked against its reference (mpg123, FAAD2, libvorbis,
libFLAC). On the slowed emulator: AAC 128 kbps takes 16% of the machine,
mp3 64 kbps 19%, Vorbis 136 kbps 26%, a MOD song under 1%.

## Tracker songs

In `src/apps/lib/audio/tracker.c`:

- **The score is read as it plays,** a row at a time from the file, so a long song needs no unpacked copy.
- **One player for four formats:** S3M and IT are turned into XM's terms as they are read.
- **IT's packed samples** are unpacked into the spare room behind the file.
- **Long strides:** between loop points a voice is mixed in a tight loop.
- **Twenty voices at most;** the quietest wait silently past that.

Checked against libopenmpt with `test/audiotest`.

## The screen

- **Only what can be seen:** under a full-screen window nothing is drawn (`term_win_covers`).
- **Only what changed:** a window is redrawn where a cell differs, and only changed rows go to the card (`vga_present_rows`).
- **Colour without redrawing:** frames and some text use sixteen palette entries placed along the screen's diagonal (`PAL_NEON`, `A_NEON` for apps). Turning those entries in the palette makes them shine for 48 bytes a step and no pixels drawn.

## The network

- **The card emptied 1000 times a second,** into a 48-frame queue, so nothing is lost while FLOPOS is busy.
- **A 16-segment TCP window,** and **SACK** (RFC 2018) with out-of-order pieces kept, so a lost segment costs one resend.

## Files and paper

- **A power-cut-safe FAT12** (`src/kernel/fs.c`): new data goes into unused clusters, then the spare allocation table, then the main one, then the folder entry; only then are the old clusters freed. The user's folder is a fence no path can climb out of. Tested with `fsck.fat` and mtools as judges, with random power cuts (`test/fstest`).
- **A page that knows its size** (`src/apps/maelcum/sleeve.c`): maelcum's sleeve is a hand-written PDF 1.3 (a picture has no paper size, so printers stretch it). One-bit pictures packed by run length, about 22 KB.

## The system

- **A wall apps never notice:** the call table an app gets points at small stubs in its own memory that do `INT 0x80`. The kernel reads the arguments off the app's stack after checking it. Calls back into the app (the sound feeder) go through `INT 0x81`.
- **A language model beside a 640K kernel:** byte weights trained already rounded, 512 word pieces, cached past pieces so each new one costs one pass, and a frame drawn between layers. The same C file is checked against a Python version (`tools/wizard/enginetest.py`).

[^1]: [Testing](../development/testing.md#running-at-the-laptops-speed)
