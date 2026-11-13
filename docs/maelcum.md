# maelcum: backup to an audio cassette

`maelcum` keeps a copy of your files on an ordinary audio cassette, as sound,
the way the home computers of the 1980s did. It is for a cassette recorder
with EAR and MIC sockets. It is named for the pilot in William Gibson's
*Neuromancer* (1984), who got people home.

Reading a tape from a real recorder worked on the laptop with 1.33.0. The
backup screen described here (1.34.0), fast speed and packing have run
only in the emulator, against a pretend tape.

## What it keeps

Everything in your [inventory](inventory.md), folders and all, and three
of the system's own records: browzer's bookmarks, the journal, and the
settings (the network's among them). Nothing else can go to a tape, and
what comes back can only go into the inventory or over one of those three.

At normal speed about 110 bytes go by each second, so 16 KB takes two and
a half minutes; packing usually halves that for text, and fast speed
halves it again. A file over 64 KB is left out, and `maelcum` says so.

## The screen

    here> maelcum

At the top are the settings, each with its key. The value in force is
green.

| Key | Setting |
|---|---|
| <kbd>J</kbd> | The job, from a list: **Standard Backup** (everything of yours on this floppy) or **Pick & Mix** (only what you tick below) |
| <kbd>C</kbd> | Compress (LZSS): on or off. A file is sent packed when that is shorter. |
| <kbd>F</kbd> | Speed: normal or fast. Fast takes half the tape and is easier to spoil: it wants a good recorder and tape. Reading needs no telling; it hears which. |
| <kbd>V</kbd> | Verify after writing: on or off. On, the tape is read back straight after it is written. |
| <kbd>Tab</kbd> | Which socket listens: line-in or microphone |
| <kbd>D</kbd> | Restore into: where each file came from, or one folder for everything |

Under them is a cassette, whose wheels turn while a tape is written or
read, and beside it the progress. Below is the list of your files and
folders. <kbd>Up</kbd> and <kbd>Down</kbd> move, <kbd>Space</kbd> ticks a
file, or a folder and everything in it. Ticking by hand makes the job
Pick & Mix.

Above the keys, one line says how it is going, in plain words: "Buckle
up!" when recording starts, "Now hear 'dis!" when playback starts,
"Smooth sailing." when all is well, and less calm things when pieces are
being spoiled or the tape runs off its speed. While recording it cannot
hear the tape, so there the line goes only by how far along it is; the
real judging happens when the tape is played back.
When a tape has been read it sums up with one of ten sayings, five for a
tape that came whole and five for one that did not. The plain facts stay
on the screen beside the cassette and in the list.

The screen keeps to red, gold and green on black, frame included, with
the keys in yellow (everywhere else in FLOPOS they are pink) and what is
switched on in a dark neon green.

## Writing

1. Press <kbd>W</kbd>. It says how much tape the ticked files need, with
   a legend of what one side of a C30, C60 and C90 holds.
2. Plug a cable from the laptop's headphone socket (on the front) into
   the recorder's MIC socket.
3. Set the recorder recording, then press <kbd>Enter</kbd>. The first
   eight seconds are a plain tone, to get past the clear leader at the
   start of a cassette.
4. The red bar and the percentage show how far it has come. When all of
   it is on the tape there is a little tune. Stop the recorder. With
   Verify on there is no tune yet: stop the recorder, rewind and press
   PLAY on it, and the tune comes when the check has passed.

<kbd>S</kbd> stops early. The laptop cannot hear the tape while it is being
recorded, so the only way to be sure of a save is to read it back: that is
what Verify does.

## Reading

1. Plug a cable from the recorder's EAR socket into the laptop's line-in
   socket (left side, marked with a note and an arrow).
2. Press <kbd>R</kbd>, rewind the tape and set the recorder playing. The
   green bar shows how loud the tape is, and under it whether what is
   heard is a tape's tones.
3. Each file shows how much of it has been read and its accuracy: the
   share of its pieces that passed their check.

Reading ends by itself when every file has come whole, or after ten
seconds of silence; <kbd>S</kbd> ends it sooner. If everything came
whole, the tune plays. Stopping by hand is not held against the tape:
it gets a poor verdict only if pieces were actually spoiled.

Then it says, for each file, whether the tape's copy is the same as the
floppy's, different, or not on the floppy at all. Nothing has been
written yet. <kbd>Enter</kbd> puts the tape's copy of every different
file on the floppy, with its folders. Only ticked files are taken from a
tape.

If pieces are missing (a dropout, a crease in the tape), rewind, press
<kbd>R</kbd> and play it again.

The journal grows as FLOPOS is used, so after a while it will always be
"different" from the tape's. That is as it should be.

## From the prompt

    here> maelcum save
    here> maelcum load
    here> maelcum save letter.md

`maelcum save` opens the screen ready to write, `maelcum load` opens it
listening. A name after either keeps to that file.

## Tape types

Type I, II and IV cassettes all work and need no setting: the tones are
far below the treble where the types differ. A simple recorder may not
fully erase a Type IV (metal) tape.

## On the tape

Two tones: a 0 is one cycle of 1200 Hz, a 1 is two cycles of 2400 Hz;
fast is the same an octave up. A byte is a 0, its eight bits and a 1.
Each file is its name and size (twice), its bytes in blocks of 256, and
an end mark (twice); every block carries its own check, and the file a
check of the whole. Packing is LZSS (Storer and Szymanski, 1982): a
4096-byte window and copies of 3 to 18 bytes. Reading times whole waves,
so loudness, a tape running fast or slow, a lopsided wave and a wave
turned upside down do not matter. A tape written by an earlier FLOPOS
still reads. The details are in `apps/lib/tape/` and `apps/maelcum/maelcum.c`.
