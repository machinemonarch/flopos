# FLOPOS

An operating system that fits on a 1.44MB floppy disk and boots a 1996
Toshiba Satellite 430CDT into a place.

Not a demo and not a Linux. It is about twenty-seven kilobytes of hand-written
boot sector, protected-mode kernel, VGA driver, keyboard driver, sound engine
and terminal, with nothing underneath it. There is no filesystem to browse and
no directory to be in. You come up at a prompt that says `here>`, and `here` is
not a folder. It is a place.

    FLOPOS
    a place to be, on one floppy disk

    Go on, then.

    here> whoami
    You're alright.

    here> _

## The disk

The image is a genuine FAT12 floppy. You can mount it, read it in DOS,
Windows or Linux, and copy your own files into the free space — it just also
happens to boot. `KERNEL.BIN` is a normal file in the root directory; the boot
sector finds it at the sectors the filesystem actually assigned, which the
build verifies rather than assumes.

## Building

Needs `gcc` with 32-bit support, GNU `as` and `ld`, `mtools`, and `python3`.

    make                        build dist/flopos.img
    make run                    boot it in QEMU
    make shot                   boot headless, capture PNG frames
    make size                   where the kernel's bytes went
    make write DEV=/dev/sdX     write it to a real floppy

Writing to a real disk, carefully:

    sudo dd if=dist/flopos.img of=/dev/sdX bs=512 conv=fsync status=progress

`/dev/sdX` is the whole floppy device, not a partition. Check it twice with
`lsblk` before you press return — `dd` will do exactly as it is told.

## Words it knows

| | |
|---|---|
| `whoami` | who you are |
| `look` | what is around |
| `here` | where you are |
| `mood` | change the light — STILL, WARM, DEEP, DUSK, VOID |
| `wide` | toggle the screen stretch; on by default where the panel allows it |
| `teach` | give it a new word and an answer, kept on the disk |
| `forget` | take one away |
| `learned` | what you have taught it |
| `journal` | what it has written down |
| `remember` | write everything to the disk now |
| `disk` | read a sector back, to prove it can |
| `speed` | where the frame time goes |
| `wire` | the serial line out of here |
| `listen` | a different rhythm |
| `sound` / `quiet` | the sound, on or off |
| `time` | the hour from the CMOS clock, and how long it has been awake |
| `sys` | what this machine actually is |
| `goaway` | wipe the screen |
| `echo` | say something back |
| `version` | which FLOPOS this is |
| `help` | the list |
| `reboot` | start the machine again |

Anything it does not recognise, it says so.

## Keys

Arrows move the caret, Up and Down walk back through what you have typed,
Home and End jump to the ends of the line, Backspace and Delete remove a
character either way, Escape abandons the line. Page Up and Page Down look
back through the scrollback. Ctrl+A, Ctrl+E, Ctrl+U and Ctrl+K behave as they
do anywhere else. Ctrl+Alt+Delete reboots from anywhere, including from the
panic screen.

The keyboard map is **Swedish**. Letters sit where they do on any QWERTY
board, and the symbols are where they are printed on a Swedish one: `"` is
Shift-2, `/` is Shift-7, `@` is AltGr-2, `\` is AltGr-plus. Å, Ä and Ö have
their own glyphs in the font. The two dead keys insert nothing rather than
pretend to compose.

## What it is made of

| | |
|---|---|
| `boot/boot.S` | 512-byte boot sector. Sets VGA mode 13h first so the load is visible, then reads the kernel a track at a time with retries, painting a stripe of colour per chunk. |
| `kernel/start.S` | 16-bit stub: measures RAM, opens the A20 gate three different ways and verifies it, loads a flat GDT, enters protected mode. Then the 32-bit entry: flat segments, a stack, zero the BSS. |
| `kernel/isr.S` `idt.c` | Interrupt tables, the 8259 pair remapped off the exception vectors, and a panic screen that tells you the register values instead of hanging. |
| `kernel/time.c` | The 8254 at 250Hz. Everything is timed off it. |
| `kernel/kbd.c` `keymap.c` | IRQ1, scancode set 1, modifier tracking, and the Swedish layout. |
| `kernel/vga.c` | Mode 13h, a back buffer, retrace-synced blitting, palette loading and cross-fades. |
| `kernel/term.c` | The terminal: 53x25 cells with 200 lines of scrollback, every glyph drawn by hand with a drop shadow so it stays readable over a moving background. |
| `kernel/mood.c` | What the place looks like. Computed at 80x50 and scaled up four times, kept dark on purpose so the text always wins. It breathes with the beat. |
| `kernel/sound.c` | A step sequencer on the PC speaker, and on the OPL2 as well if the machine answers Adlib detection. |
| `kernel/mathx.c` | Sine tables, integer atan2 and square root. Nothing in FLOPOS touches the FPU. |
| `kernel/rtc.c` | The CMOS clock, read properly: twice, and only believed when both readings agree. |
| `kernel/voice.c` | Every word FLOPOS says to anybody, in one table. |

## It runs on the real thing

Booted on a Toshiba Satellite Pro 430CDT, 5 November 2026. What it found
there, and what that cost:

- **Pentium at 119MHz measured**, family 5 model 2 stepping 12. The clock is
  timed by counting TSC cycles against the 8254, and it landed within one
  percent of the nominal 120.
- **The OPL2 answered.** The Sound Blaster Pro's FM chip responds to Adlib
  detection at 0x388 with no driver and no BIOS setup, so the sequencer has
  three real voices on that machine rather than the beeper.
- **The BIOS reports no extended memory at all.** `INT 15h AX=E801` returns
  with carry clear and every register zero -- indistinguishable from a
  successful report of nothing -- so the original code believed it and never
  tried the older calls. FLOPOS booted anyway, because it falls back to
  conventional memory, and ran in 92KB with 370KB to spare. The probe now
  treats a zero as "no answer" and drops through to `AH=88h` and then to the
  CMOS size bytes.

The lesson is worth keeping: on hardware this old, a BIOS call that succeeds
is not the same as a BIOS call that answered.

### Filling the screen

The panel is 800x600 and mode 13h is 320x200, which the VGA outputs as
640x400 -- so the C&T 65550 centres it and leaves a black border. Toshiba's
setup offers "Text Mode Stretch" and nothing at all for graphics.

The chip can do it regardless. Its flat-panel register file lives behind an
index at 0x3D0 and data at 0x3D1, and graphics expansion is two values:
FR40 goes 0x07 to 0x3F, FR48 goes 0x13 to 0x1F, with FR41, FR4D and FR4E
written either way. `panel.c` reads the old values first and can put them
back, and declines entirely if nothing answers on those ports.

The video BIOS rewrites those registers on every mode change, which is why
the DOS utility that does this needs to stay resident in memory. FLOPOS sets
mode 13h once, in the boot sector, and never changes mode again -- so it
writes them afterwards and they simply stay. Type `wide`.

That also makes a VESA driver unnecessary: 800x600x256 would mean moving
480,000 bytes per frame, where stretching 320x200 in hardware moves 64,000
for the same full screen.

## It remembers you

Two files are laid down at build time, each a fixed size and in one piece:
`JOURNAL.TXT` and `STATE.BIN`. Because they never change length, FLOPOS can
overwrite their data sectors in place and never touch the FAT or the
directory -- which is why there is no filesystem driver in here. The build
already did the filesystem's job and left behind the only two numbers the
kernel needs for each: a starting sector and a length.

Writing goes through the machine's own BIOS. The kernel drops out of
protected mode into real mode, calls `INT 13h`, and comes back. Three
things have to be right for that and all three were wrong at first:

- The thunk and its scratch must live in the **first 64K** of the kernel,
  because the 16-bit selectors it uses are based at 0x10000 with a 64K
  limit. The kernel grew past that when the boot photograph was added, and
  the thunk started reading its own far pointer out of unmapped space. The
  linker script now asserts it.
- The **BIOS reprograms the timer** to its own 18.2Hz while servicing the
  disk, so the PIT is set back after every call. Without that, everything
  timed off the clock runs thirteen times slow.
- `bios_disk` starts with `cli` and **must not end with `sti`**, because
  the caller may have had interrupts off deliberately. The flag is saved
  and restored. Forgetting it leaves the timer and the keyboard dead the
  moment anything touches the disk, which looks exactly like the whole
  machine freezing.

`teach` adds a word, `forget` removes one, `learned` lists them, `journal`
shows what has been written down and `remember` flushes. Nothing is written
unless something asks, and a command that saves says so.

`JOURNAL.TXT` is plain text, terminated with the DOS end-of-file character
so that TYPE, Notepad and cat all stop in the right place. Put the floppy in
any machine and read it -- including the list of every word you typed that
FLOPOS did not know, which is how its vocabulary grows between sessions.

## Still not done

Places you can go.

## A note on the hardware

Aimed at a Pentium with no MMX, so there is no instruction in here newer than
a 486 and no floating point anywhere. Memory above 1MB is probed rather than
assumed, and that is where the buffers normally live; the kernel itself and
its stack stay under the 640K line.

If the BIOS will not admit to any usable memory above 1MB, FLOPOS does not
give up — it allocates from conventional memory instead, where there is close
to half a megabyte free between the end of the kernel and the stack, and
everything it needs comes to about ninety kilobytes. `sys` says which it
ended up using. If even that fails it puts a readable sentence on the screen
with the machine's own numbers on it, rather than hanging.

Tested in QEMU as a Pentium and as a 486, at 4MB and 16MB, with and without
an OPL2 chip, and with the conventional-memory fallback forced.

## Licence and provenance

Written from scratch against publicly documented IBM PC/AT, VGA, 8259, 8254,
8042, FAT12 and Yamaha OPL2 interfaces. It contains no vendor firmware, no
driver binaries, and no code or documentation text from Toshiba or anyone
else.
