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

The top line names it, "Magnetic Rust: Riddim 'n' Vibrationz Engine",
in colours that run through the letters.

At the top are the settings, each with its key, in two groups, and under
them a third, **Run 'Tingz**, with the cassette and the two things to do:
<kbd>W</kbd> write and <kbd>R</kbd> read. The value in force is green;
the one that is not is grey.

**Write Options**

| Key | Setting |
|---|---|
| <kbd>J</kbd> | The job, from a list: **Standard Backup** (everything of yours on this floppy) or **Pick & Mix** (only what you tick below) |
| <kbd>C</kbd> | Compress (LZSS): on or off. A file is sent packed when that is shorter. |
| <kbd>F</kbd> | Speed: normal or fast. Fast takes half the tape and is easier to spoil: it wants a good recorder and tape. Reading needs no telling; it hears which. |
| <kbd>V</kbd> | Verify after writing: on or off. On, the tape is read back straight after it is written. |
| <kbd>E</kbd> | Encrypt: on or off (off to begin with). See below. |
| <kbd>K</kbd> | The cipher, from a list of three |

**Read Options**

| Key | Setting |
|---|---|
| <kbd>Tab</kbd> | Which socket listens: line-in or microphone |
| <kbd>D</kbd> | Restore into, from a list: where each file came from, the top of the inventory, any folder, or **a new folder**. A new folder is asked a name and made at once, inside the folder the list below is showing. |

The cassette's wheels turn while a tape is written or read, and beside it
is the progress. Below are your files, one folder at a time, as
`inventory` shows them: <kbd>Up</kbd> and <kbd>Down</kbd> move,
<kbd>Enter</kbd> goes into a folder, <kbd>Backspace</kbd> comes back out,
and <kbd>Space</kbd> ticks a file, or a folder and everything in it.
Ticking by hand makes the job Pick & Mix.

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
whole, the tune plays; if it did not, a shorter, sadder one does.
Stopping by hand is not held against the tape: it gets a poor verdict
only if pieces were actually spoiled.

The whole save is kept in memory; nothing has touched the floppy. The
list below then shows **what the tape holds**, each file marked new (not
on this floppy), different (from the floppy's copy) or same. The new
ones start ticked. A different one is left unticked, since restoring it
replaces the floppy's copy, which may be the newer; tick it if the
tape's is the one you want. The same ones are grey.

<kbd>Space</kbd> ticks, <kbd>Enter</kbd> puts the ticked files on the
floppy, with their folders, or into the folder chosen with <kbd>D</kbd>.
<kbd>Esc</kbd> goes back to the floppy's files without restoring. A tape
from another floppy restores just as well: its files are all new.

If pieces are missing (a dropout, a crease in the tape), rewind, press
<kbd>R</kbd> and play it again.

The journal grows as FLOPOS is used, so after a while it will always be
"different" from the tape's. That is as it should be.

## Encryption

With Encrypt on, writing asks for a passphrase, twice. Each file's name
and contents are then sealed before they become sound; only how many
files there are, and roughly how long each is, can be told from the tape.

Reading a sealed tape needs Encrypt on and the same passphrase. A wrong
passphrase, or a tape whose data was changed, is refused outright:
nothing is restored. With Encrypt off, a sealed tape is recognised and
`maelcum` says so.

| Cipher | Key | |
|---|---|---|
| ChaCha20-Poly1305 | 256-bit | Quick on this laptop. The one to use. |
| AES-256-GCM | 256-bit | As strong; several times slower here, since the Pentium has no AES instructions. |
| AES-128-GCM | 128-bit | Sound today, but not against a future quantum computer. |

A 256-bit cipher is what "quantum-safe" means for a backup locked with a
passphrase. The post-quantum key exchange FLOPOS uses online (ML-KEM) is
for two parties agreeing on a key, which a tape does not need.

**The passphrase is the weak point, not the cipher.** It is turned into
the key by PBKDF2-HMAC-SHA256 with a fresh random salt and 10,000 rounds,
about two seconds on this laptop. A fast modern computer can try many
thousands of guesses a second against that, so a short or common
passphrase can be found. Four or more random words cannot, in practice.

**The passphrase is kept nowhere.** It is wiped from memory when the
write or read ends. Forget it and the tape cannot be opened by anyone.

How it is put together: a save has one key. Each header is sealed with
its open part (cipher, place in the save, save number, rounds, salt) as
associated data; each file's bytes are sealed whole after packing, and
the 16-byte tag travels with them. Nonces name what is sealed (header or
file) and which file; they never repeat within a save, and every save
has a new salt and so a new key.

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
turned upside down do not matter. A fast tape is also listened to with
its treble lifted, since a dull recorder gives the high tone much weaker
than the low one. Several listeners hear the one tape; a block counts as
spoiled only when none of them has it. A tape written by an earlier FLOPOS
still reads. The details are in `apps/lib/tape/` and `apps/maelcum/maelcum.c`.
