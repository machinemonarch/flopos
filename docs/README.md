# FLOPOS documentation

FLOPOS is an operating system that fits on one 1.44MB floppy disk and boots
a 1996 Toshiba Satellite Pro 430CDT into a place. You arrive at a prompt
that says `here>`, and `here` is not a folder.

These pages cover what it is, why it is the way it is, how to use every part
of it, and how it works inside.

## Using it

| Page | What is in it |
|---|---|
| [Philosophy](project/philosophy.md) | What FLOPOS is for, the rules it is built by, and the voice it speaks in |
| [Getting started](user-guide/getting-started.md) | Putting it on a floppy, booting it, talking to it over a serial cable |
| [Commands](user-guide/commands.md) | Every word it knows, what it says back, and the keys at the prompt |
| [The inventory](user-guide/inventory.md) | Your files and folders, where they live on the floppy, and how safe they are |
| [scribe](user-guide/scribe.md) | The word processor: styles, the Markdown view, saving and opening |
| [maelcum](user-guide/maelcum.md) | Your files kept on an audio cassette, and read back |
| [The network](user-guide/network.md) | The network menu, `ping`, the browzer web browser, and how private it is |

## Under the hood

| Page | What is in it |
|---|---|
| [The hardware](design/hardware.md) | The laptop it was made for, and what FLOPOS knows about each part |
| [How it works](design/architecture.md) | Boot, memory, interrupts, the BIOS, the screen, the disk, the network stack, encryption |
| [Making apps](development/making-apps.md) | Programs that live on the floppy beside the system, and the table of things they can ask for |
| [Building and releasing](development/building.md) | Tools, the emulator, tests, version numbers, the changelog |
| [Techniques](design/techniques.md) | How radio plays without a pause on a 120 MHz Pentium, and the shared pieces it took |
| [Lessons on the way](design/lessons.md) | What the laptop and the software taught us the hard way |
| [Roadmap](project/roadmap.md) | Ideas waiting their turn: a chat app, the mystical protocol; and what became of the cassette and the wizard |
| [Security roadmap](design/security.md) | Known weak spots, what fixing each would take, and what is done |
| [References](project/references.md) | Standards, datasheets and tools consulted along the way |

The history of every release is in [CHANGELOG.md](../CHANGELOG.md).
