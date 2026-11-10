/* ===========================================================================
 * mdct.c -- the inverse MDCT, quickly.
 *
 *   y[n] = sum over k < N/2 of X[k] cos(2 pi/N (n + N/4 + 1/2)(k + 1/2))
 *
 * for N up to 8192.  With M = N/2 that is a DCT-IV of size M,
 *
 *   u[j] = sum over k < M of X[k] cos(pi/M (j + 1/2)(k + 1/2)),
 *
 * read out from j = M/2 onwards, where past the end it carries on by the
 * DCT-IV's own symmetries: u[j] = -u[2M-1-j] for M <= j < 2M, and
 * -u[j-2M] beyond.  The DCT-IV is done through a complex FFT of M/2
 * points: z[k] = X[2k] + i X[M-1-2k], turned by exp(-i pi (k + 1/4)/M),
 * transformed, turned again by exp(-i pi n/M); then u[2n] is the real
 * part and u[M-1-2n] minus the imaginary part.  Checked against the sums written out directly, on random input.
 * ===========================================================================
 */
#include "app.h"
#include "mdct.h"

static double fsin(double x) { double r; __asm__("fsin" : "=t"(r) : "0"(x)); return r; }
static double fcos(double x) { double r; __asm__("fcos" : "=t"(r) : "0"(x)); return r; }
#define PI 3.14159265358979323846

#define MAXN 8192
/* The turning factors for one size, worked out the first time that size
 * is asked for and kept: formats switch between a long and a short block
 * all the time, and working the factors out again at every switch costs
 * more than the transforms do.  Every size there can be fits in the pool:
 * about 1.25 n each, and the sizes double. */
typedef struct {
    float *tw_c, *tw_s;                  /* exp(-i pi (k + 1/4) / M)       */
    float *po_c, *po_s;                  /* exp(-i pi n / M)               */
    float *fft_c, *fft_s;                /* exp(-2 pi i k / (M/2))         */
} Turns;
static Turns s_size[14];                 /* by log2 of n                   */
static float s_pool[3 * MAXN];
static u32   s_pool_used;
static float *s_tw_c, *s_tw_s, *s_po_c, *s_po_s, *s_fft_c, *s_fft_s;
static int   s_n;

static float *carve(int n)
{
    float *p = s_pool + s_pool_used;
    s_pool_used += (u32)n;
    return p;
}

static void setup(int n)
{
    int k, M = n / 2, h = M / 2, lg = 0;
    Turns *t;
    if (s_n == n) return;
    while ((1 << lg) < n) lg++;
    t = &s_size[lg];
    s_n = n;
    if (t->tw_c) {
        s_tw_c = t->tw_c; s_tw_s = t->tw_s; s_po_c = t->po_c; s_po_s = t->po_s;
        s_fft_c = t->fft_c; s_fft_s = t->fft_s;
        return;
    }
    t->tw_c = s_tw_c = carve(h); t->tw_s = s_tw_s = carve(h);
    t->po_c = s_po_c = carve(h); t->po_s = s_po_s = carve(h);
    t->fft_c = s_fft_c = carve(h / 2 + 1); t->fft_s = s_fft_s = carve(h / 2 + 1);
    for (k = 0; k < h; k++) {
        double a = PI * (k + 0.25) / M;
        s_tw_c[k] = (float)fcos(a);
        s_tw_s[k] = (float)-fsin(a);
        s_po_c[k] = (float)fcos(PI * k / M);
        s_po_s[k] = (float)-fsin(PI * k / M);
    }
    for (k = 0; k < h / 2; k++) {
        double a = 2.0 * PI * k / h;
        s_fft_c[k] = (float)fcos(a);
        s_fft_s[k] = (float)-fsin(a);
    }
}

/* In place, forward (e^-i), radix 2, m a power of two. */
static void fft(float *re, float *im, int m)
{
    int i, j, k, len;
    for (i = 1, j = 0; i < m; i++) {
        int bit = m >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (len = 2; len <= m; len <<= 1) {
        int half = len >> 1, step = m / len;
        for (i = 0; i < m; i += len)
            for (k = 0; k < half; k++) {
                float wr = s_fft_c[k * step], wi = s_fft_s[k * step];
                float *ar = re + i + k, *ai = im + i + k, *br = re + i + k + half, *bi = im + i + k + half;
                float tr = *br * wr - *bi * wi, ti = *br * wi + *bi * wr;
                *br = *ar - tr; *bi = *ai - ti;
                *ar += tr;      *ai += ti;
            }
    }
}

/* DCT-IV of size M (X in, u out). */
static void dct4(const float *X, float *u, int M)
{
    static float re[MAXN / 4], im[MAXN / 4];
    int h = M / 2, k;
    for (k = 0; k < h; k++) {
        float a = X[2 * k], b = X[M - 1 - 2 * k];
        re[k] = a * s_tw_c[k] - b * s_tw_s[k];
        im[k] = a * s_tw_s[k] + b * s_tw_c[k];
    }
    fft(re, im, h);
    for (k = 0; k < h; k++) {
        float a = re[k], b = im[k];
        float c = a * s_po_c[k] - b * s_po_s[k], d = a * s_po_s[k] + b * s_po_c[k];
        u[2 * k] = c;
        u[M - 1 - 2 * k] = -d;
    }
}

void imdct(const float *X, float *y, int n)
{
    static float u[MAXN / 2];
    int M = n / 2, i;
    setup(n);
    dct4(X, u, M);
    for (i = 0; i < n; i++) {
        int j = i + M / 2;
        y[i] = j < M ? u[j] : j < 2 * M ? -u[2 * M - 1 - j] : -u[j - 2 * M];
    }
}
