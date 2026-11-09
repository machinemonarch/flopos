/* ===========================================================================
 * cpu.c -- who are we running on, and how to leave.
 * ===========================================================================
 */
#include "flopos.h"
#include "io.h"

char g_cpu_vendor[16];
char g_cpu_name[52];
int  g_has_cpuid;
int  g_has_tsc;
u32  g_cpu_mhz;

static int cpuid_supported(void)
{
    u32 before, after;
    /* If bit 21 of EFLAGS can be flipped, CPUID exists. A 386 or 486 that
     * predates it will refuse to keep the change. */
    __asm__ __volatile__(
        "pushfl\n\t"
        "pushfl\n\t"
        "popl %%eax\n\t"
        "movl %%eax, %0\n\t"
        "xorl $0x200000, %%eax\n\t"
        "pushl %%eax\n\t"
        "popfl\n\t"
        "pushfl\n\t"
        "popl %1\n\t"
        "popfl"
        : "=r"(before), "=r"(after) :: "eax");
    return ((before ^ after) & 0x200000) != 0;
}

static void cpuid(u32 leaf, u32 *a, u32 *b, u32 *c, u32 *d)
{
    __asm__ __volatile__("cpuid"
        : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0));
}

static u64 rdtsc(void)
{
    u32 lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}

void cpu_detect(void)
{
    u32 a, b, c, d, i;

    g_cpu_vendor[0] = 0;
    g_cpu_name[0] = 0;
    g_has_cpuid = cpuid_supported();

    if (!g_has_cpuid) {
        const char *s = "386/486 class";
        for (i = 0; s[i]; i++) g_cpu_name[i] = s[i];
        g_cpu_name[i] = 0;
        return;
    }

    cpuid(0, &a, &b, &c, &d);
    *(u32 *)&g_cpu_vendor[0] = b;
    *(u32 *)&g_cpu_vendor[4] = d;
    *(u32 *)&g_cpu_vendor[8] = c;
    g_cpu_vendor[12] = 0;

    if (a >= 1) {
        u32 family, model;
        cpuid(1, &a, &b, &c, &d);
        g_has_tsc = (d & (1u << 4)) != 0;
        family = (a >> 8) & 0xF;
        model  = (a >> 4) & 0xF;
        fmt_str(g_cpu_name, sizeof g_cpu_name, "family %u model %u stepping %u",
                family, model, a & 0xF);
    }

    /* A brand string if the chip is new enough to have one. The Pentium
     * this is aimed at is not, so the family/model line above stands in. */
    cpuid(0x80000000u, &a, &b, &c, &d);
    if (a >= 0x80000004u) {
        u32 *p = (u32 *)g_cpu_name;
        int leaf;
        for (leaf = 0; leaf < 3; leaf++) {
            cpuid(0x80000002u + leaf, &a, &b, &c, &d);
            *p++ = a; *p++ = b; *p++ = c; *p++ = d;
        }
        g_cpu_name[48] = 0;
    }
}

/* Clock speed, measured against the PIT. Needs interrupts running. */
void cpu_measure_mhz(void)
{
    u32 t0, elapsed_ms;
    u64 c0, c1;
    u32 cycles;

    if (!g_has_tsc) { g_cpu_mhz = 0; return; }

    t0 = g_ticks;
    while (g_ticks == t0) { }            /* line up with a tick edge       */
    c0 = rdtsc();
    t0 = g_ticks;
    while (g_ticks - t0 < 50) { }        /* 200ms                          */
    c1 = rdtsc();
    elapsed_ms = (g_ticks - t0) * 4;

    cycles = (u32)(c1 - c0);             /* 200ms fits in 32 bits to 20GHz */
    g_cpu_mhz = cycles / (elapsed_ms * 1000u);
}

/* Switch the machine off, properly.
 *
 * There is no instruction for this on a 1996 laptop; the only thing that
 * can cut its own power is the BIOS, through APM.  So: check it is there,
 * connect in real mode, say which version of the protocol we speak, turn
 * power management on for everything, and then ask for the off state.
 *
 * If any of that is refused the function returns, and the caller is
 * expected to say so and park the machine instead of pretending.
 */
int power_off(void)
{
    u32 r;

    cli();
    snd_silence();

    pic_to_bios();
    r = bios_int(0x15, 0x5300, 0x0000, 0, 0, 0, 0);   /* is APM there?       */
    if (!(r & 0x10000)) {
        (void)bios_int(0x15, 0x5301, 0x0000, 0, 0, 0, 0);      /* connect    */
        (void)bios_int(0x15, 0x530E, 0x0000, 0x0102, 0, 0, 0); /* version 1.2 */
        (void)bios_int(0x15, 0x5308, 0x0001, 0x0001, 0, 0, 0); /* enable      */
        (void)bios_int(0x15, 0x5307, 0x0001, 0x0003, 0, 0, 0); /* and off     */
    }
    pic_to_kernel();

    return 0;                            /* still here: it would not      */
}

void reboot(void)
{
    int i;
    cli();
    snd_silence();

    /* Ask the keyboard controller to pulse the reset line. */
    for (i = 0; i < 1000; i++) {
        if (!(inb(0x64) & 2)) break;
        io_wait();
    }
    outb(0x64, 0xFE);

    /* Still here?  Load a null IDT and raise an interrupt: the resulting
     * triple fault is a reset by any other name. */
    {
        static const struct { u16 limit; u32 base; } __attribute__((packed))
            nullidt = { 0, 0 };
        __asm__ __volatile__("lidt (%0); int $3" :: "r"(&nullidt));
    }
    for (;;) hlt();
}
