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

**Also good for:** any full-screen app, such as `write`.

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
