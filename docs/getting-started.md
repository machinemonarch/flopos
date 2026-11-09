# Getting started

## What you need

- **The laptop**: a Toshiba Satellite Pro 430CDT, or any old PC close
  enough to it. FLOPOS also runs in the QEMU emulator (see
  [Building](building.md)).
- **A 1.44MB floppy disk** (2HD).
- **A Linux PC with a floppy drive**, to write the disk. A USB floppy drive
  is fine.
- Optional: **a serial cable** (a USB-to-serial adapter on the Linux PC,
  plugged into the laptop's serial port). This lets you type at FLOPOS from
  the other computer and see everything it prints.

## Putting it on a floppy

Every released version is kept in `releases/` as `flopos-X.Y.Z.img`, with
its notes in `flopos-X.Y.Z.txt`. To build the current one yourself, run
`make`; the result is `dist/flopos.img`.

Write it with:

    tools/writefloppy.sh

The script:

1. **Finds the floppy drive by itself.** When it has, it says so in big
   green letters. It will not write to anything that isn't exactly the size
   of a floppy, so a typo can't hit a hard disk.
2. **Asks you to type `YES`.**
3. **Keeps what you made.** Documents, the journal and taught words already
   on the floppy are copied into the new version before it is written. Add
   `--fresh` to start clean instead.
4. **Writes only the part of the disk that is in use**, which takes about
   16 seconds instead of over a minute.

Other options: name the device yourself (`tools/writefloppy.sh /dev/sde`),
`-y` to skip the question, `--full` to write all 1.44MB, and `--lowlevel` to
low-level format a suspect disk first (slow).

**No password, once set up.** Writing to a drive normally needs `sudo`. To
make the floppy drive yours for good, run this once, then unplug the drive
and plug it back in:

    sudo cp tools/99-flopos-floppy.rules /etc/udev/rules.d/
    sudo udevadm control --reload

## Booting

Put the floppy in the laptop and switch it on. You will see:

1. **Saffran, painting in from the top** as his picture comes off the
   disk.
2. **A thin green bar** growing along the bottom while the system loads
   underneath him.
3. **The machine telling you what it is doing**, one line per real step,
   in its own dialect, with what each step found at the end of its line:
   the memory, the processor's speed, how many times it has woken up, and
   so on. The bar carries on to the end as the steps finish. Near the end
   the screen switches to 800×600, the panel's own resolution, with
   Saffran still up. Any key skips the typing; the steps still all
   happen.
4. Saffran fading out, and **the wizard** coming up out of the dark: a
   very tired wizard sitting on top of the login box, who says

   > Hold, explorer! For you must answer this riddle before you can enter
   > cyberdelia!

   and asks a riddle made up on the spot ("Where is the holy keyboard that
   forgets?"). Type any answer and press Enter. Every answer is correct,
   even none at all.
5. A little chip-tune jingle (a secret, found), then the screen ripples and
   glows softly while bells climb, and you are in **cyberdelia**, the
   shell:

        FLOPOS
        a place to be, on one floppy disk

        Go on, then.

        here> _

**Hold Shift while it boots** to stay at the old chunky 320×200 picture
instead. `screen big` and `screen small` switch later.

## The serial line

FLOPOS talks on the laptop's serial port (COM1) at 115200 baud, 8 data
bits, no parity, 1 stop bit. On the Linux PC:

    sudo picocom -b 115200 /dev/ttyUSB0

Anything you type there is typed into FLOPOS, and everything it says
appears there too, the wizard's riddle included, so you can answer it
from there. To leave picocom, press **Ctrl-A** then **Ctrl-X**.

A few keys can't cross a serial line, because a terminal sends the same
byte for two different keys:

- Ctrl-I arrives as Tab.
- Ctrl-M arrives as Enter.

Both work on the laptop's own keyboard.

## First things to try

    here> help          the words it knows
    here> look          what is around
    here> mood          change the light
    here> listen        a different rhythm
    here> whaaat        try it
    here> teach hej "Hej hej."
    here> write         the word processor
    here> network       how this machine is connected
    here> browzer       the web
    here> bye           save everything and switch off

## Switching off

Type `bye`. FLOPOS writes anything unsaved to the floppy and switches the
laptop off by itself. If the power can't be cut that way, it tells you it
is safe to switch off.
