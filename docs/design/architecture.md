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
   It measures memory (three ways, see hardware[^1]), opens the
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

- **0x07C00**: The boot sector, while booting
- **0x10000**: The kernel. Its first 64KB holds the code that returns to 16-bit mode for the BIOS, and the build checks it stays there.
- **0x70000**: The disk buffer: below 1MB, where the BIOS can reach it
- **below 640KB**: The kernel's code, data and stack
- **0x100000 (1MB) and up**: The heap: screen buffers, lookup tables, network buffers. A simple allocator that only ever hands out more. Capped below 9MB.
- **in the heap**: The wizard's model: about 1.4MB once its weights are spread out for use
- **0xC00000 (12MB) to 20MB**: The app area, the only memory an app can touch (see Making apps[^2])
- **0x7000000**: The video card's frame buffer, at 800×600

Paging is on, with 4MB pages that map every address to itself; only the
two pages of the app area are open to apps. It is switched off for the
length of each BIOS call and on again after (`kernel/wall.c`,
`kernel/bioscall.c`).

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
  are held, a queue of presses, the modifier keys, and the layout in use
  (the tables in `kernel/keymaps.c`, made by `tools/mkkeymap.py`), with
  the accent keys that wait for a letter and Alt+Shift between the two
  alphabets of a Cyrillic or Hebrew keyboard. `kernel/kbpick.c` is the
  window Ctrl+Shift+F1 opens to choose a keyboard from, drawn over
  anything, like the HUD.
- Characters are ints: FLOPOS's own codes below 0x100, Unicode numbers
  above (`flopos.h`, "characters past one byte"). The terminal, windows,
  the HUD and the line editor keep them in 16 bits; in a C string a wide
  one is four bytes that start with `CH_WIDE_ESC` (`ch_next`, `ch_put`).
  Hebrew is turned right to left only when a row is drawn
  (`bidi_order` in `kernel/draw.c`), so text is always kept in the order
  it was typed. The keyboard's own auto-repeat is dropped. The keys that move
  something (Tab, the arrows, PgUp, PgDn, Backspace, Delete) repeat while
  held, 15 times a second after 0.4 s, made when a program asks for a
  key so that they never pile up behind a busy one.

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
that was the cause of every freeze (see hardware[^1]).

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
  `whaaat`, and the boot narration over Saffran; the same narrator types
  the steps of `bye` and `reboot` over Ozzy, whose picture (`OZZY.PIX`)
  is read off the floppy at boot so leaving never waits on the disk.

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

Recorded sound is `kernel/audio.c`: the Sound Blaster's DSP plays 8-bit
samples from a 16 KB buffer it reads by itself, by DMA on channel 1, looping
forever. Nothing waits on the card's interrupt: 250 times a second the
system timer asks the DMA controller where the card is reading, and fills
the part it has just finished from a queue apps write into (about six
seconds), or with silence. So it doesn't matter which IRQ the card was
given. Apps reach it through the app interface's `audio_*` calls.

The card listens the same way, the other way round (`audio_listen`): it
writes what it hears into the same buffer, and the timer takes it out into
the queue for an app to collect. The mixer picks the line-in or microphone
socket and switches the card's input filter off. This is how `maelcum` reads
a cassette; it read a real tape on the laptop with 1.33.0.

The sequencer steps aside for effects. `whaaat` slides one voice across four octaves;
the way into cyberdelia plays square-wave notes and bells (`snd_fx_*`) on
FM channels 3 to 5, which the music never uses.

## The wizard's words

`kernel/wizsay.c` runs a small transformer (512 word pieces, 3 layers, 96
wide, a memory of 64 pieces; about 240,000 weights) from `WIZARD.WIZ`.
The file is read once at boot, about eight seconds of floppy, so no answer
waits for the drive. Each weight is a byte on the floppy and becomes a
32-bit float in memory.

It is the one kernel file built to use the floating-point unit. Nothing
else in the kernel touches it, so interrupts have nothing to save, and the
sound routine that runs in the timer interrupt already saves it for
itself.

A line is made a piece at a time. Each piece is three layers of sums, a
few milliseconds each on the laptop, and between layers the engine calls
`main_pump`, so frames are drawn and the network answered on time. Sound
is decoded in the timer interrupt and is never behind him. The next piece
is drawn by chance from the thirty likeliest. `@` in a line is where the
typed word goes. The shell calls `wizsay()` only when every other meaning
of a word has failed.

How the model is made is in Training the wizard[^3].

## The floppy

The floppy is an ordinary DOS (FAT12) disk in two parts, treated
differently.

The system's own files are in the top folder. Those FLOPOS only reads, or
overwrites in place at the size the build made them (the journal, the
settings, the bookmarks). The top folder itself is never written.

Everything of the user's is in the folder `INVENTORY`. There FLOPOS does
the whole of FAT12: makes, grows, shrinks, renames, moves and removes
files and folders. Every path is followed down from that folder and
cannot leave it. A save goes in an order a power cut cannot turn into a
damaged disk: data into unused clusters, then the allocation table (the
spare copy first), then the folder entry, then the old clusters are let
go. See The inventory[^4] and
techniques.md[^5].

- `kernel/disk.c`: sectors on and off the floppy through the BIOS, in
  runs where possible, via the buffer at 0x70000.
- `kernel/store.c`: `JOURNAL.TXT` (plain text, ending with the DOS
  end-of-file character so every computer stops reading in the right
  place) and `STATE.BIN` (settings, network settings, with a
  checksum).
- `kernel/fs.c`: the FAT12 filesystem: finds and reads any file by name,
  writes system files in place, and everything in the inventory (the
  `inv_` calls). The same file is built for the PC and tested there
  (`tools/fstest/run.py`).
- `kernel/apps.c`: loads `NAME.APP` files (see
  Making apps[^2]).

## The serial line

`kernel/serial.c`: COM1 at 115200 baud. Everything the terminal prints is
copied there, typing there is fed in as keys, and `report` and crashes
write there first.

## The network

Each layer is its own file:

- **PC Card** (`pcic.c`): Powers the slots, reads each card's description, maps its registers into I/O space
- **Cards** (`el3.c`, `ne2000.c`, `net.c`): The 3Com 3C589, the emulator's NE2000, and a common interface for both. No card interrupts: the timer empties the card into a 48-frame queue a thousand times a second, so nothing is lost while FLOPOS is busy drawing or decoding.
- **Addresses** (`ip.c`): ARP (finding the hardware behind an address), IPv4, ICMP (ping), UDP, DHCP (being given an address), DNS (names)
- **Conversations** (`tcp.c`): TCP: ordered, reliable, retransmitted. A window of sixteen segments; pieces that come out of turn are kept, and every answer says which pieces have come (SACK), so a lost one costs one resend.
- **Connections** (`conn.c`): One connection for apps: lookup, TCP, and for a secure one TLS with the certificate check, behind open, read, write, close
- **Secure web** (`tls.c`): TLS 1.3, and 1.2 in its AEAD-only form, client side
- **Programs** (`netui.c`): The network menu
- **Web** (`apps/browzer/`): BROWZER.APP: HTTP/1.0 GET and POST, redirects, turning HTML into wrapped text with links and form fields (`web.c`), addresses (`url.c`), and the browser itself (`browzer.c`).

## Encryption

How the encryption is built and how a site is made to prove who it is:
Security[^6].

[^1]: [hardware](hardware.md)
[^2]: [Making apps](../development/making-apps.md)
[^3]: [Training the wizard](../development/training-the-wizard.md)
[^4]: [The inventory](../user-guide/inventory.md)
[^5]: [techniques.md](techniques.md)
[^6]: [Security](security.md#encryption)
