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
