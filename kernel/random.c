/* ===========================================================================
 * random.c -- unpredictable numbers, on a machine with no source of them.
 *
 * A TLS connection is only as private as the secret this machine picks
 * for the key exchange, and a 1996 PC has nothing built in for picking
 * one.  What it does have is jitter: the cycle counter runs at the
 * processor's speed, the timer chip and the I/O bus run at their own, and
 * how many cycles pass while the bus answers a read wobbles in ways
 * nothing outside the machine can see.  So does the moment a key is
 * pressed, or a frame arrives from the network, measured in cycles.
 *
 * All of that is stirred into a SHA-256 pool, and what comes out is a
 * hash of the pool and a counter -- never the pool itself.  It is an
 * honest, ordinary design.  It is not a hardware generator, and on a
 * machine with no cycle counter it is noticeably weaker.
 * ===========================================================================
 */
#include "flopos.h"
#include "io.h"

static u8  s_pool[32];
static u32 s_count;
static Sha256 s_stir;
static int s_stir_on;

static u32 cycles(void) { return g_cpu_mhz ? tsc32() : g_ticks; }

/* Anything that happens at an unpredictable moment. */
/* Called from the keyboard interrupt as well as from ordinary code, so the
 * pool is only ever touched with interrupts off: a key arriving half way
 * through an update would otherwise run a second update over the first
 * and walk off the end of its buffer. */
void rand_add(u32 v)
{
    u32 t = cycles(), f;
    u8 b[8];
    int i;
    for (i = 0; i < 4; i++) { b[i] = (u8)(v >> (8 * i)); b[4 + i] = (u8)(t >> (8 * i)); }
    f = irq_save();
    if (!s_stir_on) { sha256_init(&s_stir); sha256_update(&s_stir, s_pool, 32); s_stir_on = 1; }
    sha256_update(&s_stir, b, 8);
    irq_restore(f);
}

/* Timing jitter, sampled now: how long reads of the timer chip take. */
static void gather(void)
{
    int i;
    for (i = 0; i < 256; i++) {
        u32 a = cycles();
        outb(0x43, 0x00);                /* latch channel 0                */
        a ^= (u32)inb(0x40) << 8;
        a ^= (u32)inb(0x40);
        rand_add(a ^ cycles());
    }
    rand_add(g_ticks);
}

void rand_bytes(u8 *out, int n)
{
    u8 block[32], seed[36];
    int i, k = 0;

    gather();
    {
        u32 f = irq_save();
        sha256_final(&s_stir, s_pool);   /* the pool absorbs the stirring  */
        s_stir_on = 0;
        irq_restore(f);
    }

    while (k < n) {
        for (i = 0; i < 32; i++) seed[i] = s_pool[i];
        s_count++;
        seed[32] = (u8)s_count; seed[33] = (u8)(s_count >> 8);
        seed[34] = (u8)(s_count >> 16); seed[35] = (u8)(s_count >> 24);
        sha256(seed, 36, block);
        for (i = 0; i < 32 && k < n; i++) out[k++] = block[i];
    }
    /* And the pool moves on, so nothing handed out can be wound back. */
    for (i = 0; i < 32; i++) seed[i] = s_pool[i] ^ 0x5c;
    sha256(seed, 36, s_pool);
}
