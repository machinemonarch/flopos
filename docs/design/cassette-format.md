# The cassette format

How maelcum[^1] turns files into sound on an audio cassette and reads them
back. The code is in `src/apps/lib/tape/` and `src/apps/maelcum/`.

## The format

- **Two tones:** a 0 is one cycle of 1200 Hz, a 1 two cycles of 2400 Hz. Fast speed is an octave up.
- **A byte** is a 0, its eight bits, and a 1.
- **A file** is its name and size (twice), its bytes in blocks of 256, and an end mark (twice). Every block has its own check; the file has a check of the whole.
- **Packing** is LZSS (Storer and Szymanski, 1982; laid out as in Okumura's LZSS.C, not his code): a 4096-byte window, copies of 3 to 18 bytes. A file is unpacked only when every block is in.
- **Filling** at the end of a file (one repeated byte) is sent as a count.
- Tapes written by earlier FLOPOS versions still read.

## Reading

- **Only timing is trusted.** Loudness, drift, an upside-down wave and a few percent off speed don't matter: only the time between crossings is read, to a sixteenth of a sample.
- **Whole waves,** upward crossing to upward crossing, because a recorder plays the wave back lopsided.
- **Several listeners at once:** both speeds, each on upward and downward crossings, and for fast speed a treble-lifted copy for dull recorders. No speed setting is needed.
- **A block lost by one listener** counts as lost only when every listener inside it has given up.
- **Missing beats wrong:** a block that fails its check is dropped; a second playing fills the gaps.

Normal speed has been read from a real recorder; fast speed only from a
simulated tape. A test build of FLOPOS keeps what it plays and plays it
back when asked to listen, so maelcum can be tried without a recorder
(see Testing[^2]).

[^1]: [maelcum](../user-guide/maelcum.md)
[^2]: [Testing](../development/testing.md)
