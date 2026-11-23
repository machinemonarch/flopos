# The hardware

FLOPOS was made for one machine: a **Toshiba Satellite Pro 430CDT** from
1996. Everything here was measured by FLOPOS on that laptop.

## The parts

- **Processor:** Intel Pentium, 119 MHz measured, no MMX. FLOPOS uses no instruction newer than a 486; only the wizard, the audio decoders and the script room use the FPU.
- **Memory:** 32 MB, measured at boot through the BIOS. 12–24 MB is kept for apps, 24–31 MB for the script room.
- **Video:** Chips & Technologies 65550, 2 MB, VESA 2.0. 800×600 in 256 colours (VESA mode 103), flat frame buffer at 0x7000000; VGA 320×200 if that fails. The BIOS sets the mode; FLOPOS draws every pixel.
- **Screen:** 11.3" 800×600 TFT, run at its native size.
- **Sound:** Sound Blaster Pro compatible, with a Yamaha OPL2 FM chip (port 0x388) for the music and the DSP for recordings, by DMA. The PC speaker plays the music if the FM chip doesn't answer.
- **Keyboard:** Built in, Swedish. Sixteen other layouts with `keyboard`.
- **Floppy:** External 1.44 MB drive, read and written through the BIOS.
- **Serial:** COM1 at 0x3F8, IRQ 4, 16550 compatible, 115200 baud.
- **Clock:** CMOS clock, read twice and believed only when both agree.
- **Power:** APM BIOS, for `bye` and the battery in the HUD.
- **PC Card slots:** Two, with an Intel 82365SL compatible controller at 0x3E0.
- **Network:** 3Com 3C589D EtherLink III PC Card, 10BASE-T.
- **Modem:** LASAT Credit 560 Voice PC Card. Recognised, not driven.

## Video modes

`vesa` reports 640×400 (mode 100), 640×480 (101), 800×600 (103) and
1024×768 (105), all 256 colours with the flat frame buffer.

At 320×200, `wide` stretches the picture over the panel through the
65550's flat-panel registers (index 0x3D0, data 0x3D1): FR40 from 0x07 to
0x3F and FR48 from 0x13 to 0x1F. The BIOS rewrites them at every mode
change, so they are written after the mode is set.

## How fast things are

- **One X25519 key exchange:** 284 ms.
- **ML-KEM-768:** 60 ms to make a key, 72 ms to open a share.
- **Drawing every character:** About 3.4 ms a frame.
- **Sending a frame to the card at 800×600:** About 32 ms.

## BIOS quirks

What this BIOS does that FLOPOS works around:

- **Memory size:** `INT 15h AX=E801` reports success with every number zero. Zero is taken as no answer, and `AH=88h` and the CMOS are asked next.
- **The timer:** A floppy call sets the timer back to 18.2 Hz. FLOPOS sets it to 250 Hz again after every BIOS call.
- **Interrupt controllers:** The BIOS must get the 8259s in its own layout during a call, or a tick is lost and the clock stops. Every call goes through `bios_int` (`src/kernel/bioscall.c`).
- **Interrupts:** Code that disables interrupts restores them as they were, never just enables them.
- **The first 64 KB:** The code that returns to 16-bit mode must sit in the kernel's first 64 KB; `src/kernel/linker.ld` refuses a kernel where it doesn't.
- **Below 0x80000:** The kernel's image is loaded whole below Saffran's picture before it is copied up to 1 MB; the linker script checks it fits.
- **The 3Com card:** Its configuration register address is in card units, writing attribute memory switches off its I/O window, and the adapter's enable bit must be set.
