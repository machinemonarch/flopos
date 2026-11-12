# Roadmap

Ideas waiting their turn. None is started until it is picked, and each
gets a short plan with a rough cost first. Security work has its own list,
in [security-roadmap.md](security-roadmap.md).

## Waiting to be picked

### Data backup to cassette

**Begun.** Saving files to an ordinary audio cassette and reading them
back, on a PYE Audio Automatic Data Recorder D6600/35P: the documents from
`write`, the bookmarks, the journal and the settings.

Done so far: the sound itself (`apps/lib/tape`). Two tones, 1200 and
2400 Hz, as the home computers of the 1980s used; 120 bytes a second;
the file in blocks of 256 bytes, each with its own check, so a damaged
block is dropped and the tape can be played again to fill it in. It is
tested against a pretend tape on this PC (`tools/tapetest/run.sh`): 6%
fast or slow, wavering, quiet, clipped, noisy, dull, upside down, and a
dropout. A full 16 KB document takes about two and a half minutes.

Also done, in 1.30.0: the sound card listens as well as plays
(`audio_listen`), and the `tape` app saves, reads back, compares with the
floppy and puts back ([tape](tape.md)). Tried only in the emulator, where
a test build plays back what it played, as a pretend tape.

Still to do: trying it on the laptop with the recorder. The emulator's
sound card cannot record, so listening has never run on real hardware,
and this is where the surprises will be: whether the laptop's sound chip
records through its Sound Blaster side at all, which socket it listens
to, and how loud the recorder must be.

### A chat app

Something like Discord, IRC, WhatsApp or Signal. Which one, and whether
there is something better to talk on, is still open: IRC is the easiest for
this machine, if there were a channel worth being in.

### A mystical protocol for sharing data

A client and a server, one on the Toshiba and one on another machine, to do
what SFTP, FTP, stunnel, NFS and SMB do, without simply reinventing netcat.
Plain text at first, standard ciphers later. It should feel mysterious,
obscure and foreign, dark and enticing, like a forgotten metro-2 tunnel, a
liminal space, another dimension. That goes all the way down: how it checks
its data, how a session begins, every layer it touches. What it is for
(talking, sharing files, looking after a server) is not decided yet, so it
starts with an interview of its product owner.

### A wizard who answers

Done in 1.28.0: see [Commands](commands.md) and
[Internals](internals.md#the-wizards-words). Still open: he answers only
unknown words, and can't yet be talked to.
