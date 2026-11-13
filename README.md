# FLOPOS

An operating system that fits on a 1.44MB floppy disk and boots a 1996
Toshiba Satellite Pro 430CDT into a place.

    FLOPOS
    a place to be, on one floppy disk

    Go on, then.

    here> whoami
    You're alright.

    here> _

Not a demo and not a Linux. Written from nothing: a boot sector, a
protected-mode kernel, its own drivers for the screen, keyboard, sound,
floppy, serial line and PC Card network card, a network stack with TLS 1.3
and post-quantum key exchange, a web browser, a word processor, and a
shell where `here` is not a folder. It fits on one floppy, with room to
spare.

It is an art project, and its rule is that everything runs on the laptop's
own hardware: a 120MHz Pentium with 32MB of memory. No helper machines, no
proxies, no JavaScript.

## What it does

- **Boots to 800×600** with smooth text over a slowly moving, generated
  background, with music on the laptop's FM sound chip and a wireframe
  banana tumbling in the corner.
- **Talks back** in a warm deadpan, and keeps
  a journal on the floppy that any computer can read.
- **Gets online**: DHCP, DNS, `ping`, `browzer fetch` for reading a page as text,
  and **browzer**, a small web browser with working forms, that shows
  images as captioned boxes and searches through FrogFind.
- **Speaks TLS 1.3** with X25519MLKEM768, ML-KEM-768, ChaCha20-Poly1305 and
  AES-128-GCM, and makes every site prove who it is: certificate chains
  checked with RSA and ECDSA up to Mozilla's trusted authorities. All of it
  is written from the standards and checked against official test vectors
  on the machine itself.
- **`scribe`**, a word processor with bold, italic, underline, a title and
  four headings, saving Markdown documents to the floppy, with a view for
  editing the Markdown directly.
- **`maelcum`**, a backup program that keeps your files on an ordinary
  audio cassette as sound and reads them back: packing (LZSS), two
  speeds, verifying, a check on every piece, and a running commentary.
  Read back from a real recorder on the laptop.
- **An inventory** of your own files and folders, as plain FAT12 files
  any computer can read.
- **Apps load from the floppy** beside the system, so programs don't make
  the system bigger: `scribe`, `browzer`, `inventory` and `maelcum` are
  apps. Each runs behind a wall: it can touch only its own memory and
  must ask the system for everything else.

## Quick start

    make                      build dist/flopos.img
    make run                  boot it in QEMU
    tools/writefloppy.sh      put it on a real floppy

Needs `gcc` with 32-bit support, binutils, `mtools` and `python3`; QEMU to
run it without the laptop.

## On the floppy

Put it in any computer: it is an ordinary DOS floppy. `INVENTORY` is the
folder with everything you have made, as plain files. Everything beside
it is FLOPOS itself and its own records; leave those where they are.

## Documentation

Everything is in **[docs/](docs/README.md)**:

- [Philosophy](docs/philosophy.md): what it is and the rules it's built by
- [Getting started](docs/getting-started.md): floppy, booting, the serial line
- [Commands](docs/commands.md): every word it knows
- [The inventory](docs/inventory.md): your files and folders
- [scribe](docs/scribe.md): the word processor
- [maelcum](docs/maelcum.md): backup to an audio cassette
- [The network](docs/network.md): the menu, browzer, and how private it is
- [The hardware](docs/hardware.md): the laptop and its parts
- [Lessons on the way](docs/lessons.md): what the laptop and the software taught us
- [How it works](docs/internals.md): a tour of the inside
- [Making apps](docs/making-apps.md): programs that live on the floppy
- [Building and releasing](docs/building.md)
- [References](docs/references.md): standards and documents consulted

What changed in each release: [CHANGELOG.md](CHANGELOG.md).

## The disk

The image is a genuine FAT12 floppy. Mount it or read it in DOS, Windows,
Linux or a Mac and you'll find the system, the journal and your documents
as ordinary files. It just also happens to boot.

## Provenance

Written from scratch against published standards and the publicly
documented PC interfaces. Toshiba's manuals and drivers were consulted for
facts about the machine; none of their code, firmware or text is included,
and they are not in this repository. See
[References](docs/references.md).

Directed by Max. Built by Claude.
