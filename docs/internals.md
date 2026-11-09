# How it works

A tour of FLOPOS from power-on to a web page. Each section names the
source files involved; every file opens with a comment explaining itself.

## Booting

1. **The boot sector** (`boot/boot.S`, 512 bytes). The BIOS loads it at
   address 0x7C00. It switches the screen to VGA mode 13h, then reads two
   files off the floppy, a track at a time with retries:
   - `SAFFRAN.PIX` (his palette, then 320×200 pixels) into 0x80000. After
     every chunk it copies everything of him that has arrived onto the
     screen, so he appears exactly as fast as the drive reads him.
   - `KERNEL.BIN` into 0x10000, underneath him, drawing a two-pixel bar
     along the bottom that reaches halfway when the kernel is in.

   The disk is a real FAT12 floppy, so the build works out where both
   files actually landed and writes those sector numbers into the boot
   sector.
2. **The 16-bit stub** (`kernel/start.S`). The kernel is loaded at 0x10000.
   It measures memory (three ways, see [hardware](hardware.md)), opens the
   A20 gate (three ways, and checks it worked), loads a flat descriptor
   table and switches the processor into 32-bit protected mode.
3. **The 32-bit start**: flat segments, a stack, zeroed memory for the
   kernel's variables.
4. **`main`** (`kernel/main.c`) takes Saffran over from the screen
   (`vga_adopt`) and starts the machine: memory, interrupts, timer,
   keyboard, serial line, the processor's measured speed, sound, the saved
   state from the floppy, the panel, the text, the background, the shell,
   and the switch to 800×600 unless Shift is held.

   `kernel/arrive.c` narrates this. `boot_step` puts up a line just
   before each step runs, and `boot_result` adds what the step found. The
   first few steps happen before the timer exists, so their lines are
   told just after, in order. From the serial line's own step onward, each
   line also goes down the cable as `[boot] ...`.

5. **The wizard** (`kernel/wizard.c`). Saffran fades out and the gate
   comes up: a riddle, built by taking one of twenty shapes ("Where do %A
   %P go to %V?") and filling it from lists of adjectives, nouns and verbs,
   with plurals, "it" forms and "a"/"an" worked out by rule. Which shape
   and which words is decided by the CMOS clock, the date and the
   processor's cycle counter, folded together (FNV-1a) and then drawn with
   a xorshift. The wizard is character art in a window with no paper
   (`term_win_bare`), so the moving background shows around him. Any
   answer is accepted, and the riddle and answer go into the journal.
6. **The way in** (`kernel/cyberdelia.c`). An original chip-tune jingle on
   the PC speaker (a real square wave) doubled on FM, then about three
   seconds of ripple: every row of the finished frame slid sideways along
   two sine waves, through a hook (`g_frame_fx`) the main loop applies
   after the text is drawn, so it works at either screen size. The palette
   glows and drifts between cyan and magenta, gently: one smooth rise and
   fall every 0.8 seconds, at most a ninth of the way to white, nothing
   red. Bells climb an A minor pentatonic run on two FM channels the music
   never uses.

   Then the main loop runs forever: read keys, run commands, draw a
   frame.

## Memory

| Where | What |
|---|---|
| 0x07C00 | The boot sector, while booting |
| 0x10000 | The kernel. Its first 64KB holds the code that returns to 16-bit mode for the BIOS, and the build checks it stays there. |
| 0x70000 | The disk buffer: below 1MB, where the BIOS can reach it |
| below 640KB | The kernel's code, data and stack |
| 0x100000 (1MB) and up | The heap: screen buffers, lookup tables, network buffers. A simple allocator that only ever hands out more. Capped below 9MB. |
| 0xA00000 (10MB) to 14MB | The app area (see [Making apps](making-apps.md)) |
| 0x7000000 | The video card's frame buffer, at 800×600 |

If the BIOS admits to no memory above 1MB, the heap moves into
conventional memory. Everything the system itself needs fits in about
90KB.

## Interrupts and time

- `kernel/isr.S`, `kernel/idt.c`: entry points for the 32 processor
  exceptions and 16 hardware interrupts. The two 8259 interrupt
  controllers are moved out of the way of the exceptions. A crash prints
  its details (which fault, where, the registers) down the serial line
  first, then on a panic screen.
- `kernel/time.c`: the 8254 timer ticks 250 times a second, and
  everything is timed from it. The processor's own cycle counter is used
  as a second opinion. If the timer ever stops ticking for 100ms of cycle
  time, the clock restarts it and logs that it had to.
- `kernel/kbd.c`, `kernel/keymap.c`: the keyboard on IRQ 1, which keys
  are held, a queue of presses, the modifier keys, and the Swedish
  layout.

## Borrowing the BIOS

Some jobs are done better by the laptop's own BIOS, because it knows its
own hardware: reading and writing the floppy (`INT 13h`), switching video
modes (`INT 10h`, VESA), measuring memory and switching the power off
(`INT 15h`).

To call it, FLOPOS drops from 32-bit mode back to 16-bit "real" mode, makes
the call, and comes back (`kernel/bios.S`). Every call goes through one
function, `bios_int` in `kernel/bioscall.c`, which:

1. hands the interrupt controllers back to the BIOS's own layout
2. sets the timer to the BIOS's speed
3. makes the call
4. takes the interrupt controllers back and sets the timer to 250Hz again
5. credits the clock with the time spent away
6. throws away keys and serial bytes that arrived garbled in between

Before this was the only way in, video calls skipped steps 1 and 4, and
that was the cause of every freeze (see [hardware](hardware.md)).

## The screen

- `kernel/vga.c`: everything is drawn into a 320×200, 256-colour buffer
  in memory. At 320×200 that buffer is copied to the screen as it is. At
  800×600 it is scaled up into an 800×600 canvas first, and the text is
  drawn on top at double size, so it stays sharp instead of being blown
  up. Frames are capped at about 30 a second. If the first frames at
  800×600 take over 400ms, FLOPOS drops back to 320×200 by itself.
- `kernel/vbe.c`: asks the video BIOS what modes it has.
- `kernel/panel.c`: the 65550's panel stretching, for `wide`.
- `kernel/mood.c`: the moving background, worked out at 80×50 and blown up
  four times, which is cheap. Its brightness follows the music.
- `kernel/mathx.c`: integer sines, square roots and angles (no floating
  point anywhere).
- `kernel/banana.c`, `kernel/vortex.c`, `kernel/arrive.c`: the banana,
  `whaaat`, and the boot narration over Saffran.

## The terminal

`kernel/term.c` draws every character by hand onto the frame. It has:

- a grid of cells with scrollback, and per-line "quirks" (the text effects
  `quirks` switches off)
- two text faces at each screen size (`squint` switches between them).
  At 800×600 the letters are smoothed with the Scale2x method, worked out
  once when the face is chosen.
- a shadow under every glyph, so text stays readable over the moving
  background
- a **window layer** for menus and full-screen programs: cells with their
  own colours, bold (drawn twice, a pixel apart), italic (each row of the
  letter shifted) and underline

`kernel/line.c` is the line editor at the prompt; `kernel/shell.c` is the
prompt, the tokeniser and the command table; `kernel/voice.c` is every
sentence FLOPOS says and every phrase it recognises; `kernel/ui.c` turns
keys and serial escape sequences into single keypresses for menus and
apps.

## Sound

`kernel/sound.c` is a step sequencer. It plays rhythms on the OPL2 FM chip
when there is one, and on the PC speaker when there isn't, driven from the
timer interrupt so it keeps time whatever else is happening.

It steps aside for effects. `whaaat` slides one voice across four octaves;
the way into cyberdelia plays square-wave notes and bells (`snd_fx_*`) on
FM channels 3 to 5, which the music never uses.

## The floppy

FLOPOS can't grow or shrink files. It doesn't need to: every file it
writes is made at full size when the disk is built, and FLOPOS only ever
overwrites a file's data in place. It never touches the FAT or the
directory, so it can't corrupt the disk, and the disk stays a normal DOS
floppy.

- `kernel/disk.c`: sectors on and off the floppy through the BIOS, in
  runs where possible, via the buffer at 0x70000.
- `kernel/store.c`: `JOURNAL.TXT` (plain text, ending with the DOS
  end-of-file character so every computer stops reading in the right
  place) and `STATE.BIN` (taught words, settings, network settings, with a
  checksum).
- `kernel/fs.c`: reads the FAT12 filesystem: finds any file by name,
  reads it, and writes into it in place. Used for apps and documents.
- `kernel/apps.c`: loads `NAME.APP` files (see
  [Making apps](making-apps.md)).

## The serial line

`kernel/serial.c`: COM1 at 115200 baud. Everything the terminal prints is
copied there, typing there is fed in as keys, and `report` and crashes
write there first.

## The network

Each layer is its own file:

| Layer | File | What it does |
|---|---|---|
| PC Card | `pcic.c` | Powers the slots, reads each card's description, maps its registers into I/O space |
| Cards | `el3.c`, `ne2000.c`, `net.c` | The 3Com 3C589, the emulator's NE2000, and a common interface for both. Polled: no interrupts. |
| Addresses | `ip.c` | ARP (finding the hardware behind an address), IPv4, ICMP (ping), UDP, DHCP (being given an address), DNS (names) |
| Conversations | `tcp.c` | TCP: ordered, reliable, retransmitted. Its receive window is kept small to fit the card's buffer. |
| Web | `http.c`, `url.c` | HTTP/1.0 requests, redirects, and turning HTML into wrapped, readable text with links |
| Secure web | `tls.c` | TLS 1.3, client side |
| Programs | `netui.c`, `browzer.c` | The network menu and the browser |

## Encryption

All written from the published standards and checked against official
test answers before it was trusted:

| File | What |
|---|---|
| `sha256.c` | SHA-256, HMAC, HKDF: the hashing every TLS 1.3 key is derived with |
| `chacha.c` | ChaCha20-Poly1305 encryption |
| `aes.c` | AES-128-GCM encryption |
| `x25519.c` | X25519 key agreement |
| `sha3.c` | SHA-3 and SHAKE, which ML-KEM is built on |
| `mlkem.c` | ML-KEM-768, the post-quantum key agreement |
| `random.c` | Random numbers: timing jitter between the timer and the processor's cycle counter, stirred into a SHA-256 pool |
| `selftest.c` | `crypto`: every one of the above checked on the machine itself |

A TLS connection offers both the hybrid post-quantum key (X25519MLKEM768:
the ML-KEM secret and the X25519 secret, joined) and plain X25519, and
uses whichever the server picks. Certificates are not checked yet.
