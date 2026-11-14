# The cassette format

How maelcum ([its page](../user-guide/maelcum.md)) turns files into sound
on an ordinary audio cassette, and reads them back.

## In short

Two tones: a 0 is one cycle of 1200 Hz, a 1 is two cycles of 2400 Hz;
fast is the same an octave up. A byte is a 0, its eight bits and a 1.
Each file is its name and size (twice), its bytes in blocks of 256, and
an end mark (twice); every block carries its own check, and the file a
check of the whole. Packing is LZSS (Storer and Szymanski, 1982): a
4096-byte window and copies of 3 to 18 bytes. Reading times whole waves,
so loudness, a tape running fast or slow, a lopsided wave and a wave
turned upside down do not matter. A fast tape is also listened to with
its treble lifted, since a dull recorder gives the high tone much weaker
than the low one. Several listeners hear the one tape; a block counts as
spoiled only when none of them has it. A tape written by an earlier FLOPOS
still reads. The details are in `apps/lib/tape/` and `apps/maelcum/maelcum.c`.

## Files as sound, read by the gaps between crossings

For the cassette (`apps/lib/tape`, `apps/maelcum`). Normal speed has read
from a real recorder; fast speed has not yet. What follows holds against
a simulated tape.

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
- **Treble lifted for a dull recorder.** Fast speed read badly on the
  real recorder. A dull recorder gives the high tone much weaker than the
  low one, and the low one then decides where the wave crosses. So a
  fast tape is heard a second way too: each sample less three quarters
  of the one before, which lifts the high tone back up. In the simulated
  dull tape, fast went from unreadable to read. Not yet tried on the
  recorder itself.
- **Count a loss once, and only if it is one.** With several readers on
  one tape, one losing a block that another has is not a loss. A failure
  waits a twentieth of a second; if any reader passes a block in that
  time it is forgotten. Before this the counts ran high and the screen
  could despair over a tape that was reading.
- **Wait for the readers still inside the block.** The twentieth of a
  second was not enough. On a dull recorder at fast speed the plain
  readers lose every block half way, while the treble-lifted pair read
  it to its end, which is longer than a twentieth of a second later.
  Each loss was counted, the screen said "Sorry! Sorry!" throughout, and
  the tape then came whole. Now a failure notes which readers were inside
  a block at that moment (or came into it just after) and is counted only
  when all of them have given it up; one of them finishing the block
  clears it. The simulated tape test now fails if a tape read whole
  counted any loss: before, four of its cases did, after, none.
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
