# The cassette format

How maelcum[^1] turns files into sound on an ordinary audio cassette, and
reads them back. The code is in `src/apps/lib/tape/` and
`src/apps/maelcum/maelcum.c`.

## In short

- **Two tones.** A 0 is one cycle of 1200 Hz, a 1 is two cycles of
  2400 Hz. Fast speed is the same an octave up.
- **A byte** is a 0, its eight bits, and a 1.
- **A file** is its name and size (twice), its bytes in blocks of 256, and
  an end mark (twice). Every block carries its own check, and the file
  carries a check of the whole.
- **Packing** is LZSS (Storer and Szymanski, 1982): a 4096-byte window and
  copies of 3 to 18 bytes.
- **Reading** times whole waves, so loudness, a tape running fast or slow,
  a lopsided wave and a wave turned upside down do not matter.
- A tape written by an earlier FLOPOS still reads.

Normal speed has been read back from a real recorder. Fast speed has
only been read from a simulated tape so far.

## How it reads

**Only the timing is trusted.** A cassette changes loudness, lets the
middle of the wave drift, may turn the wave upside down, and runs a few
percent off speed. The time between the wave's crossings of its middle
survives all of that, so that is all the reader looks at, to a sixteenth
of a sample. It keeps re-learning how long a short wave is.

**Whole waves, not halves.** A recorder plays the wave back lopsided: the
half above the middle lasts longer than the half below, or the other way
round. Timed from one upward crossing to the next, a whole wave is the
same length however lopsided it is, so whole waves are what is timed.

**Several listeners on one tape.** The tape may come back upside down, and
it may have been written at either speed, so several readers listen at
once: two speeds, and each speed timed on the upward and on the downward
crossings. Only the one in step with the tape gets blocks past their
check. There is no setting for the speed.

**Treble lifted for a dull recorder.** A dull recorder plays the high
tone much weaker than the low one, and the low one then decides where
the wave crosses. So a fast tape is also heard a second way, each sample
less three quarters of the one before, which lifts the high tone back up.

**A loss is counted once, and only if it is one.** One listener losing a
block that another has is not a loss. When a listener gives up on a
block, maelcum notes which listeners were inside that block at the
moment, and counts the loss only when all of them have given it up. One
of them finishing the block clears it.

## How it keeps a file whole

**Missing is better than wrong.** Every block has its own check and is
dropped whole if it fails. Blocks say which file and which piece they
are, so a second playing fills only the gaps.

**Packing that a spoiled piece cannot corrupt.** Files are packed with
LZSS, laid out as in Haruhiko Okumura's LZSS.C of 1989 (not his code).
The pieces are still checked one by one, and a file is unpacked only
when all of them are in, and then checked whole.

**Only what is in use.** FLOPOS's own files never change size and are
mostly filling. The run of one repeated byte at the end is sent as a
count, which works for any file without knowing what it is.

## Testing without a recorder

The emulator's sound card cannot record. A test build keeps what it plays
and hands it back when asked to listen, so the whole app can be tried,
saving and reading, without a recorder[^2].

How the reader came to be this way is in the lessons[^3].

[^1]: [maelcum](../user-guide/maelcum.md)
[^2]: [Testing](../development/testing.md)
[^3]: [Lessons on the way: what the cassette taught us](lessons.md#what-the-cassette-taught-us)
