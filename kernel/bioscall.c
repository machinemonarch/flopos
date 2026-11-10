/* ===========================================================================
 * bioscall.c -- the only way into the BIOS.
 *
 * FLOPOS borrows the machine's own BIOS for the jobs it does better than
 * anything written from a datasheet: the floppy on Toshiba's own external
 * port, switching the machine off, and asking the video chip for modes.
 * Getting into real mode and back is bios.S.  Everything that has to be
 * true around that trip lives here, because every time it lived in the
 * callers, a caller forgot a step:
 *
 *   - The interrupt controllers point at the BIOS's vectors while it
 *     runs, not at ours.  The video calls once skipped this, and every
 *     freeze this machine has had -- screen big, the boot screen, report
 *     stopping dead at the first delay after the video section -- was a
 *     timer tick arriving inside a video BIOS call, landing on a vector
 *     the BIOS never meant for hardware, and stopping the clock for good.
 *
 *   - The timer runs at the BIOS's own 18.2Hz while the BIOS has the
 *     machine, and at ours again afterwards.  Ours is 250Hz.  Its timer
 *     handler counts ticks to time things -- a floppy motor spinning up, a
 *     flat panel being powered on in the right order -- and at our rate
 *     every one of those waits would be thirteen times too short.  The
 *     disk code set it back for itself; the video BIOS never did, and a
 *     laptop panel sequenced thirteen times too fast is a black panel.
 *
 *   - Our clock is credited with the time the BIOS had the machine, from
 *     the cycle counter, because our timer handler did not run for any of
 *     it.  A floppy read is a fifth of a second; a mode change on a flat
 *     panel can be more.
 *
 *   - The keyboard and the serial line are emptied.  Re-initialising the
 *     interrupt controllers forgets any request that was already raised,
 *     and both of those hold their line raised until somebody reads them,
 *     so a key pressed during a disk read would otherwise never be heard
 *     -- and neither would any key after it.
 * ===========================================================================
 */
#include "flopos.h"
#include "io.h"

u32 bios_int_raw(u16 which, u16 ax, u16 bx, u16 cx, u16 dx, u16 es, u16 di);
extern u16 blk_bx, blk_cx, blk_dx;

void bios_regs(u16 *bx, u16 *cx, u16 *dx) { *bx = blk_bx; *cx = blk_cx; *dx = blk_dx; }

u32 bios_int(u16 which, u16 ax, u16 bx, u16 cx, u16 dx, u16 es, u16 di)
{
    u32 flags, r, t0 = 0;

    flags = irq_save();
    if (g_cpu_mhz) t0 = tsc32();

    pic_to_bios();
    outb(0x43, 0x36);                    /* ch0, lo/hi, square wave        */
    outb(0x40, 0x00);                    /* 65536: the BIOS's 18.2Hz       */
    outb(0x40, 0x00);
    r = bios_int_raw(which, ax, bx, cx, dx, es, di);
    pic_to_kernel();

    time_init();
    if (g_cpu_mhz) clock_credit_cycles(tsc32() - t0);
    clock_rebase();

    kbd_drain();
    serial_drain();

    irq_restore(flags);
    return r;
}
