# Techniques

How FLOPOS keeps radio playing on a 120 MHz Pentium while the screen
runs at its full 30 frames a second.

To be honest about it first: none of these is new to the world. Each is a
known technique from the 1980s and 1990s, or from later decoders, and the
references say where it comes from. What is particular to FLOPOS is
putting them all together on one floppy for one machine, and measuring
each against that machine. They are written as shared pieces, so later
work (pictures, compression, a DAW) can use them too.

## Measuring, before anything else

FLOPOS is tested in an emulator held to about 62 million instructions a
second, roughly this laptop's speed on this kind of code:
`qemu -icount shift=4,sleep=on,align=on`. `align=on` matters. Without it
the emulated clock runs as fast as the host can execute, and a stream
looks ten times slower than it is.

While sound plays, FLOPOS reports to the serial cable every ten seconds:
- what share of the machine decoding took
- how many frames the screen drew
- how many seconds of music are in hand

Every change below was judged by those three numbers.

## Sound: refilled at interrupt time

*kernel/audio.c `audio_feeder`, app interface version 4.*

A player can't count on being asked for more when it suits it. The app
may be drawing, or waiting on the network, for longer than the sound card's
queue lasts. So, as the Mac's Sound Manager did with its double-buffer
callbacks, the app hands the system a refill routine. The timer calls it
whenever the queue has room:
- after acknowledging the interrupt
- with interrupts back on, so the clock, keys and network carry on
- never inside itself
- with the floating-point unit's state saved around it, since decoders
  use it and so might whatever was interrupted

It works in slices of at most 4 ms, so no frame of the screen waits on it
for long. browzer's routine decodes every format: mp3, AAC, Vorbis, FLAC,
WAV and tracker songs.

**Also good for:** anything that must keep time while the foreground is
busy, such as a DAW's playback, or a synthesiser.

## Network: nothing lost while busy

*kernel/net.c, kernel/time.c, kernel/tcp.c.*

- **The card emptied a thousand times a second.** The 3Com PC Card holds
  only a few kilobytes, and a full frame arrives every 1.2 ms at 10 Mbit,
  so anything not taken in time is lost. The timer now interrupts 1000
  times a second instead of 250. Every interrupt moves whatever the card
  holds into a 48-frame queue; every fourth is still the system's tick.
- **A wider window.** TCP now lets a server have 16 segments in flight,
  not 3. That used to hold a station 200 ms away to barely what it needs
  to play.
- **Out-of-order pieces kept, and SACK** (RFC 2018). A lost segment used
  to cost every segment after it, resent one round trip at a time: two or
  three seconds of nothing. Now pieces that arrive early are kept where
  they belong. Every answer tells the server exactly which pieces have
  come, so it resends only the missing one.
- **Answers sent at every read**, not only when the app runs out of data.

**Also good for:** every download, not only radio.

## The screen: only what can be seen, only what changed

*kernel/term.c `term_win_covers`, kernel/main.c, kernel/vga.c `vga_present_rows`.*

The moving background took 21.5 ms of every frame on the slowed emulator.
Inside browzer, it was drawn and enlarged, then completely covered by
browzer's window. Now:
- when a window with paper covers the whole screen, nothing under it is
  drawn
- the window is redrawn only where a cell differs from how it was drawn
  last time
- only the rows that changed are sent to the card, and nothing at all if
  nothing changed

The picture is the same to the pixel.

**Also good for:** any full-screen app, such as `scribe`.

## Decoding: the same arithmetic, less of it

*apps/lib/bits, apps/lib/dsp, and the decoders in apps/lib/audio.*

- **Bits read 25 at a time** (`bits.h`), with one unaligned load, in
  either bit order. They used to be read one at a time, at tens of
  instructions a bit.
- **Huffman codes by table** (`prefix.h`). A codebook becomes a table on
  the next nine bits, so a codeword costs one lookup, or two, instead of a
  branch per bit. The same tables serve high-first codes (mp3, AAC, one
  day JPEG) and low-first ones (Vorbis, one day gzip and PNG).
- **mp3's synthesis as a fast cosine transform** (`dsp_dct2`, B. G. Lee,
  1984). The 64-by-32 multiplication it did 689 times a second is one
  32-point DCT laid out by its symmetries: 80 multiplications instead of
  2048, and no 8 KB table to thrash the Pentium's 8 KB data cache.
- **mp3's 36-point inverse MDCT as an 18-point DCT-IV**, read out by its
  symmetries: half the multiplications.
- **Silence skipped.** At radio bitrates mp3's upper subbands hold
  nothing, and transforming zeros gives zeros. AAC skips zero coefficients
  in the same way.
- **No division where a table will do.** AAC's codewords were split into
  their numbers by dividing by 27, 9 and 3, at 46 cycles a division on a
  Pentium; now a table made once does it.
- **Vorbis's window from a table.** It computed two sines for every
  sample of every block, two thirds of all its time. Now each slope is
  worked out once per stream.
- **Quick rounding** (`dsp_round`). A cast from float to int makes the
  x87 change its rounding mode and back at every sample. Adding
  1.5 × 2²³ puts the integer where it can simply be read.

Every decoder was checked against its previous output, sample for sample
(within one step of rounding), and the formats against their reference
decoders: mpg123, FAAD2, libvorbis, libFLAC.

| On the slowed emulator | Decoding takes |
|---|---|
| AAC, 128 kbps (SomaFM) | 15.7% of the machine |
| mp3, 64 kbps mono | 18.7% |
| Ogg Vorbis, 136 kbps, 44.1 kHz | 26% |
| a MOD song | under 1% |

## Tracker songs: the score read as it plays

*apps/lib/audio/tracker.c.*

- **No score in memory.** A song's patterns used to be unpacked into a
  table of notes before it played, and a long one didn't fit. Now each row
  is read from the file when it is reached, carrying on from where the last
  one ended; only a jump makes it read its pattern again from the top.
- **Four formats, one player.** S3M and IT are put into XM's terms as
  they are read: their effects become XM's, and each sample's own rate
  becomes XM's relative note and finetune. Where XM has nothing alike (IT's
  shared slide memories, its channel volume, its notes that ring on under
  new ones), the player learns the one thing more.
- **IT's packed samples** (Impulse Tracker 2.14's own compression) are
  unpacked into the download buffer's spare room behind the file, so they
  cost no memory of their own.
- **Mixing in long strides.** Between a voice's loop points, nothing needs
  checking: those stretches are mixed in a tight loop with the position in
  registers. Only the samples at the ends are done one at a time.
- **Twenty voices at most.** A voice too quiet to move an 8-bit sample is
  skipped. Past twenty at once, the quietest wait silently, still keeping
  their place. A 64-channel song went from a third of the machine, and a
  stuttering screen, to under a fifth.

Every format was checked against libopenmpt on real songs from The Mod
Archive: most match at 0.8 to 0.999 (`tools/audiotest`).

**Also good for:** any app that plays a song: a game's music, a DAW's
sample player.

## Out of 8 bits: noise shaping

*apps/lib/audio/decode.c `lines`.*

The Sound Blaster Pro takes 8-bit samples. Dropping each sample's low
byte leaves distortion nearly as loud as a quiet passage itself: the grit
that makes an 8-bit card sound crackly. Instead, each sample is rounded
with the error the last one left added in. The rounding error then comes
out as a faint, even hiss in the treble. For a quiet tone, that is 14 dB
less error where the ear listens hardest.

## Lessons on the way

The refill routine calls the clock. The clock's watchdog reads the cycle
counter, then compares it with when the tick was last seen to move. If
the refill came between those two steps, "last seen" was newer than "now",
and the difference wrapped round to hours. A healthy timer was then
restarted for nothing, two or three times a second, with the keyboard's
buffer emptied each time. It showed up first on the laptop's serial cable.
The watchdog now looks with interrupts held off.



Drawing only what changed (1.22) never worked inside browzer. browzer
opens its window again each time it draws, and opening a window marked it
to be drawn whole. So every cell was drawn every time: about 8 ms of each
frame. A window is now drawn whole only when it is new or has changed size.

How busy the processor is, for the HUD's 'Thunking, is the time it
spends halted. FLOPOS halts it whenever it waits for the next interrupt
with nothing to do (`idle_wait`), and counts those cycles. A loop that only
waits is now seen to: called again at once with nothing done in between,
`main_pump` halts. That also kept browzer's window from being drawn on
every pass of its loop, which a loop with nothing to do shouldn't.

The kernel's own memory once grew past 0x70000, where the floppy
and the video BIOS keep their buffers. Every disk read then wrote over the
kernel's variables, and the first one to go was the keyboard's record of
which keys are held. The machine believed Shift was down. `linker.ld` now
refuses to build a kernel that reaches that line.

## Frames that shine without being redrawn

Every frame piece is drawn as geometry in one place (`blit_cell` in
`kernel/term.c`), so colouring it there colours every program's frames.
Each pixel takes one of sixteen palette entries (`PAL_NEON`) by its place
on the screen's diagonal, so neighbouring cells join without a seam, and
every other pixel is taken half a band on, which blends one band into the
next. The sixteen entries hold a full turn of the colour wheel at full
strength. `mood_neon` turns that wheel in the palette fifteen times a
second: 48 bytes to the card, and not one pixel drawn again. The entries
came out of the prompt's ramp, which had 32 and needed 16.


## A language model beside a kernel that fits in 640K

The wizard's model had to cost the floppy little, the kernel almost
nothing, and the screen no frames.

- **A byte a weight, trained that way.** Weights are stored as bytes with
  one scale for each row. The last fifth of the training runs with the
  weights already rounded to bytes, so what is written to the floppy is
  what was trained, not an approximation of it.
- **Word pieces, not letters.** He speaks in 512 pieces of words. A line
  is about twenty pieces instead of seventy letters, so it takes a third
  of the sums, and the model spends itself on phrasing, not spelling.
- **Nothing to read, so nothing to parse.** He never reads what was typed.
  He writes `@` where the word belongs and the shell puts it there. The
  engine needs no way of cutting text into pieces, only the list of them.
- **Remembering instead of redoing.** What each past piece offered to the
  ones after it is kept, so each new piece costs one pass, however long
  the line is.
- **A layer, then a frame.** The engine hands the machine back between
  layers, a few milliseconds apart. Measured at the laptop's speed with a
  song playing: 30 frames a second before, during and after.
- **Most of the voice is a grammar.** A model this small can't learn
  Shakespeare, but it learns a few hundred sentence shapes with their
  slots very well, and mixes them. The books are a twentieth of his
  training, for the oddity.
- **One engine, two builds.** The same C file is built for this PC and
  compared, score by score, with a plain Python version of the sums
  (`tools/wizard/enginetest.py`).

## Files as sound, read by the gaps between crossings

For the cassette (`apps/lib/tape`, `apps/tape`). Not yet proven on a real
recorder; what follows holds against a simulated tape.

- **Only the timing is trusted.** A cassette changes loudness, drifts the
  wave's middle, may turn it upside down and runs a few percent off speed.
  The time between crossings of the middle survives all of that, so that
  is all the reader looks at, to a sixteenth of a sample, and it keeps
  re-learning how long a short wave is.
- **Whole waves, not halves.** The first reader timed each half of a
  wave, and the first real recorder defeated it: nothing at all was read.
  A recorder's output is lopsided, one half longer than the other, which
  a simulated tape had not been. From one upward crossing to the next is
  the same length however lopsided the wave. Since a tape may come back
  upside down, a second reader times the downward crossings, and only
  the one in step with the bits gets blocks past their check.
- **Two speeds, no setting to read them.** Fast is the same signal an
  octave up. Four readers run at once (two speeds, two ways up) and
  only the right one gets a block past its check.
- **Packing that a spoiled piece cannot corrupt.** Files are packed with
  LZSS (Storer and Szymanski, 1982; laid out as in Haruhiko Okumura's
  LZSS.C of 1989, not his code). The pieces are still checked one by one
  and the file is unpacked only when all are in, then checked whole.
- **Missing is better than wrong.** Every block has its own check and is
  dropped whole if it fails. The blocks say which file and which piece
  they are, so a second playing fills only the gaps.
- **Only what is in use.** FLOPOS's files never change size and are
  mostly filling. The run of one repeated byte at the end is sent as a
  count, which works for any file without knowing what it is.
- **A pretend tape in the driver.** The emulator's sound card cannot
  record. A test build keeps what it plays and hands it back when asked
  to listen, so the whole app can be tried, saving and reading, without
  a recorder.

## Writing a floppy's filesystem so a power cut cannot break it

For the inventory (`kernel/fs.c`). FAT12 has no journal, so the order of
the writes is the whole of the safety.

- **New data never lands on old data.** A changed file goes into
  clusters nothing uses. The folder entry is one sector, written last but
  one; until it lands the old file is untouched, and after it the new one
  is complete. Only then are the old clusters let go.
- **The spare table first.** FAT12 keeps two copies of its allocation
  table. The spare is written before the one in use, so if a sector of
  the main one is cut mid-write and will not read, the spare has it
  whole. At the first change of a session the two are compared and made
  to agree.
- **The user's folder is a fence.** The system's files are never moved
  or resized and the top folder is never written. Every inventory path
  is followed down from one folder, and a name beginning with a dot is
  not a name, so nothing can climb out.
- **Test it with someone else's reader.** The file code is built for the
  PC and run through thousands of random changes; the judges are
  `fsck.fat` and mtools, not FLOPOS. That found one thing no reading of
  the format would have: mtools rejects a disk whose very last cluster is
  in use, though the format allows it. FLOPOS never gives that cluster
  out.
- **Cut the power on purpose.** The test stops the write after a random
  number of sectors, hundreds of times, and checks that every file is
  whole, old or new.

## The whole browser on a pretend system, fed damaged pages

Apps reach the system only through one table of functions (`apps/app.h`).
So an app can be built for an ordinary PC and handed a different table:
one whose network answers with whatever a test gives it, and whose
keyboard is a script. `tools/fuzz/browzer.c` does that with browzer, and
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


## A wall the apps never notice

Apps were built to call the system through a table of function addresses.
To put them behind a wall (ring 3 and paging) without rebuilding how they
are written, the table they are handed now points at eight-byte stubs
that live in the app's own memory: load a number, `INT 0x80`, return. The
arguments stay where C left them, on the app's stack, and the kernel
reads them from there after checking the stack is the app's. Going the
other way, when the kernel must call a function the app gave it (the
sound feeder, from inside the timer interrupt), it builds a small frame
on the app's stack whose return address is another stub, `INT 0x81`, and
drops to ring 3; the stub brings it back to exactly where it left off.
These calls nest. The paging is the plainest possible: 4MB pages, every
address mapped to itself, two of them open to apps. It goes off for each
BIOS call, since the BIOS runs in 16-bit mode. Sources: Intel's
architecture manuals (privilege levels, the task segment, 4MB pages).
