# Roadmap

Ideas waiting their turn. None is started until Max picks it, and each
gets a short plan with a rough cost first. What is done is in the
CHANGELOG.

## Planned

- **Stereo, later:** The card is Sound Blaster Pro compatible, which plays 8-bit stereo up to 22 kHz (`dsp` says which card: 4.xx would allow 16-bit). FLOPOS plays mono, adding the channels before the costly last step of decoding; stereo roughly doubles that step (Opus from about 10% of the machine to 15-20%). It needs a stereo mode in `audio.c` and each decoder keeping its channels apart.

## Waiting to be picked

- **One connection for many files:** browzer asks in HTTP/1.0, so every page, script and stylesheet opens a new connection with a full TLS handshake, heavy work for a Pentium. Keeping the line open (HTTP/1.1) needs page lengths and pieces (chunked) read in `web.c` and the script fetches, a line kept per site in the kernel, and a retry when a site has quietly hung up. Medium-large; time a slow page first to see what it saves.
- **Tabs in browzer:** Several pages open at once, switched with a key, each keeping its place. It weighs memory (32 MB, with the script room's share) and goes well with one connection kept per site.

- **A chat app:** Like Discord, IRC, WhatsApp or Signal; which is open. IRC is easiest for this machine, given a channel worth being in.
- **A mystical protocol for sharing data:** A client on the Toshiba and a server elsewhere, doing what SFTP, NFS or SMB do without reinventing netcat. It should feel obscure and enticing, like a forgotten metro tunnel, all the way down to how it checks data and starts a session. What it is for is undecided, so it starts with an interview of its product owner.

## Audit, one part at a time

A full read of the code for bugs and holes, too big for one go, so a part
per round, most exposed first. Each round: read every line of the part,
fuzz what takes outside input, check on the bench, fix, and note here
what was found. Sizes are rough.

1. ~~**The network's front door** (about 6,000 lines): network card drivers, IP, TCP, DNS, NTP and NTS. Everything a stranger's packet touches first.~~ Done in 1.48.0.
2. ~~**TLS and certificates** (about 5,000): `tls.c`, `x509.c`, the key exchanges and signatures. Checked against test vectors and the TLS bench.~~ Done in 1.49.0.
3. ~~**browzer's reading of pages** (about 8,500): HTTP, HTML, CSS, cookies, addresses, playlists (HLS's included).~~ Done in 1.49.0.
4. **The JavaScript room** (about 10,500): the engine, the page objects, the wall around them.
5. **Sound decoders** (about 7,500): mp3, mp2, AAC, MP4, Ogg, Vorbis, Opus, FLAC, trackers, HLS's sound pieces (transport stream and MP4 unpacking, in browzer), and the small bit-reading and sound libraries under them. Already fuzzed; this round reads them.
6. **The wall between apps and the system** (about 2,500): `wall.c`, `apps.c`, every call an app can make, and the apps' side of it (`src/apps/sdk`).
7. ~~**Disk and files** (about 1,800)~~ Done in 1.49.0: `disk.c`, `fs.c`, `store.c` (STATE.BIN), the inventory app, and the file calls in `wall.c` and the SDK. Done only when every row below names its proof, kept in `test/fstest/evidence/` or a commit:
   - **Doors:** every way outside bytes get in (a floppy made on another PC: boot sector, FAT, folders; an app's names, paths and buffers; STATE.BIN) listed first. Proof: `doors.txt`.
   - **Power cut:** QEMU killed at every disk write of a save, new folder, delete, move and STATE.BIN save; after each, the floppy checks clean and the file is wholly old or wholly new. Proof: the run's output.
   - **Bad floppy:** AFL++ on the mount, find, read and write code with broken FATs, looping chains and odd folder entries. Proof: the run's output and coverage.
   - **Failure paths:** disk full, read error, floppy pulled, out of memory: an error back, nothing half-written. Proof: an fstest case each.
   - **Paths:** no name gets out of INVENTORY or onto a system file (`..`, `/`, long names, 8.3 clashes, empty names). Proof: an fstest case each.
   - **Buffers that cut silently:** each fixed size listed with what happens at its edge. Proof: `buffers.txt`.
   - **SEI CERT C:** INT30-32, ARR30, STR31, EXP33, MEM30-35, ERR33 read line by line. Proof: `cert.txt`, one line per hit or "none".
   - **CWE Top 25:** each of the 25 marked as found, fixed or not reachable here, with why. Proof: `cwe.txt`.
   - **Tools:** gcc -fanalyzer, cppcheck, clang scan-build, ASan and UBSan on fstest. Proof: each tool's output, every warning answered.
   - **Savings:** nm sizes, dead code, oversized buffers, with RAM and floppy numbers before and after. Proof: `savings.txt`.
   - **Privacy:** what the files keep about Max (times, journal lines). Proof: `doors.txt`.
   - **Docs:** the docs about files and STATE.BIN match the code. Proof: the commit.
   - **Per finding:** a severity, reproduced, checked in 1.48.0, a repo test, and whether it is proven only in QEMU. Trade-offs to Max one by one at the end.
8. **maelcum and the tape** (about 2,500): reading tapes, sealing, packing.
9. **The rest of the system** (about 8,500): the boot sector, memory, the screen, keys, the shell, scribe and the inventory.
10. **The build** (about 4,000): `tools/` and the Makefile, especially anything that writes to a disk, and the generated tables checked against what made them.
11. **The tests themselves** (about 4,300): `test/`. A test that cannot fail hides bugs.

## Security, waiting for a decision

Nothing at the moment. What was fixed is in Security[^1].

[^1]: [Security](../design/security.md)
