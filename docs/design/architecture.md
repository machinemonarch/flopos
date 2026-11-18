# How it works

A map of FLOPOS from power-on to a web page: which file does what. Every
source file opens with a comment explaining itself; read that for the
detail.

## Booting

1. **The boot sector** (`src/boot/boot.S`). Switches to VGA mode 13h and reads two files a track at a time: `SAFFRAN.PIX` into 0x80000, shown as it arrives, then `KERNEL.BIN` into 0x10000. The build writes both files' sector numbers into the boot sector.
2. **The 16-bit stub** (`src/kernel/start.S`). Measures memory, opens the A20 gate, loads a flat descriptor table and enters 32-bit protected mode.
3. **`main`** (`src/kernel/main.c`). Starts memory, interrupts, timer, keyboard, serial line, sound, the saved state, the panel, the text and the shell, then switches to 800×600 unless Shift is held. `src/kernel/arrive.c` narrates each step on screen and as `[boot]` lines on the serial line.
4. **The wizard** (`src/kernel/wizard.c`). A riddle from twenty sentence shapes filled from word lists, chosen by the clock and the cycle counter. Any answer passes.
5. **The way in** (`src/kernel/cyberdelia.c`). A jingle and a few seconds of ripple, then the main loop: read keys, run commands, draw a frame.

## Memory

- **0x10000:** The boot stub and the real-mode thunk every BIOS call goes through, which must stay in the first 64 KB. The rest of the kernel is loaded behind it, then copied up to 1 MB at start (`start.S`).
- **0x70000:** The disk buffer, below 1 MB where the BIOS can reach it.
- **1 MB:** The kernel: code, data and variables (`linker.ld`).
- **After the kernel:** The heap: screen buffers, tables, network buffers, the wizard's model. It only ever grows.
- **12 MB to 20 MB:** The app area, the only memory an app can touch.
- **0x7000000:** The video card's frame buffer.

Paging is on, with 4 MB pages mapping every address to itself; only the
app area is open to apps. It is switched off during each BIOS call
(`src/kernel/wall.c`). FLOPOS needs at least 3 MB in all.

## Interrupts, time and keys

- **`isr.S`, `idt.c`:** Exceptions and hardware interrupts. A crash prints its registers down the serial line, then on a panic screen.
- **`time.c`:** The 8254 timer at 1000 Hz, every fourth tick the system's. A watchdog restarts the timer if it stops for 100 ms of cycle time.
- **`kbd.c`, `keymap.c`, `kbpick.c`:** The keyboard, the layouts (`src/kernel/gen/keymaps.c`, made by `tools/build/mkkeymap.py`), dead keys, Alt+Shift between alphabets, and the Ctrl+Shift+F1 picker.
- **Characters are ints:** FLOPOS's own codes below 0x100, Unicode above (`flopos.h`). In a C string a wide one is four bytes starting with `CH_WIDE_ESC`. Hebrew is reordered only when drawn (`bidi_order` in `draw.c`).

## Borrowing the BIOS

The BIOS does the floppy (`INT 13h`), video modes (`INT 10h`, VESA),
memory size and power off (`INT 15h`). FLOPOS drops to real mode for each
call (`src/kernel/bios.S`), always through `bios_int` in
`src/kernel/bioscall.c`, which hands the interrupt controllers and timer
to the BIOS, makes the call, takes them back, credits the clock with the
time away, and drops input garbled in between.

## The screen

- **`vga.c`:** Frames are composed in memory and only changed rows are sent to the card, capped at about 30 a second. If 800×600 is far too slow at the start, it drops to 320×200.
- **`vbe.c`, `panel.c`:** The video BIOS's modes; the 65550's panel registers for `wide` and for putting the panel back after the switch.
- **`mood.c`:** The moving background, worked out at half size and dithered; its brightness follows the music.
- **`term.c`:** Draws every character by hand: a scrollback grid, four text faces (`squint`), shadows, and a window layer with bold, italic and underline for menus and apps.
- **`line.c`, `shell.c`, `voice.c`, `ui.c`:** The line editor, the command table, every sentence FLOPOS says, and keys for menus.
- **`banana.c`, `vortex.c`, `arrive.c`:** The banana, `whaaat`, and the boot and goodbye narration over Saffran and Ozzy.

No floating point in the system: `mathx.c` has integer sines and roots.

## Sound

- **`sound.c`:** A step sequencer on the OPL2 FM chip, or the PC speaker, run from the timer.
- **`audio.c`:** Recordings: the Sound Blaster plays a 16 KB buffer by DMA, looping, and the timer refills it from a queue apps write into (about six seconds). No card interrupt is used. `audio_listen` runs it the other way, for maelcum's tapes.

## The wizard's words

`src/kernel/wizsay.c` runs a small transformer (about 240,000 byte-sized
weights) from `WIZARD.WIZ`, read once at boot. It is the one kernel file
that uses the FPU. A line is made a piece at a time, handing the machine
back between layers so frames keep coming. The shell calls it only when
nothing else understood a word. Training: Training the wizard[^1].

## The floppy

An ordinary FAT12 disk in two parts. The system's own files are only read,
or overwritten in place at their built size; the top folder is never
written. Everything of the user's is in `INVENTORY`, where FLOPOS does the
whole of FAT12, in an order a power cut cannot turn into a broken disk
(see Techniques[^2]).

- **`disk.c`:** Sectors through the BIOS, via the buffer at 0x70000.
- **`store.c`:** `JOURNAL.TXT` (plain text) and `STATE.BIN` (settings, with a checksum).
- **`fs.c`:** FAT12: finding and reading files, writing system files in place, and the inventory (`inv_` calls).
- **`apps.c`:** Loads `NAME.APP` (see Making apps[^3]).

## The serial line

`src/kernel/serial.c`: COM1 at 115200. Everything printed is copied there,
and typing there is fed in as keys.

## The network

- **`pcic.c`:** The PC Card slots and each card's self-description.
- **`el3.c`, `ne2000.c`, `net.c`:** The 3Com 3C589, the emulator's NE2000, and one interface for both. The timer empties the card into a 48-frame queue.
- **`ip.c`:** ARP, IPv4, ICMP, UDP, DHCP, DNS.
- **`tcp.c`:** TCP with a 16-segment window and SACK.
- **`conn.c`:** One connection for apps: lookup, TCP, and TLS with the certificate check.
- **`tls.c`:** TLS 1.3 and 1.2, client side. See Security[^4].
- **`nts.c`, `clockui.c`:** The clock's time from the network, signed (NTS), and the `clock` menu.
- **`netui.c`:** The `network` menu.
- **`src/apps/browzer/`:** HTTP, HTML, CSS and the script room's door.
- **`src/room/`:** The JavaScript engine, run walled off in its own memory.

[^1]: [Training the wizard](../development/training-the-wizard.md)
[^2]: [Techniques](techniques.md)
[^3]: [Making apps](../development/making-apps.md)
[^4]: [Security](security.md#encryption)
