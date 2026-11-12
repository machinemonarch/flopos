# Roadmap

Ideas waiting their turn. None is started until it is picked, and each
gets a short plan with a rough cost first. Security work has its own list,
in [security-roadmap.md](security-roadmap.md).

## Waiting to be picked

### Data backup to cassette

Saving files to an ordinary audio cassette (types I, II and IV) and reading
them back, on a PYE Audio Automatic Data Recorder D6600/35P: documents from
`write`, and pages from browzer. The likely way is the Sound Blaster's line
out and line in, with the sound turned into bits the way home computers of
the 1980s did it (FSK), and a checksum on every block.

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
