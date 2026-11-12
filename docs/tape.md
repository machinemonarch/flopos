# tape: your files on an audio cassette

`tape` keeps a copy of your files on an ordinary audio cassette, as sound,
the way the home computers of the 1980s did. It is for a cassette recorder
with EAR and MIC sockets; it was built with a PYE Audio Automatic Data
Recorder D6600/35P in mind.

**Not yet tried with a real recorder.** Saving and reading back work in
the emulator, against a pretend tape. Listening through the laptop's sound
card has never run on the laptop itself.

## What it keeps

Everything in your [inventory](inventory.md), folders and all, and three
of the system's own records: browzer's bookmarks, the journal, and the
words you have taught (with the network settings beside them). Nothing
else can go to a tape, and what comes back can only go into the inventory
or over one of those three.

About 110 bytes go by each second, so 16 KB takes two and a half minutes.
A file over 64 KB (ten minutes) is left out, and `tape` says so. `tape` says how long the tape must be before
it starts.

## Saving

    here> tape save
    here> tape save letter.md

1. A cable from the laptop's headphone socket (on the front) to the
   recorder's MIC socket.
2. Set the recorder recording, then press <kbd>Enter</kbd>. The first
   eight seconds are a plain tone, to get past the clear leader at the
   start of a cassette.
3. When it says all of it is on the tape, stop the recorder.

<kbd>Esc</kbd> stops early. To be sure of a save, rewind and load it: every
file should come back "the same as on the floppy".

## Loading

    here> tape load
    here> tape load letter.md

1. A cable from the recorder's EAR socket to the laptop's line-in socket
   (left side, marked with a note and an arrow). <kbd>Tab</kbd> listens to
   the microphone socket instead.
2. Rewind the tape and set the recorder playing. The bar shows how loud
   the tape is, and beside it whether what is heard is a tape's tones.
   The tape is also heard through the laptop's loudspeaker.
3. Each file is listed as its pieces arrive. When all have come whole,
   `tape` stops listening by itself; <kbd>Esc</kbd> stops it sooner.

Then it says, for each file, whether the tape's copy is the same as the
floppy's, different, or not on the floppy at all (it is then made, with
its folders). Nothing has been written yet. <kbd>Enter</kbd> puts
the tape's copy of every different file on the floppy, over what is there.
<kbd>Esc</kbd> leaves the floppy as it is.

If pieces are missing (a dropout, a crease in the tape), rewind, press
<kbd>Space</kbd> and play it again: what was already read is kept and only
the gaps are filled.

The journal grows as FLOPOS is used, so after a while it will always be
"different" from the tape's. That is as it should be.

## On the tape

Two tones: a 0 is one cycle of 1200 Hz, a 1 is two cycles of 2400 Hz. A
byte is a 0, its eight bits and a 1. Each file is its name and size
(twice), its bytes in blocks of 256, and an end mark (twice); every block
carries its own check, and the file a check of the whole. Reading goes by
the time between the wave's crossings of its middle, so loudness, a tape
running a few percent fast or slow, and a wave turned upside down do not
matter. The details are in `apps/lib/tape/tape.h` and `apps/tape/tape.c`.
