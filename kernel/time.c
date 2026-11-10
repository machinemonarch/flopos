/* ===========================================================================
 * time.c -- PIT channel 0 as the system heartbeat.
 *
 * 250Hz: fine enough to sequence sixteenth notes at any sane tempo and to
 * time frames in 4ms steps, coarse enough that the interrupt costs nothing
 * on a 120MHz Pentium.  Channel 2 is left alone for the speaker.
 *
 * The chip itself interrupts 1000 times a second, and every fourth of
 * those is the tick.  The other three are for the network card, which
 * holds only a few kilobytes: at 10 Mbit a full one arrives every 1.2 ms,
 * so it is emptied every millisecond, before it can overflow.
 * ===========================================================================
 */
#include "flopos.h"
#include "io.h"

#define PIT_HZ   1193182u
#define PIT_CH0  0x40
#define PIT_CMD  0x43
#define IRQ_HZ   1000                    /* interrupts; a tick every fourth */

volatile u32 g_ticks = 0;

void time_init(void)
{
    u32 div = PIT_HZ / IRQ_HZ;           /* 1193 -> 1000.15 Hz             */

    outb(PIT_CMD, 0x34);                 /* ch0, lo/hi, mode 2 rate gen    */
    outb(PIT_CH0, (u8)(div & 0xFF));
    outb(PIT_CH0, (u8)(div >> 8));
}

void timer_irq(void)
{
    static u32 sub;
    net_tick();
    if (++sub < IRQ_HZ / TICK_HZ) return;
    sub = 0;
    g_ticks++;
    snd_tick();
    audio_tick();
}

void clock_watch(void);

/* ticks * 4 exactly; at 250Hz one tick is 4ms.
 *
 * Every wait in FLOPOS asks this what time it is, which makes it the one
 * place a stopped clock can be caught without touching every loop that
 * waits.  It only referees when interrupts are meant to be on: with them
 * off -- inside an interrupt handler, or on the way through the BIOS --
 * the tick is supposed to be standing still. */
u32 ms_now(void)
{
    u32 f;
    __asm__ __volatile__("pushfl; popl %0" : "=r"(f));
    if (f & 0x200) clock_watch();
    return g_ticks << 2;
}

/* ---- the clock, and keeping it honest -----------------------------------
 * Everything that waits in FLOPOS waits on the tick.  That made a stopped
 * timer the worst failure the machine could have: not a crash with a
 * message, but every delay, every frame and every timeout waiting forever
 * for a tick that was never coming.  The cycle counter keeps running no
 * matter what the interrupt controllers are doing, so it is the referee:
 * if it says a tick is long overdue, the timer is restarted rather than
 * believed.
 */
static u32 s_seen_ticks, s_seen_tsc;
static u32 s_restarts;

u32 clock_restarts(void) { return s_restarts; }

/* Time the BIOS had the machine, which our tick never saw. */
void clock_credit_cycles(u32 cycles)
{
    if (!g_cpu_mhz) return;
    g_ticks += cycles / (g_cpu_mhz * (1000000u / TICK_HZ));
}

void clock_rebase(void)
{
    s_seen_ticks = g_ticks;
    if (g_cpu_mhz) s_seen_tsc = tsc32();
}

/* Put the timer and the interrupt controllers back the way the kernel
 * wants them, and say so down the wire -- this should never be needed,
 * and if it ever is, that is worth knowing. */
void clock_restart(void)
{
    u32 f = irq_save();

    pic_to_kernel();
    time_init();
    kbd_drain();
    serial_drain();
    clock_rebase();
    s_restarts++;
    irq_restore(f);

    serial_print("[clock] the timer had stopped; it has been restarted\n");
}

/* Called from the main loop and from anything that waits.  A tick is due
 * every 4ms; a hundred milliseconds of cycles with none means the timer
 * has stopped, not that it is running late. */
/* With interrupts held off while it looks, because the sound card's
 * refill (audio.c) calls this too, from inside the timer's interrupt: if
 * that came between reading the cycle counter and comparing it, "now" was
 * older than "last seen", the difference wrapped round to hours, and a
 * healthy timer was restarted for nothing -- two or three times a second,
 * with the keyboard's buffer emptied each time. */
void clock_watch(void)
{
    u32 now, f;
    int stopped = 0;

    if (!g_cpu_mhz) return;
    f = irq_save();
    now = tsc32();
    if (g_ticks != s_seen_ticks) {
        s_seen_ticks = g_ticks;
        s_seen_tsc = now;
    } else if ((s32)(now - s_seen_tsc) > (s32)(g_cpu_mhz * 100000u))
        stopped = 1;
    irq_restore(f);
    if (stopped) clock_restart();
}

/* Waits on the tick, refereed by the cycle counter.  If the tick has not
 * arrived when the counter says the whole wait is over and then some, the
 * timer is restarted and the wait ends anyway: a delay that is a little
 * long is a nuisance, and one that never ends is the whole machine. */
void delay_ms(u32 ms)
{
    u32 target = g_ticks + (ms + 3) / 4;

    if (g_cpu_mhz) {
        u32 per_ms = g_cpu_mhz * 1000u;
        u32 start = tsc32();
        u32 left = ms + 20;

        while ((s32)(g_ticks - target) < 0) {
            if (tsc32() - start >= per_ms) {
                start += per_ms;
                if (!--left) { clock_restart(); return; }
            }
        }
        return;
    }

    /* No cycle counter on a 486, so nothing to referee with: trust the
     * tick, which with every BIOS call going through bios_int() is now
     * safe to trust. */
    while ((s32)(g_ticks - target) < 0) hlt();
}
