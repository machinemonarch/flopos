# The hardware

FLOPOS was made for one machine: a **Toshiba Satellite Pro 430CDT**, a
laptop model from 1996. Everything below was measured or reported by
FLOPOS on that laptop itself.

## The machine

Each part, what it is, what FLOPOS does with it, and whether FLOPOS needs
a driver of its own for it. **Own driver** means FLOPOS talks to the chip
directly, with code written for it. **Through the BIOS** means FLOPOS asks
the laptop's built-in firmware to do it. **Needs nothing** means the part
just works.

**Processor.** Intel Pentium, 119MHz measured (family 5, model 2, stepping
12), no MMX. FLOPOS times its own speed against the system timer and uses
no instruction newer than a 486. The system itself uses no floating
point; the wizard and browzer's audio decoders use the Pentium's
floating-point unit. *Needs nothing.*

**Memory.** 32MB. Measured at boot. The memory above 1MB holds the screen
buffers and tables, and 12–20MB is kept for apps, walled off from the
rest. *Through the BIOS* (to measure it).

**Video.** Chips & Technologies 65550, 2MB, VESA 2.0. 800×600 in 256
colours (VESA mode 103) through a flat frame buffer at address 0x7000000,
or VGA 320×200 if that fails. *Through the BIOS* (to set the mode), then
*own driver* (FLOPOS draws every pixel itself, and `wide` sets the chip's
panel registers).

**Screen.** 11.3" 800×600 TFT panel. FLOPOS runs it at its native size by
default, so nothing is stretched. *Needs nothing.*

**Sound.** A Sound Blaster Pro compatible sound chip with a Yamaha OPL2 FM
synthesizer in it, and the old PC speaker beeper. Both come out of the
laptop's one built-in loudspeaker, on the front.

- The **FM synthesizer** (port 0x388, three voices) plays the music at the
  prompt. *Own driver.*
- The **Sound Blaster's DSP** plays recordings (songs, MP3, Ogg, FLAC,
  WAV) by DMA; `dsp` checks it. *Own driver.*
- The **PC speaker** is the beeper every PC has: one plain tone at a time.
  FLOPOS plays the music on it only if the FM chip doesn't answer. *Own
  driver.*

The volume knob on the front sets the sound chip's loudness. Whether it
also turns the beeper down has not been checked on this laptop.

**Keyboard.** Built in, Swedish. Swedish by default; sixteen other layouts
with `keyboard`, Cyrillic and Hebrew among them. *Own driver.*

**Floppy.** External 1.44MB drive on a dedicated port. Read and written
*through the BIOS*, which knows how this drive is wired.

**Serial.** COM1 at 0x3F8, IRQ 4, a 16550-compatible chip, at 115200
baud. Everything FLOPOS prints is copied down it, and it accepts typing.
*Own driver.*

**Clock.** CMOS real-time clock. Read twice, and only believed when both
readings agree. *Own driver.*

**Power.** APM BIOS. `bye` asks it to switch the machine off, and the HUD
asks it about the battery. *Through the BIOS.*

**PC Card slots.** Two stacked 16-bit 5V slots; the controller is
compatible with the Intel 82365SL, at port 0x3E0. FLOPOS powers the
slots, reads each card's self-description, and sets up the network card.
*Own driver.*

**Network.** 3Com 3C589D EtherLink III PC Card, 10BASE-T. *Own driver.*

**Modem.** LASAT Credit 560 Voice PC Card. Recognised: it describes itself
as a serial port or modem with a choice of COM port addresses. Not driven
yet.

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
- **Socket 1:** 3Com 3C589D, TP/BNC LAN card.

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
