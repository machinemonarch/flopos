# FLOPOS documentation

FLOPOS is an operating system that fits on one 1.44MB floppy disk and boots
a 1996 Toshiba Satellite Pro 430CDT into a place. You arrive at a prompt
that says `here>`, and `here` is not a folder.

The pages are in four parts: using it, working on it, how it is made, and
the project around it. Each fact has one home; other pages link to it.

## User guide

| Page | What is in it |
|---|---|
| [Getting started](user-guide/getting-started.md) | Putting it on a floppy, booting it, talking to it over a serial cable |
| [Commands](user-guide/commands.md) | Every word it knows, what it says back, and the keys at the prompt |
| [The inventory](user-guide/inventory.md) | Your files and folders, where they live on the floppy, and how safe they are |
| [scribe](user-guide/scribe.md) | The word processor: styles, the Markdown view, saving and opening |
| [maelcum](user-guide/maelcum.md) | Your files kept on an audio cassette, and read back |
| [The network](user-guide/network.md) | The network menu, `ping`, the browzer web browser, and how private it is |

## Development

| Page | What is in it |
|---|---|
| [Building](development/building.md) | Tools, what is in the project, the floppy image, the emulator |
| [Testing](development/testing.md) | What is tested where, and the test tools |
| [Making apps](development/making-apps.md) | Programs that live on the floppy beside the system, and the table of things they can ask for |
| [Releasing](development/releasing.md) | Version numbers, the changelog, making a release |
| [Training the wizard](development/training-the-wizard.md) | How his model is taught, on a PC |

## Design

| Page | What is in it |
|---|---|
| [Architecture](design/architecture.md) | Boot, memory, interrupts, the BIOS, the screen, the disk, the network stack |
| [The hardware](design/hardware.md) | The laptop it was made for, and what FLOPOS knows about each part |
| [Techniques](design/techniques.md) | How radio plays without a pause on a 120 MHz Pentium, and the other inventions |
| [The cassette format](design/cassette-format.md) | How files become sound on a tape, and are read back |
| [Security](design/security.md) | The encryption, how a site proves who it is, and the security review |
| [Lessons on the way](design/lessons.md) | What the laptop and the software taught us the hard way |

## Project

| Page | What is in it |
|---|---|
| [Philosophy](project/philosophy.md) | What FLOPOS is for, the rules it is built by, and the voice it speaks in |
| [Roadmap](project/roadmap.md) | Ideas waiting their turn, open security items, and what is done |
| [References](project/references.md) | Standards, datasheets and tools consulted along the way |

The history of every release is in [CHANGELOG.md](../CHANGELOG.md).
Reporting a security problem: [SECURITY.md](../SECURITY.md).
