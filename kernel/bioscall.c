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
 *   - The timer is set back to our rate afterwards.  The BIOS puts it
 *     back to its own 18.2Hz for disk work.
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

u32 bios_int(u16 which, u16 ax, u16 bx, u16 cx, u16 dx, u16 es, u16 di)
{
    u32 flags, r, t0 = 0;

    flags = irq_save();
    if (g_cpu_mhz) t0 = tsc32();

    pic_to_bios();
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
