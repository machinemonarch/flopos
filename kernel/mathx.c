/* ===========================================================================
 * mathx.c -- integer maths.  No FPU is touched anywhere in FLOPOS: the
 * target is a 120MHz Pentium and table lookups beat it every time, plus
 * there is then no FPU state to save in an interrupt.
 *
 * Angles are "brads": 1024 of them to the turn, so wrapping is an AND.
 * SIN()/COS() return -4096..4096, i.e. 1.0 == 1<<12.
 * ===========================================================================
 */
#include "flopos.h"

s16 g_sin[SINLEN];
static u16 s_atan[257];                  /* atan(i/256) in brads, 0..128   */
static u32 s_rnd = 0x1BADB002u;

/* sin(x) for 0 <= x <= pi/2, x and the result in 16.16 fixed point.
 * Taylor to the x^9 term, which is good to about one part in 10^7 over
 * that range -- far beyond what a 12-bit table can hold. */
static s32 sin_fx(s32 x)
{
    /* The products need 64 bits, which the CPU gives us for free from a
     * 32x32 multiply.  The divisions are deliberately kept at 32 bits:
     * a 64-bit divide would call into libgcc, and there is no libgcc. */
    s32 x2 = (s32)(((s64)x * x) >> 16);
    s32 t  = x;
    s32 sum = t;

    t = (s32)(((s64)t * x2) >> 16); sum -= t / 6;
    t = (s32)(((s64)t * x2) >> 16); sum += t / 120;
    t = (s32)(((s64)t * x2) >> 16); sum -= t / 5040;
    t = (s32)(((s64)t * x2) >> 16); sum += t / 362880;
    return sum;
}

void mathx_init(void)
{
    int i;

    /* 2*pi in 16.16 is 411775 (rounded); dividing by SINLEN gives the step
     * per brad, kept inside the multiply to avoid losing bits. */
    for (i = 0; i <= SINLEN / 4; i++) {
        s32 x = (i * 411775) >> 10;   /* 256 * 411775 fits in s32 */
        s32 s = sin_fx(x);
        s32 v = (s * 4096) >> 16;
        if (v > 4096) v = 4096;
        g_sin[i] = (s16)v;
    }
    for (i = SINLEN / 4 + 1; i <= SINLEN / 2; i++)
        g_sin[i] = g_sin[SINLEN / 2 - i];
    for (i = SINLEN / 2 + 1; i < SINLEN; i++)
        g_sin[i] = (s16)(-g_sin[i - SINLEN / 2]);

    /* atan table for iatan2.  atan(t) for t in [0,1] maps to [0,128]
     * brads; derived from the sine table by bisection so there is only one
     * place where trigonometry is defined. */
    for (i = 0; i <= 256; i++) {
        int lo = 0, hi = 128, mid;
        /* find the angle a where tan(a) == i/256, i.e. sin*256 == cos*i */
        while (lo < hi) {
            mid = (lo + hi) >> 1;
            if ((s32)SIN(mid) * 256 < (s32)COS(mid) * i) lo = mid + 1;
            else hi = mid;
        }
        s_atan[i] = (u16)lo;
    }
}

/* Without this every boot produces the same "random" sequence, which is
 * very obvious when it picks which lines to print on the way up. */
void rnd_seed(u32 seed)
{
    s_rnd = seed ? seed : 0x1BADB002u;
}

u32 rnd(void)
{
    /* xorshift32: cheap, and long enough a period for sparks and stars. */
    s_rnd ^= s_rnd << 13;
    s_rnd ^= s_rnd >> 17;
    s_rnd ^= s_rnd << 5;
    return s_rnd;
}

int rnd_range(int lo, int hi)
{
    if (hi <= lo) return lo;
    return lo + (int)(rnd() % (u32)(hi - lo + 1));
}

int isqrt32(u32 v)
{
    u32 rem = 0, root = 0;
    int i;
    for (i = 0; i < 16; i++) {
        root <<= 1;
        rem = (rem << 2) | (v >> 30);
        v <<= 2;
        root++;
        if (root <= rem) { rem -= root; root++; }
        else root--;
    }
    return (int)(root >> 1);
}

int iatan2(int y, int x)
{
    int ax = x < 0 ? -x : x;
    int ay = y < 0 ? -y : y;
    int a;

    if ((ax | ay) == 0) return 0;

    if (ax >= ay) a = s_atan[(ay * 256) / ax];
    else          a = 256 - s_atan[(ax * 256) / ay];

    if (x >= 0) { if (y >= 0) return a;            return (1024 - a) & SINMASK; }
    else        { if (y >= 0) return 512 - a;      return 512 + a; }
}
