# FLOPOS documentation

FLOPOS is an operating system that fits on one 1.44MB floppy disk and boots
a 1996 Toshiba Satellite Pro 430CDT into a place. You arrive at a prompt
that says `here>`, and `here` is not a folder.

These pages cover what it is, why it is the way it is, how to use every part
of it, and how it works inside.

## Using it

| Page | What is in it |
|---|---|
| [Philosophy](philosophy.md) | What FLOPOS is for, the rules it is built by, and the voice it speaks in |
| [Getting started](getting-started.md) | Putting it on a floppy, booting it, talking to it over a serial cable |
| [Commands](commands.md) | Every word it knows, what it says back, and the keys at the prompt |
| [The inventory](inventory.md) | Your files and folders, where they live on the floppy, and how safe they are |
| [scribe](scribe.md) | The word processor: styles, the Markdown view, saving and opening |
| [maelcum](maelcum.md) | Your files kept on an audio cassette, and read back |
| [The network](network.md) | The network menu, `ping`, the browzer web browser, and how private it is |

## Under the hood

| Page | What is in it |
|---|---|
| [The hardware](hardware.md) | The laptop it was made for, what FLOPOS knows about each part, and what the machine taught us |
| [How it works](internals.md) | Boot, memory, interrupts, the BIOS, the screen, the disk, the network stack, encryption |
| [Making apps](making-apps.md) | Programs that live on the floppy beside the system, and the table of things they can ask for |
| [Building and releasing](building.md) | Tools, the emulator, tests, version numbers, the changelog |
| [Techniques](techniques.md) | How radio plays without a pause on a 120 MHz Pentium, and the shared pieces it took |
| [Roadmap](roadmap.md) | Ideas waiting their turn: cassette backup, a chat app, the mystical protocol, a wizard who answers |
| [Security roadmap](security-roadmap.md) | Known weak spots, what fixing each would take, and what is done |
| [Crimes against tokenomics](crimes-against-tokenomics.md) | The most unreasonable requests, and what became of them |
| [References](references.md) | Standards, datasheets and tools consulted along the way |

The history of every release is in [CHANGELOG.md](../CHANGELOG.md).
