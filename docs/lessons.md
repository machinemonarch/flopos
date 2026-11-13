# Lessons on the way

What cost real time to find, kept because it is true of plenty of
machines and programs like these. Nothing here is needed to use FLOPOS.

## What the machine taught us

A BIOS from 1996 has its own ideas. These are the ones that cost real time
to find, kept here because they are true of plenty of machines from then.

**A BIOS call that succeeds is not the same as one that answered.** Asked
how much memory there is (`INT 15h AX=E801`), the 430CDT's BIOS reports
success with every number set to zero. FLOPOS now treats zero as "no
answer" and asks the older way (`AH=88h`), then reads the CMOS. It never
gives up entirely: with no extended memory at all, it runs in the 640KB of
conventional memory.

**The BIOS changes the timer behind your back.** While it reads or writes
the floppy, the BIOS sets the system timer back to its own 18.2 ticks a
second. FLOPOS runs it at 250, so after every BIOS call the timer is set
back. Without that, everything timed by the clock ran thirteen times slow.

**Every freeze was one bug.** `screen big` hanging, the boot screen
freezing, and `report` locking up all had the same cause. While the video
BIOS was working, a timer tick could arrive while the interrupt controller
was set up for FLOPOS rather than for the BIOS. The tick was lost and the
clock stopped for good, and everything waiting on the clock waited forever.
The fix: every call into the BIOS now goes through one gate (`bioscall.c`),
which hands the interrupt controller to the BIOS and takes it back
properly. As a safety net, if the clock ever stops ticking for a tenth of a
second, it is restarted.

**Don't assume interrupts are on.** Code that switches interrupts off must
put them back the way they were, not simply switch them on. Getting this
wrong leaves the keyboard and timer dead the moment anything touches the
disk, which looks exactly like a frozen machine.

**The 3Com card needed three fixes before it said anything.**

1. A card's configuration register address is given in card address units,
   and FLOPOS was doubling it.
2. Writing the card's attribute memory switched off its I/O window.
3. The adapter's enable bit was never set.

After those, it came up first time.

**An empty slot can look like a card.** The emulated NE2000 driver read an
empty bus as a card with the address FF:FF:FF:FF:FF:FF. It now checks that a
register really holds what was written to it and that the address is
plausible.

**The first 64KB matter.** The code that drops back to the BIOS runs in
16-bit mode and can only see the first 64KB of the kernel. When the boot
photograph made the kernel bigger, that code ended up out of reach. The
linker script now refuses to build a kernel where that can happen.

## What the software taught us

The refill routine calls the clock. The clock's watchdog reads the cycle
counter, then compares it with when the tick was last seen to move. If
the refill came between those two steps, "last seen" was newer than "now",
and the difference wrapped round to hours. A healthy timer was then
restarted for nothing, two or three times a second, with the keyboard's
buffer emptied each time. It showed up first on the laptop's serial cable.
The watchdog now looks with interrupts held off.

Drawing only what changed (1.22) never worked inside browzer. browzer
opens its window again each time it draws, and opening a window marked it
to be drawn whole. So every cell was drawn every time: about 8 ms of each
frame. A window is now drawn whole only when it is new or has changed size.

How busy the processor is, for the HUD's 'Thunking, is the time it
spends halted. FLOPOS halts it whenever it waits for the next interrupt
with nothing to do (`idle_wait`), and counts those cycles. A loop that only
waits is now seen to: called again at once with nothing done in between,
`main_pump` halts. That also kept browzer's window from being drawn on
every pass of its loop, which a loop with nothing to do shouldn't.

The kernel's own memory once grew past 0x70000, where the floppy
and the video BIOS keep their buffers. Every disk read then wrote over the
kernel's variables, and the first one to go was the keyboard's record of
which keys are held. The machine believed Shift was down. `linker.ld` now
refuses to build a kernel that reaches that line.
