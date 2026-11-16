/* ===========================================================================
 * aac.c -- AAC, as the radio sends it, into mono samples.
 *
 * A station that says "aac" sends ADTS: frames, each with a seven-byte
 * header (the sync word, the sample rate, the channels, the frame's
 * length) and then one block of 1024 samples per channel, coded as:
 *
 *   for each channel, how the block is cut up in time (one long window,
 *   or eight short ones for a sudden sound), which codebook each band of
 *   frequencies uses, a scale for each band, and the band's numbers, each
 *   packed with one of eleven Huffman codebooks;
 *
 *   between the two channels of a pair, mid/side ("both channels'
 *   sum and difference were sent") and intensity ("this band of the
 *   right channel is the left channel, quieter"), and for noisy bands no
 *   numbers at all, only how loud the noise is (noise substitution);
 *
 *   and temporal noise shaping, a small filter run along the spectrum to
 *   put the noise of the coding where the sound is loud in time.
 *
 * Then an inverse MDCT per window, the windows (sine or Kaiser-Bessel,
 * chosen per block), and overlap with the block before.  This is AAC LC,
 * the kind every station sends.  HE-AAC ("aacp") is AAC LC at half the
 * rate with a little extra data that rebuilds the treble; that extra is
 * skipped, and the half-rate core played as it is -- which, for a card
 * that plays 22 kHz anyway, loses next to nothing.
 *
 * Mono, by adding a pair's two spectra before the transform when both
 * use the same windows (it is linear), so there is one transform, not
 * two.  At 32 kHz and up it works at half rate, as mp3.c and vorbis.c
 * do: half the spectrum, half-size transforms.  Written from ISO/IEC
 * 13818-7 and 14496-3; the codebooks are in aactab.h.
 * ===========================================================================
 */
#include "decode.h"
#include "mdct.h"
#include "gen/aactab.h"
#include "prefix.h"                      /* src/apps/lib/bits: the bit reader and the codebooks */
#include "dsp.h"                         /* src/apps/lib/dsp: quick rounding */

void *memcpy(void *d, const void *s, u32 n);

static double fsin(double x) { double r; __asm__("fsin" : "=t"(r) : "0"(x)); return r; }
static double fsqrt(double x) { double r; __asm__("fsqrt" : "=t"(r) : "0"(x)); return r; }
#define PI 3.14159265358979323846

enum { ONLY_LONG, LONG_START, EIGHT_SHORT, LONG_STOP };
enum { ZERO_HCB = 0, NOISE_HCB = 13, INTENSITY_HCB2 = 14, INTENSITY_HCB = 15 };
#define MAXCH 8

/* ---- bits, most significant first, and the codebooks ----------------------------
 * Through src/apps/lib/bits: the reader takes up to 25 bits a load, and each
 * codebook is a table indexed by the next nine bits, so a codeword costs a
 * lookup or two rather than a branch per bit (prefix.h). */
static Bits s_b;
#define s_over (s_b.over)
static u8 s_frame[8192 + 8];             /* this frame, with room to read past its end */

static inline u32 gb(int n) { return bits_get(&s_b, n); }
static inline int bit(void) { return (int)bits_get(&s_b, 1); }

static Prefix s_book[12];                /* 1..11 the spectral books, 0 the scalefactors */
static u32    s_store[24576];

static inline int huff(int book) { return prefix_get(&s_book[book], &s_b); }

/* Each codeword's two or four numbers, worked out once rather than by
 * division at every codeword (a division is 46 cycles on a Pentium), and
 * how many of them are not zero (for the books that send signs after). */
static signed char s_unpack[1241][4];
static u8 s_nonzero[1241];

/* ---- fixed tables ---------------------------------------------------------------- */
static float s_pow43[8208];              /* |q|^(4/3)                        */
static float s_pow2[256];                /* 2^((sf - 100)/4)                 */
static float s_win_l[2][1024], s_win_s[2][128];   /* rising halves: sine, KBD */
static int   s_win_n;                    /* the long window's length they are for */
static int   s_ready;

/* 2^(e/4), for any e. */
static double p2q(int e)
{
    static const double q[4] = { 1.0, 1.189207115002721, 1.414213562373095, 1.681792830507429 };
    double v;
    int w = e >= 0 ? e / 4 : -((-e + 3) / 4), k;
    v = q[e - 4 * w];
    for (k = 0; k < w && k < 200; k++) v *= 2.0;
    for (k = 0; k > w && k > -200; k--) v *= 0.5;
    return v;
}

static double bessel_i0(double x)
{
    double s = 1.0, t = 1.0, q = x * x / 4.0;
    int k;
    for (k = 1; k < 80; k++) {
        t *= q / ((double)k * k);
        s += t;
        if (t < s * 1e-12) break;
    }
    return s;
}

/* The rising half of a window n long: sine, or Kaiser-Bessel-derived. */
static void make_window(float *w, int n, int kbd, double alpha)
{
    int half = n / 2, i;
    if (!kbd) {
        for (i = 0; i < half; i++) w[i] = (float)fsin(PI / n * (i + 0.5));
        return;
    } else {
        static double k[1025];
        double sum = 0, cum = 0;
        for (i = 0; i <= half; i++) {
            double x = 2.0 * i / half - 1.0;
            k[i] = bessel_i0(PI * alpha * fsqrt(1.0 - x * x));
            sum += k[i];
        }
        for (i = 0; i < half; i++) { cum += k[i]; w[i] = (float)fsqrt(cum / sum); }
    }
}

static void tables(void)
{
    int b, i;
    u32 used = 0;
    if (s_ready) return;
    for (b = 0; b < 12; b++) {
        static u32 codes[289];
        const u8 *lens;
        int n;
        if (b == 0) {
            for (i = 0; i < 121; i++) codes[i] = AAC_SFCODE[i];
            lens = AAC_SFLEN; n = 121;
        } else {
            n = AAC_HSTART[b + 1] - AAC_HSTART[b];
            for (i = 0; i < n; i++) codes[i] = AAC_HCODE[AAC_HSTART[b] + i];
            lens = AAC_HLEN + AAC_HSTART[b];
        }
        if (!prefix_build(&s_book[b], s_store + used, (u32)(sizeof s_store / 4) - used, 9, codes, lens, n, 0))
            return;                      /* cannot happen: the books are checked */
        used += s_book[b].used;
    }
    /* |q|^(4/3) = q times its cube root, the root by Newton's method. */
    s_pow43[0] = 0;
    for (i = 1; i < 8208; i++) {
        double x = i, y = i < 8 ? 1.0 : i < 512 ? 4.0 : 16.0;
        int k;
        for (k = 0; k < 40; k++) {
            double ny = (2.0 * y + x / (y * y)) / 3.0;
            if (ny == y) break;
            y = ny;
        }
        s_pow43[i] = (float)(x * y);
    }
    for (i = 0; i < 256; i++) s_pow2[i] = (float)p2q(i - 100);
    for (b = 1; b < 12; b++)
        for (i = 0; i < AAC_HSTART[b + 1] - AAC_HSTART[b]; i++) {
            signed char *v = s_unpack[AAC_HSTART[b] + i];
            int k, nz = 0;
            switch (b) {
            case 1: case 2: v[0] = (signed char)(i / 27 - 1); v[1] = (signed char)((i / 9) % 3 - 1);
                            v[2] = (signed char)((i / 3) % 3 - 1); v[3] = (signed char)(i % 3 - 1); break;
            case 3: case 4: v[0] = (signed char)(i / 27); v[1] = (signed char)((i / 9) % 3);
                            v[2] = (signed char)((i / 3) % 3); v[3] = (signed char)(i % 3); break;
            case 5: case 6: v[0] = (signed char)(i / 9 - 4); v[1] = (signed char)(i % 9 - 4); break;
            case 7: case 8: v[0] = (signed char)(i / 8); v[1] = (signed char)(i % 8); break;
            case 9: case 10: v[0] = (signed char)(i / 13); v[1] = (signed char)(i % 13); break;
            default: v[0] = (signed char)(i / 17); v[1] = (signed char)(i % 17); break;
            }
            for (k = 0; k < (b < 5 ? 4 : 2); k++) nz += v[k] != 0;
            s_nonzero[AAC_HSTART[b] + i] = (u8)nz;
        }
    s_ready = 1;
}

/* ---- one channel's stream ------------------------------------------------------------ */
typedef struct {
    int ws, shape, max_sfb, groups, glen[8], nwin;
    u8  cb[8][64];
    int sf[8][64];
    int tns_on, nfilt[8], coef_res[8];
    int flen[8][3], forder[8][3], fdir[8][3], fcomp[8][3], fcoef[8][3][32];
} Ics;

static struct {
    int sri, rate, cfg, half;            /* the stream                       */
    int nlong, nshort;                   /* coefficients in use per window   */
    const unsigned short *swb_l;
    const unsigned char *swb_s;
    int nswb_l, nswb_s;
    int prev_shape[MAXCH];
    float ov[1024];                      /* the second half of the last block */
    float acc[2048];                     /* this block, every channel added  */
    int nch;                             /* channels added into acc          */
    s16 out[4 * 1024];
    int out_n, out_at;
    int synced;
    u32 rng;
} C;

static Ics   s_ics[2];
static int   s_q[2][1024];               /* quantised numbers                */
static float s_spec[2][1024];
static u8    s_ms[8][64];
static int   s_ms_present;

static int ics_info(Ics *s)
{
    int i;
    if (gb(1)) return 0;                 /* reserved                         */
    s->ws = (int)gb(2);
    s->shape = (int)gb(1);
    if (s->ws == EIGHT_SHORT) {
        int grouping;
        s->max_sfb = (int)gb(4);
        grouping = (int)gb(7);
        s->nwin = 8;
        s->groups = 1;
        s->glen[0] = 1;
        for (i = 0; i < 7; i++) {
            if (grouping & (1 << (6 - i))) s->glen[s->groups - 1]++;
            else s->glen[s->groups++] = 1;
        }
        if (s->max_sfb > C.nswb_s) return 0;
    } else {
        s->max_sfb = (int)gb(6);
        s->nwin = 1;
        s->groups = 1;
        s->glen[0] = 1;
        if (gb(1)) return 0;             /* prediction: AAC Main, not LC     */
        if (s->max_sfb > C.nswb_l) return 0;
    }
    return !s_over;
}

static int section_data(Ics *s)
{
    int g, k, bits = s->ws == EIGHT_SHORT ? 3 : 5, esc = (1 << bits) - 1;
    for (g = 0; g < s->groups; g++) {
        k = 0;
        while (k < s->max_sfb) {
            int cb = (int)gb(4), len = 0, incr, j;
            if (cb == 12) return 0;
            do { incr = (int)gb(bits); len += incr; } while (incr == esc && !s_over);
            if (s_over || k + len > s->max_sfb) return 0;
            for (j = k; j < k + len; j++) s->cb[g][j] = (u8)cb;
            k += len;
        }
        for (; k < 64; k++) s->cb[g][k] = ZERO_HCB;
    }
    return 1;
}

static int scale_factors(Ics *s, int global)
{
    int g, b, sf = global, is = 0, noise = global - 90, first_noise = 1;
    for (g = 0; g < s->groups; g++)
        for (b = 0; b < s->max_sfb; b++) {
            int cb = s->cb[g][b], t;
            if (cb == ZERO_HCB) { s->sf[g][b] = 0; continue; }
            if (cb == INTENSITY_HCB || cb == INTENSITY_HCB2) {
                if ((t = huff(0)) < 0) return 0;
                is += t - 60;
                s->sf[g][b] = is;
            } else if (cb == NOISE_HCB) {
                if (first_noise) { first_noise = 0; t = (int)gb(9) - 256; }
                else { if ((t = huff(0)) < 0) return 0; t -= 60; }
                noise += t;
                s->sf[g][b] = noise;
            } else {
                if ((t = huff(0)) < 0) return 0;
                sf += t - 60;
                if (sf < 0 || sf > 255) return 0;
                s->sf[g][b] = sf;
            }
        }
    return !s_over;
}

static int tns_data(Ics *s)
{
    int w, f, i, lng = s->ws != EIGHT_SHORT;
    for (w = 0; w < s->nwin; w++) {
        s->nfilt[w] = (int)gb(lng ? 2 : 1);
        if (s->nfilt[w]) s->coef_res[w] = (int)gb(1);
        for (f = 0; f < s->nfilt[w]; f++) {
            s->flen[w][f] = (int)gb(lng ? 6 : 4);
            s->forder[w][f] = (int)gb(lng ? 5 : 3);
            if (s->forder[w][f]) {
                int len;
                s->fdir[w][f] = (int)gb(1);
                s->fcomp[w][f] = (int)gb(1);
                len = s->coef_res[w] + 3 - s->fcomp[w][f];
                for (i = 0; i < s->forder[w][f]; i++) s->fcoef[w][f][i] = (int)gb(len);
            }
        }
    }
    return !s_over;
}

/* One codeword's two or four numbers into v. */
static int spec_word(int cb, int *v)
{
    int idx = huff(cb), i, n = cb < 5 ? 4 : 2, at, nz;
    const signed char *u;
    if (idx < 0) return 0;
    at = AAC_HSTART[cb] + idx;
    u = s_unpack[at];
    for (i = 0; i < n; i++) v[i] = u[i];
    if (cb == 1 || cb == 2 || cb == 5 || cb == 6) return 1;     /* signed already */
    /* The unsigned books: a sign bit for each number not zero, all read
     * at once. */
    if ((nz = s_nonzero[at]) != 0) {
        u32 sg = gb(nz);
        for (i = 0; i < n; i++)
            if (v[i] && ((sg >> --nz) & 1)) v[i] = -v[i];
    }
    if (cb == 11)
        for (i = 0; i < 2; i++)
            if (v[i] == 16 || v[i] == -16) {
                int e = 4, m;
                while (bit()) if (++e > 12) return 0;
                m = (1 << e) | (int)gb(e);
                v[i] = v[i] < 0 ? -m : m;
            }
    return !s_over;
}

/* The numbers of every band, into q[w * 128 + k] (long: q[k]).  Within a
 * group of short windows a band's numbers come window by window. */
static int spectral_data(Ics *s, int *q)
{
    int g, b, w0 = 0, i;
    for (i = 0; i < 1024; i++) q[i] = 0;
    for (g = 0; g < s->groups; g++) {
        for (b = 0; b < s->max_sfb; b++) {
            int cb = s->cb[g][b], lo, hi, win, step = cb < 5 ? 4 : 2;
            if (cb == ZERO_HCB || cb >= NOISE_HCB) continue;
            lo = s->ws == EIGHT_SHORT ? C.swb_s[b] : C.swb_l[b];
            hi = s->ws == EIGHT_SHORT ? C.swb_s[b + 1] : C.swb_l[b + 1];
            for (win = 0; win < s->glen[g]; win++) {
                int *dst = q + (s->ws == EIGHT_SHORT ? (w0 + win) * 128 : 0);
                int k, v[4];
                for (k = lo; k < hi; k += step) {
                    if (!spec_word(cb, v)) return 0;
                    for (i = 0; i < step; i++) dst[k + i] = v[i];
                }
            }
        }
        w0 += s->glen[g];
    }
    return 1;
}

/* One channel: everything but the windows, which the caller has read if
 * they are shared. */
static int ics(Ics *s, int common, int ch)
{
    int global = (int)gb(8);
    if (!common && !ics_info(s)) return 0;
    if (!section_data(s) || !scale_factors(s, global)) return 0;
    if (gb(1)) {                         /* pulses: a few numbers made bigger */
        int n = (int)gb(2) + 1, start = (int)gb(6), k, i, off[4], amp[4];
        if (s->ws == EIGHT_SHORT || start >= C.nswb_l) return 0;
        for (i = 0; i < n; i++) { off[i] = (int)gb(5); amp[i] = (int)gb(4); }
        s->tns_on = 0;
        if (gb(1)) { s->tns_on = 1; if (!tns_data(s)) return 0; }
        if (gb(1)) return 0;             /* gain control: AAC SSR only       */
        if (!spectral_data(s, s_q[ch])) return 0;
        k = C.swb_l[start];
        for (i = 0; i < n; i++) {
            k += off[i];
            if (k >= 1024) return 0;
            if (s_q[ch][k] > 0) s_q[ch][k] += amp[i]; else s_q[ch][k] -= amp[i];
        }
        return 1;
    }
    s->tns_on = 0;
    if (gb(1)) { s->tns_on = 1; if (!tns_data(s)) return 0; }
    if (gb(1)) return 0;
    return spectral_data(s, s_q[ch]);
}

/* ---- the numbers into a spectrum ------------------------------------------------------ */
static u32 noise_rand(void)
{
    C.rng = C.rng * 1664525u + 1013904223u;
    return C.rng;
}

/* Quantised numbers times their band's scale; noise bands filled with
 * noise of the band's loudness. */
static void dequantise(const Ics *s, const int *q, float *x)
{
    int g, b, w0 = 0, i;
    for (i = 0; i < 1024; i++) x[i] = 0;
    for (g = 0; g < s->groups; g++) {
        for (b = 0; b < s->max_sfb; b++) {
            int cb = s->cb[g][b], lo, hi, win;
            lo = s->ws == EIGHT_SHORT ? C.swb_s[b] : C.swb_l[b];
            hi = s->ws == EIGHT_SHORT ? C.swb_s[b + 1] : C.swb_l[b + 1];
            for (win = 0; win < s->glen[g]; win++) {
                int base = s->ws == EIGHT_SHORT ? (w0 + win) * 128 : 0, k;
                if (cb == NOISE_HCB) {
                    double e = 0, scale;
                    int sf = s->sf[g][b];
                    for (k = lo; k < hi; k++) {
                        float r = (float)(int)noise_rand();
                        x[base + k] = r;
                        e += (double)r * r;
                    }
                    /* The band's energy is 2^(sf/2): its length 2^(sf/4). */
                    scale = e > 0 ? p2q(sf) / fsqrt(e) : 0;
                    for (k = lo; k < hi; k++) x[base + k] = (float)(x[base + k] * scale);
                } else if (cb != ZERO_HCB && cb < NOISE_HCB) {
                    float sc = s_pow2[s->sf[g][b]];
                    for (k = lo; k < hi; k++) {
                        int v = q[base + k], a;
                        float m;
                        if (!v) continue;        /* most are: x is already 0 */
                        a = v < 0 ? -v : v;
                        m = s_pow43[a > 8207 ? 8207 : a] * sc;
                        x[base + k] = v < 0 ? -m : m;
                    }
                }
            }
        }
        w0 += s->glen[g];
    }
}

/* Mid/side and intensity, for a pair sharing its windows. */
static void stereo(const Ics *l, const Ics *r, float *L, float *R)
{
    int g, b, w0 = 0;
    for (g = 0; g < r->groups; g++) {
        for (b = 0; b < r->max_sfb; b++) {
            int lo, hi, win, rcb = r->cb[g][b], lcb = l->cb[g][b];
            int used = s_ms_present == 2 || (s_ms_present == 1 && s_ms[g][b]);
            lo = r->ws == EIGHT_SHORT ? C.swb_s[b] : C.swb_l[b];
            hi = r->ws == EIGHT_SHORT ? C.swb_s[b + 1] : C.swb_l[b + 1];
            for (win = 0; win < r->glen[g]; win++) {
                int base = r->ws == EIGHT_SHORT ? (w0 + win) * 128 : 0, k;
                if (rcb == INTENSITY_HCB || rcb == INTENSITY_HCB2) {
                    /* The right is the left, scaled by 2^(-pos/4), and
                     * turned over for book 14, and again where mid/side
                     * is marked. */
                    int neg = (rcb == INTENSITY_HCB2) != (s_ms_present == 1 && s_ms[g][b]);
                    double sc = p2q(-r->sf[g][b]);
                    if (neg) sc = -sc;
                    for (k = lo; k < hi; k++) R[base + k] = (float)(L[base + k] * sc);
                } else if (lcb == NOISE_HCB && rcb == NOISE_HCB && used) {
                    /* Noise in both, marked mid/side: the same noise in
                     * both, each at its own loudness. */
                    double sc = p2q(r->sf[g][b] - l->sf[g][b]);
                    for (k = lo; k < hi; k++) R[base + k] = (float)(L[base + k] * sc);
                } else if (used && lcb != NOISE_HCB) {
                    for (k = lo; k < hi; k++) {
                        float m = L[base + k], sd = R[base + k];
                        L[base + k] = m + sd;
                        R[base + k] = m - sd;
                    }
                }
            }
        }
        w0 += r->glen[g];
    }
}

/* Temporal noise shaping: an all-pole filter run along each window's
 * spectrum, over the bands its filters cover. */
static void tns(const Ics *s, float *x)
{
    static const u8 max_long[12]  = { 31, 31, 34, 40, 42, 51, 46, 46, 42, 42, 42, 39 };
    static const u8 max_short[12] = { 9, 9, 10, 14, 14, 14, 14, 14, 14, 14, 14, 14 };
    int w, f, i, m, sh = s->ws == EIGHT_SHORT;
    int nswb = sh ? C.nswb_s : C.nswb_l, maxb = sh ? max_short[C.sri] : max_long[C.sri];
    if (!s->tns_on) return;
    for (w = 0; w < s->nwin; w++) {
        int bottom = nswb, top;
        float *spec = x + (sh ? w * 128 : 0);
        for (f = 0; f < s->nfilt[w]; f++) {
            int order = s->forder[w][f], start, end, size, inc, res = s->coef_res[w] + 3;
            double a[33], b[33], k[32];
            float hist[32];
            top = bottom;
            bottom = top - s->flen[w][f];
            if (bottom < 0) bottom = 0;
            if (!order) continue;
            if (order > (sh ? 7 : 12)) order = sh ? 7 : 12;
            for (i = 0; i < order; i++) {
                int len = res - s->fcomp[w][f], v = s->fcoef[w][f][i];
                double iq;
                if (v & (1 << (len - 1))) v -= 1 << len;
                iq = v >= 0 ? ((1 << (res - 1)) - 0.5) / (PI / 2) : ((1 << (res - 1)) + 0.5) / (PI / 2);
                k[i] = fsin(v / iq);
            }
            a[0] = 1.0;
            for (m = 1; m <= order; m++) {
                for (i = 1; i < m; i++) b[i] = a[i] + k[m - 1] * a[m - i];
                for (i = 1; i < m; i++) a[i] = b[i];
                a[m] = k[m - 1];
            }
            start = bottom < maxb ? bottom : maxb;
            if (start > s->max_sfb) start = s->max_sfb;
            end = top < maxb ? top : maxb;
            if (end > s->max_sfb) end = s->max_sfb;
            start = sh ? C.swb_s[start] : C.swb_l[start];
            end = sh ? C.swb_s[end] : C.swb_l[end];
            size = end - start;
            if (size <= 0) continue;
            inc = 1;
            if (s->fdir[w][f]) { inc = -1; start = end - 1; }
            for (i = 0; i < order; i++) hist[i] = 0;
            for (i = 0; i < size; i++) {
                float *p = spec + start + i * inc, y = *p;
                int j;
                for (j = 0; j < order; j++) y -= (float)a[j + 1] * hist[j];
                for (j = order - 1; j > 0; j--) hist[j] = hist[j - 1];
                hist[0] = y;
                *p = y;
            }
        }
    }
}

/* ---- the filter bank ------------------------------------------------------------------ *
 * One channel's spectrum (or a pair's, added) into C.acc, windowed: the
 * left half of the window follows the last block's shape, the right half
 * this one's.  At half rate everything is half the size. */
static void synth(const float *x, int ws, int shape, int prev)
{
    static float buf[2048];
    int N = C.half ? 1024 : 2048, H = N / 2, Ns = N / 8, Hs = Ns / 2, edge = (H - Hs) / 2, i, w;
    const float *lw = s_win_l[prev], *rw = s_win_l[shape];
    float sc;
    if (ws != EIGHT_SHORT) {
        imdct(x, buf, N);
        sc = 1.0f / 1024.0f;             /* the standard's 2/N, N = 2048     */
        for (i = 0; i < H; i++) {
            float wl;
            if (ws == LONG_STOP) wl = i < edge ? 0.0f : i < edge + Hs ? s_win_s[prev][i - edge] : 1.0f;
            else wl = lw[i];
            buf[i] *= wl * sc;
        }
        for (i = 0; i < H; i++) {
            float wr;
            if (ws == LONG_START) wr = i < edge ? 1.0f : i < edge + Hs ? s_win_s[shape][Hs - 1 - (i - edge)] : 0.0f;
            else wr = rw[H - 1 - i];
            buf[H + i] *= wr * sc;
        }
        for (i = 0; i < N; i++) C.acc[i] += buf[i];
        return;
    }
    sc = 1.0f / 128.0f;                  /* 2/N, N = 256                     */
    for (w = 0; w < 8; w++) {
        const float *ls = s_win_s[w ? shape : prev], *rs = s_win_s[shape];
        float *dst = C.acc + edge + w * Hs;
        imdct(x + w * 128, buf, Ns);
        for (i = 0; i < Hs; i++) {
            dst[i]      += buf[i] * ls[i] * sc;
            dst[Hs + i] += buf[Hs + i] * rs[Hs - 1 - i] * sc;
        }
    }
}

/* ---- elements ---------------------------------------------------------------------- */
static int element_single(void)
{
    int ch = C.nch;
    gb(4);                               /* its tag                          */
    if (!ics(&s_ics[0], 0, 0)) return 0;
    dequantise(&s_ics[0], s_q[0], s_spec[0]);
    tns(&s_ics[0], s_spec[0]);
    if (ch < MAXCH) {
        synth(s_spec[0], s_ics[0].ws, s_ics[0].shape, C.prev_shape[ch]);
        C.prev_shape[ch] = s_ics[0].shape;
    }
    C.nch++;
    return 1;
}

static int element_pair(void)
{
    int common, g, b, ch = C.nch;
    gb(4);
    common = (int)gb(1);
    s_ms_present = 0;
    if (common) {
        if (!ics_info(&s_ics[0])) return 0;
        s_ics[1] = s_ics[0];
        s_ms_present = (int)gb(2);
        if (s_ms_present == 3) return 0;
        if (s_ms_present == 1)
            for (g = 0; g < s_ics[0].groups; g++)
                for (b = 0; b < s_ics[0].max_sfb; b++) s_ms[g][b] = (u8)bit();
    }
    if (!ics(&s_ics[0], common, 0) || !ics(&s_ics[1], common, 1)) return 0;
    dequantise(&s_ics[0], s_q[0], s_spec[0]);
    dequantise(&s_ics[1], s_q[1], s_spec[1]);
    if (common) stereo(&s_ics[0], &s_ics[1], s_spec[0], s_spec[1]);
    tns(&s_ics[0], s_spec[0]);
    tns(&s_ics[1], s_spec[1]);
    if (ch + 1 >= MAXCH) { C.nch += 2; return 1; }
    if (common && C.prev_shape[ch] == C.prev_shape[ch + 1]) {
        /* The same windows: add the spectra, one transform. */
        int i, n = C.nlong;
        if (s_ics[0].ws == EIGHT_SHORT)
            for (i = 0; i < 1024; i++) s_spec[0][i] += s_spec[1][i];
        else
            for (i = 0; i < n; i++) s_spec[0][i] += s_spec[1][i];
        synth(s_spec[0], s_ics[0].ws, s_ics[0].shape, C.prev_shape[ch]);
    } else {
        synth(s_spec[0], s_ics[0].ws, s_ics[0].shape, C.prev_shape[ch]);
        synth(s_spec[1], s_ics[1].ws, s_ics[1].shape, C.prev_shape[ch + 1]);
    }
    C.prev_shape[ch] = s_ics[0].shape;
    C.prev_shape[ch + 1] = s_ics[1].shape;
    C.nch += 2;
    return 1;
}

/* A low-frequency channel: read and left out of the mix. */
static int element_lfe(void)
{
    gb(4);
    return ics(&s_ics[0], 0, 0);
}

static void skip_bits(u32 n) { bits_skip(&s_b, n); }
static void align(void) { bits_align(&s_b); }

static int element_data(void)
{
    int align_flag, count;
    gb(4);
    align_flag = (int)gb(1);
    count = (int)gb(8);
    if (count == 255) count += (int)gb(8);
    if (align_flag) align();
    skip_bits((u32)count * 8);
    return !s_over;
}

static int element_fill(void)
{
    int count = (int)gb(4);
    if (count == 15) count += (int)gb(8) - 1;
    skip_bits((u32)count * 8);           /* SBR lives here: left out         */
    return !s_over;
}

static int element_program(void)
{
    int front, side, back, lfe, assoc, cc, i;
    gb(4); gb(2); gb(4);
    front = (int)gb(4); side = (int)gb(4); back = (int)gb(4);
    lfe = (int)gb(2); assoc = (int)gb(3); cc = (int)gb(4);
    if (gb(1)) gb(4);
    if (gb(1)) gb(4);
    if (gb(1)) gb(3);
    for (i = 0; i < front + side + back; i++) gb(5);
    for (i = 0; i < lfe + assoc; i++) gb(4);
    for (i = 0; i < cc; i++) gb(5);
    align();
    skip_bits(gb(8) * 8);
    return !s_over;
}

/* One raw block: elements until the end mark.  1 if it decoded. */
static int raw_block(void)
{
    int i;
    C.nch = 0;
    for (i = 0; i < 2048; i++) C.acc[i] = 0;
    for (;;) {
        int id = (int)gb(3), ok;
        if (s_over) return 0;
        switch (id) {
        case 0: ok = element_single(); break;
        case 1: ok = element_pair(); break;
        case 3: ok = element_lfe(); break;
        case 4: ok = element_data(); break;
        case 5: ok = element_program(); break;
        case 6: ok = element_fill(); break;
        case 7: align(); return C.nch > 0;
        default: return 0;               /* a coupling channel: not here      */
        }
        if (!ok) return 0;
    }
}

/* The block's samples: the last block's tail, plus this one's head. */
static void emit(void)
{
    int H = C.half ? 512 : 1024, i;
    float g = 32767.0f / 32768.0f * AUDIO_HEADROOM / (float)(C.nch ? C.nch : 1);
    for (i = 0; i < H; i++) {
        int o = dsp_round(C.ov[i] + C.acc[i] * g);
        C.ov[i] = C.acc[H + i] * g;
        C.out[C.out_n++] = (s16)(o > 32767 ? 32767 : o < -32768 ? -32768 : o);
    }
}

/* ---- ADTS ------------------------------------------------------------------------------ */
static const int RATES[12] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000 };

/* A header that could be one, for this stream if it has started. */
static int header_ok(const u8 *p)
{
    int sri = (p[2] >> 2) & 15, len = ((p[3] & 3) << 11) | (p[4] << 3) | (p[5] >> 5);
    if (p[0] != 0xFF || (p[1] & 0xF6) != 0xF0) return 0;
    if (sri >= 12 || ((p[2] >> 6) & 3) != 1) return 0;      /* LC only        */
    if (len < 7 + ((p[1] & 1) ? 0 : 2)) return 0;
    if (C.rate && sri != C.sri) return 0;
    return len;
}

/* Decode one frame into C.out.  1 done, 2 skipped (damaged), or
 * AUDIO_MORE / AUDIO_END. */
static int frame(AudioIn *in)
{
    for (;;) {
        u32 left = in->len - in->pos;
        const u8 *p = in->buf + in->pos;
        int len, nblocks, prot, b, good = 0;
        if (left < 7) return in->done ? AUDIO_END : AUDIO_MORE;
        if (!(len = header_ok(p))) { in->pos++; C.synced = 0; continue; }
        if (left < (u32)len) return in->done ? AUDIO_END : AUDIO_MORE;
        /* Found by searching: believe it only if the next frame starts
         * where this one says it ends. */
        if (!C.synced) {
            if (left < (u32)len + 7) { if (!in->done) return AUDIO_MORE; }
            else if (!header_ok(p + len)) { in->pos++; continue; }
            C.synced = 1;
        }
        prot = !(p[1] & 1);
        nblocks = (p[6] & 3) + 1;
        memcpy(s_frame, p, (u32)len);
        s_frame[len] = s_frame[len + 1] = s_frame[len + 2] = s_frame[len + 3] = 0;
        bits_init(&s_b, s_frame, (u32)len);
        bits_skip(&s_b, 56 + (prot ? 16 * nblocks : 0));   /* positions and the check */
        C.out_n = C.out_at = 0;
        for (b = 0; b < nblocks; b++) {
            if (raw_block()) { emit(); good = 1; }
            else break;
            if (prot && nblocks > 1) skip_bits(16);
        }
        in->pos += (u32)len;
        return good ? 1 : 2;
    }
}

static int aac_decode(Decoder *d, AudioIn *in, s16 *out, int max)
{
    int n = 0;
    (void)d;
    while (n < max) {
        int r;
        if (C.out_at < C.out_n) {
            while (n < max && C.out_at < C.out_n) out[n++] = C.out[C.out_at++];
            continue;
        }
        r = frame(in);
        if (r == AUDIO_MORE || r == AUDIO_END) return n ? n : r;
    }
    return n;
}

/* Ready once a frame header has been found and checked against the next. */
int aac_open(AudioIn *in, Decoder *d)
{
    u32 at = in->pos;
    int i;

    tables();
    /* An ID3 tag in front, as some .aac files have. */
    if (in->len - at >= 10 && in->buf[at] == 'I' && in->buf[at + 1] == 'D' && in->buf[at + 2] == '3') {
        u32 size = ((u32)(in->buf[at + 6] & 0x7F) << 21) | ((u32)(in->buf[at + 7] & 0x7F) << 14) |
                   ((u32)(in->buf[at + 8] & 0x7F) << 7) | (u32)(in->buf[at + 9] & 0x7F);
        at += 10 + size;
    }
    C.rate = 0;
    for (;;) {
        const u8 *p;
        int len;
        if (at + 7 > in->len) return in->done ? 0 : AUDIO_MORE;
        p = in->buf + at;
        if ((len = header_ok(p)) != 0) {
            if (at + (u32)len + 7 > in->len) {
                if (!in->done) return at - in->pos > 65536 ? 0 : AUDIO_MORE;
            } else if (!header_ok(p + len)) { at++; continue; }
            C.sri = (p[2] >> 2) & 15;
            C.cfg = ((p[2] & 1) << 2) | (p[3] >> 6);
            C.rate = RATES[C.sri];
            C.half = C.rate >= 32000;
            d->kbps = len * 8 * (C.rate / 100) / 10240;
            break;
        }
        if (++at - in->pos > 65536) return 0;
    }
    in->pos = at;
    C.swb_l = AAC_SWB_LONG[C.sri]; C.nswb_l = AAC_NSWB_LONG[C.sri];
    C.swb_s = AAC_SWB_SHORT[C.sri]; C.nswb_s = AAC_NSWB_SHORT[C.sri];
    C.nlong = C.half ? 512 : 1024;
    C.nshort = C.half ? 64 : 128;
    if (s_win_n != (C.half ? 1024 : 2048)) {
        s_win_n = C.half ? 1024 : 2048;
        make_window(s_win_l[0], s_win_n, 0, 0);
        make_window(s_win_l[1], s_win_n, 1, 4.0);
        make_window(s_win_s[0], s_win_n / 8, 0, 0);
        make_window(s_win_s[1], s_win_n / 8, 1, 6.0);
    }
    for (i = 0; i < MAXCH; i++) C.prev_shape[i] = 0;
    for (i = 0; i < 1024; i++) C.ov[i] = 0;
    C.out_n = C.out_at = 0;
    C.synced = 1;
    C.rng = 0x1F2E3D4Cu;
    d->kind = "AAC";
    d->rate = C.half ? C.rate / 2 : C.rate;
    d->channels = C.cfg ? C.cfg : 2;
    d->decode = aac_decode;
    return 1;
}
