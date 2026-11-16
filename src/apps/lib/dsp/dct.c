/* ===========================================================================
 * dct.c -- the cosine transform, fast (B. G. Lee, 1984).
 *
 * A DCT-II of n numbers is two of n/2: the sums of the two halves
 * (one folded back on the other) give the even outputs, and their
 * differences, each divided by 2 cos(pi (2k + 1) / 2n), give the odd ones
 * once neighbouring results are added.  All the way down, 32 numbers take
 * 80 multiplications instead of the 1024 of doing it by its definition.
 * ===========================================================================
 */
#include "dsp.h"

static double fcos(double x) { double r; __asm__("fcos" : "=t"(r) : "0"(x)); return r; }
#define PI 3.14159265358979323846

static float s_fac[7][32];               /* 1 / (2 cos(pi (2k + 1) / 2n)), by log2 n */
static int   s_ready;

static void setup(void)
{
    int lg, k;
    for (lg = 1; lg <= 6; lg++) {
        int n = 1 << lg;
        for (k = 0; k < n / 2; k++) s_fac[lg][k] = (float)(0.5 / fcos(PI * (2 * k + 1) / (2.0 * n)));
    }
    s_ready = 1;
}

static void lee(float *x, int n, int lg)
{
    float t[64];
    const float *c = s_fac[lg];
    int h = n >> 1, k;
    for (k = 0; k < h; k++) {
        float a = x[k], b = x[n - 1 - k];
        t[k] = a + b;
        t[h + k] = (a - b) * c[k];
    }
    if (h > 1) { lee(t, h, lg - 1); lee(t + h, h, lg - 1); }
    for (k = 0; k < h - 1; k++) {
        x[2 * k] = t[k];
        x[2 * k + 1] = t[h + k] + t[h + k + 1];
    }
    x[n - 2] = t[h - 1];
    x[n - 1] = t[n - 1];
}

void dsp_dct2(float *x, int n)
{
    int lg = 0;
    if (!s_ready) setup();
    while ((1 << lg) < n) lg++;
    if (n < 2 || n > 64 || (1 << lg) != n) return;
    lee(x, n, lg);
}
