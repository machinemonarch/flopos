/* ===========================================================================
 * time.c -- PIT channel 0 as the system heartbeat.
 *
 * 250Hz: fine enough to sequence sixteenth notes at any sane tempo and to
 * time frames in 4ms steps, coarse enough that the interrupt costs nothing
 * on a 120MHz Pentium.  Channel 2 is left alone for the speaker.
 * ===========================================================================
 */
#include "flopos.h"
#include "io.h"

#define PIT_HZ   1193182u
#define PIT_CH0  0x40
#define PIT_CMD  0x43

volatile u32 g_ticks = 0;

void time_init(void)
{
    u32 div = PIT_HZ / TICK_HZ;          /* 4772 -> 250.04 Hz              */

    outb(PIT_CMD, 0x34);                 /* ch0, lo/hi, mode 2 rate gen    */
    outb(PIT_CH0, (u8)(div & 0xFF));
    outb(PIT_CH0, (u8)(div >> 8));
}

void timer_irq(void)
{
    g_ticks++;
    snd_tick();
}

u32 ms_now(void)
{
    /* ticks * 4 exactly; at 250Hz one tick is 4ms. */
    return g_ticks << 2;
}

void delay_ms(u32 ms)
{
    u32 target = g_ticks + (ms + 3) / 4;
    while ((s32)(g_ticks - target) < 0) __asm__ __volatile__("hlt");
}
