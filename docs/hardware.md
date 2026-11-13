# The hardware

FLOPOS was made for one machine: a **Toshiba Satellite Pro 430CDT** from
1996. Everything below was measured or reported by FLOPOS on that laptop
itself.

## The machine

| Part | What it is | What FLOPOS does with it |
|---|---|---|
| Processor | Intel Pentium, 119MHz measured (family 5, model 2, stepping 12). No MMX. | Times its own speed against the system timer. Uses no instruction newer than a 486 and no floating point at all. |
| Memory | 32MB | Measures it at boot. Uses the memory above 1MB for screen buffers and tables, and 12–20MB for apps, walled off from the rest. |
| Video | Chips & Technologies 65550, 2MB, VESA 2.0 | 800×600 in 256 colours (VESA mode 103) through a flat frame buffer at address 0x7000000. Falls back to VGA 320×200. |
| Screen | 11.3" 800×600 TFT panel | Runs at its native size by default, so nothing is stretched. |
| Sound | Sound Blaster Pro compatible, with a Yamaha OPL2 FM chip | Plays music on the OPL2 at port 0x388 (three voices), or on the PC speaker if the OPL2 doesn't answer. Plays recordings (songs, mp3, Ogg, FLAC, WAV) through the Sound Blaster's DSP by DMA; `dsp` checks it. |
| Keyboard | Swedish layout | Its own keyboard driver and Swedish keymap. |
| Floppy | External 1.44MB drive on a dedicated port | Read and written through the laptop's own BIOS, which knows how this drive is wired. |
| Serial | COM1 at 0x3F8, IRQ 4, a 16550-compatible chip | 115200 baud. Everything FLOPOS prints is copied down it, and it accepts typing. |
| Clock | CMOS real-time clock | Read twice, and only believed when both readings agree. |
| Power | APM BIOS | `bye` asks it to switch the machine off. |
| PC Card | Two stacked 16-bit 5V slots, controller compatible with the Intel 82365SL at port 0x3E0 | Powers the slots, reads each card's self-description, and sets up the network card. |
| Network | 3Com 3C589D EtherLink III PC Card, 10BASE-T | Its own driver. |
| Modem | LASAT Credit 560 Voice PC Card | Recognised: it describes itself as a serial port/modem with a choice of COM port addresses. Not driven yet. |

## The video card's modes

What `vesa` reports on the 430CDT:

| Mode | Size | Colours | Frame buffer |
|---|---|---|---|
| 100 | 640×400 | 256 | flat, at 0x7000000 |
| 101 | 640×480 | 256 | flat, at 0x7000000 |
| 103 | 800×600 | 256 | flat, at 0x7000000 |
| 105 | 1024×768 | 256 | flat, at 0x7000000 |

FLOPOS composes each frame at 320×200 and scales it up to 800×600, drawing
the text directly at double size with smoothed letters. Copying 480,000
bytes to the card is the slowest part of a frame (about 32ms), so the big
screen runs at about 15 frames a second. If the first frames take far too
long, it drops back to 320×200 by itself.

At 320×200 the chip can also stretch the picture to fill the panel. Its
flat-panel registers sit behind an index at port 0x3D0 and data at 0x3D1,
and graphics stretching is two values: FR40 from 0x07 to 0x3F and FR48 from
0x13 to 0x1F. `wide` does this, reading the old values first so it can put
them back.

The BIOS rewrites these registers whenever the video mode changes, so
FLOPOS sets the mode once and writes them after.

## The PC Card cards, as they describe themselves

Every PC Card carries a description of itself (its "CIS"). `card` reads and
decodes it:

- **Socket 0:** LASAT COMMUNICATIONS A/S, LASAT Credit 560 Voice. Maker
  0x134, product 0x5600, a serial port or modem, configuration registers at
  0x200, offering the I/O addresses 3F8, 2F8, 3E8 and 2E8. COM1 (3F8) is
  FLOPOS's own serial line, so a modem driver would use 2E8 or 3E8.
- **Socket 1:** 3Com 3C589D, TP/BNC LAN card. MAC address 00:10:4B:xx:xx:xx.

## What the machine taught us

Moved, whole, to [Lessons on the way](lessons.md).

## How fast things are

Measured on the 430CDT by `crypto` and `report`:

| Job | Time |
|---|---|
| One X25519 key exchange | 284 ms |
| ML-KEM-768 key generation | 60 ms |
| ML-KEM-768 opening a key share | 72 ms |
| Moving background, per frame | ~30 ms |
| Drawing every character, per frame | ~3.4 ms |
| Copying a frame to the card at 800×600 | ~32 ms |
