# Getting started

## What you need

- **The laptop:** A Toshiba Satellite Pro 430CDT or a PC close to it. FLOPOS also runs in QEMU (see Building[^1]).
- **A 1.44 MB floppy disk** (2HD).
- **A Linux PC with a floppy drive** (USB is fine) to write the disk.
- **Optional, a serial cable:** A USB-to-serial adapter on the PC, to the laptop's serial port. You can then type at FLOPOS from the PC and see everything it prints.

## Putting it on a floppy

Download the newest `flopos-X.Y.Z.img` from the Releases page on GitHub,
or run `make` to build your own as `dist/flopos.img`. Write it with:

    tools/floppy/writefloppy.sh --image flopos-X.Y.Z.img
    tools/floppy/writefloppy.sh --dev           your own build

Alone, it writes the newest image in `releases/`, where `make release`
keeps them.

It finds the floppy drive itself (and writes to nothing that isn't
exactly a floppy's size), asks you to type `YES`, and carries your
inventory, bookmarks, journal and settings over to the new version. If
they wouldn't fit, nothing is written.

- **`/dev/sde`:** Name the drive yourself.
- **`-y`:** Don't ask.
- **`--fresh`:** Start clean, keeping nothing.
- **`--full`:** Write all 1.44 MB. Otherwise an empty inventory writes only the used part (about 16 seconds), and one with files writes it all.
- **`--lowlevel`:** Low-level format a suspect disk first (slow).
- **`--dev`:** Write your own build instead of the newest release.

**No password:** run this once, then replug the drive:

    sudo cp tools/floppy/99-flopos-floppy.rules /etc/udev/rules.d/
    sudo udevadm control --reload

## Booting

Put the floppy in and switch on. Saffran paints in as he loads, a green
bar shows the system loading, and the machine narrates each step (any key
skips the typing). The screen switches to 800×600, and the wizard asks a
riddle. Any answer opens the gate, even none. After a jingle and a ripple
you are at the prompt:

    here> _

- **Hold Shift while it boots** for the 320×200 picture instead.
- **If the screen stays black** with FLOPOS running behind it, press Ctrl-Shift-F5: it switches the screen again from anywhere.

## The serial line

COM1, 115200 baud, 8N1. From the repository on the PC:

    make wire

It finds the adapter, connects, and adds the whole session to
`logs/serial.log`. Type there to drive FLOPOS; Ctrl+] leaves. Your user
needs to be in the `dialout` group (`sudo usermod -aG dialout $USER`,
once, then log in again).

Over serial, Ctrl-I arrives as Tab and Ctrl-M as Enter; use the laptop's
keyboard for those.

## First things to try

    here> help          the words it knows
    here> mood          change the light
    here> whaaat        try it
    here> scribe        the word processor
    here> network       how this machine is connected
    here> browzer       the web
    here> bye           save everything and switch off

`bye` writes anything unsaved and switches the laptop off, or says when
it is safe to.

[^1]: [Building](../development/building.md)
