# Roadmap

Ideas waiting their turn. None is started until it is picked, and each
gets a short plan with a rough cost first. Security work has its own list,
in [security-roadmap.md](../design/security.md).

## Waiting to be picked

### Data backup to cassette

**Done, as `maelcum`** ([maelcum](../user-guide/maelcum.md)). What was reached:

- The sound (`apps/lib/tape`): two tones, 1200 and 2400 Hz, or an octave
  up for fast; blocks of 256 bytes, each with its own check.
- 1.30.0: the sound card listens as well as plays; an app saves, reads
  back, compares and puts back.
- 1.33.0: the first real recorder read nothing, because its waves are
  lopsided. The reader now times whole waves, and **reading a tape back
  worked on the laptop**.
- 1.34.0: a full-screen backup program: a job list, ticks for files and
  folders, LZSS packing, fast speed, verify, accuracy for each file, a
  folder to restore into, a cassette whose wheels turn, and a tune.
- 1.35.0 to 1.36.1: its own look (red, gold and green, yellow keys), a
  line that says how it is going, ten summing-up sayings, and the name.

- 1.37.0: encryption of what goes to tape, with a choice of cipher.
- 1.38.0 to 1.40.0: reading became restoring (the tape's files are
  listed, to tick and put on the floppy), the options in sections, a
  reader that copes better with a dull recorder at fast speed, and a
  tape sleeve to print.

How far it has been tried on the laptop is at the top of
[maelcum](../user-guide/maelcum.md).

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

Done in 1.28.0: see [Commands](../user-guide/commands.md) and
[Internals](../design/architecture.md#the-wizards-words). Still open: he answers only
unknown words, and can't yet be talked to.
