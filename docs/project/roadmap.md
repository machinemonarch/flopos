# Roadmap

Ideas waiting their turn. None is started until Max picks it, and each
gets a short plan with a rough cost first. What is done is in the
CHANGELOG.

## Planned

- **Packed files, the release after next:** The kernel, the apps and the pictures kept packed on the floppy with a simple, fast LZ, and unpacked as they load. Nothing is packed today. A strong packer halves the kernel and browzer; a simple one saves about a third, roughly 250 KB of floppy, and loads a third faster, since the Pentium unpacks far faster than the drive reads. It needs an unpacker in the boot stub and the app loader, and testing on the laptop.
- **Stereo, later:** The card is Sound Blaster Pro compatible, which plays 8-bit stereo up to 22 kHz (`dsp` says which card: 4.xx would allow 16-bit). FLOPOS plays mono, adding the channels before the costly last step of decoding; stereo roughly doubles that step (Opus from about 10% of the machine to 15-20%). It needs a stereo mode in `audio.c` and each decoder keeping its channels apart.

## Waiting to be picked

- **A chat app:** Like Discord, IRC, WhatsApp or Signal; which is open. IRC is easiest for this machine, given a channel worth being in.
- **A mystical protocol for sharing data:** A client on the Toshiba and a server elsewhere, doing what SFTP, NFS or SMB do without reinventing netcat. It should feel obscure and enticing, like a forgotten metro tunnel, all the way down to how it checks data and starts a session. What it is for is undecided, so it starts with an interview of its product owner.

## Audit, one part at a time

A full read of the code for bugs and holes, too big for one go, so a part
per round, most exposed first. Each round: read every line of the part,
fuzz what takes outside input, check on the bench, fix, and note here
what was found. Sizes are rough.

1. **The network's front door** (about 6,000 lines): network card drivers, IP, TCP, DNS, NTP and NTS. Everything a stranger's packet touches first.
2. **TLS and certificates** (about 5,000): `tls.c`, `x509.c`, the key exchanges and signatures. Checked against test vectors and the TLS bench.
3. **browzer's reading of pages** (about 8,500): HTTP, HTML, CSS, cookies, addresses, playlists, HLS.
4. **The JavaScript room** (about 10,500): the engine, the page objects, the wall around them.
5. **Sound decoders** (about 7,000): mp3, mp2, AAC, MP4, Ogg, Vorbis, Opus, FLAC, trackers. Already fuzzed; this round reads them.
6. **The wall between apps and the system** (about 2,000): `wall.c`, `apps.c`, every call an app can make.
7. **Disk and files** (about 2,500): FAT12 writing, STATE.BIN, the inventory.
8. **maelcum and the tape** (about 2,500): reading tapes, sealing, packing.
9. **The rest of the system** (about 8,000): boot, memory, the screen, keys, the shell and the small apps.
10. **The build tools** (about 4,000): `tools/`, especially anything that writes to a disk.

## Security, waiting for a decision

Nothing at the moment. What was fixed is in Security[^1].

[^1]: [Security](../design/security.md)
