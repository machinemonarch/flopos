# Roadmap

Ideas waiting their turn. None is started until it is picked, and each
gets a short plan with a rough cost first. What was fixed in the security
review is in [Security](../design/security.md).

## Waiting to be picked

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

## Security, waiting for a decision

These are the weak spots found in FLOPOS's code and what fixing each
one would take. **Nothing here is started until Max picks it.** He sets the
order. The order below is only a suggestion: most protection for the least
work first.

These come from reading the code on 2026-11-10. None of them has been
tried as a real attack.

Sizes are rough. **Small** is a change in one place. **Medium** touches
several files or needs new tools. **Large** changes how the system works.

### 9. The time from the network is not signed (medium)

**Set aside (2026-11-12), not dropped.** The laptop's clock battery
works, so the clock is right without the network, and it already refuses
to go back past the last `bye`. Signed time would cost a few seconds at
every network start and a fixed outside time server. Take it up again if
the clock battery fails.

`rtc.c`, `ip.c`. The clock sets itself from a time server, and plain NTP
has no signature. The answer is checked to come from the server asked and
to carry back a random number from the question, so nobody off the path
can fake it. Someone on your own network still could.

**Fix:** NTS, Network Time Security (RFC 8915), which is NTP with a TLS
handshake first. **Stops:** a faked time making a recently expired
certificate look valid. The clock already refuses to go back past the last
`bye`, which limits how far back that can reach.

## Done

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
- 1.41.0 to 1.41.2: the file list shows the floppy's files only after W
  and the cassette's only after R; a read is restored into a folder of
  its own named by the date; the sleeve became one A4 PDF, cut as one
  piece and folded, small enough to go on the tape; lost pieces are
  counted fairly, and an older backup further along the tape is ignored.

How far it has been tried on the laptop is at the top of
[maelcum](../user-guide/maelcum.md).

### A wizard who answers

Done in 1.28.0: see [Commands](../user-guide/commands.md) and
[Internals](../design/architecture.md#the-wizards-words). Still open: he answers only
unknown words, and can't yet be talked to.
