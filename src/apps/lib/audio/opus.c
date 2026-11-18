/* ===========================================================================
 * opus.c -- Opus (RFC 6716), its music layer CELT, into mono samples.
 *
 * An Opus packet starts with a table-of-contents byte: which layer (SILK
 * for speech, CELT for music, or both, "hybrid"), how wide, how long a
 * frame (2.5 to 20 ms), mono or stereo, and how many frames follow.  This
 * reads CELT, which is what music stations send; SILK and hybrid frames
 * come out as silence (a speech station is refused at the start).
 *
 * A CELT frame is one range-coded bitstream (section 4.1) holding, in order:
 *
 *   flags (silence, the pitch post-filter's period and gain, short blocks
 *   for a transient, intra energy), then each of 21 bands' energy, coarse
 *   (Laplace-coded differences from the last frame and the band below);
 *   per band whether time or frequency resolution is changed; how much the
 *   spectrum is spread; boosts; a trim; then the bit allocation, which is
 *   not sent but worked out from all that, exactly as the encoder did
 *   (allocate() -- one integer slip and every bit after is misread);
 *
 *   fine energy, then each band's shape: a unit vector of N numbers coded
 *   as K "pulses" (pyramid vector quantisation), the vector split in halves
 *   with an angle between them when K would be too large, stereo as an
 *   angle between mid and side, and bands with no bits filled by folding
 *   in a lower band's shape;
 *
 * then the energies give the shapes their size, an inverse MDCT turns the
 * spectrum into time (one long block, or 2-8 short ones), the comb filter
 * puts back the pitch, and a de-emphasis filter the low end.
 *
 * Mono: a stereo frame's two spectra are added before the transform (it is
 * linear), so there is one transform.  With half set it works at 24 kHz,
 * as the other decoders do: the lower half of the spectrum, half-size
 * transforms, the post-filter and de-emphasis made for the lower rate.
 *
 * FLOPOS's own code, written from RFC 6716; the numbers the format fixes
 * (bands, allocation, probabilities, the pulse cache) are in gen/opustab.h,
 * from libopus.  Checked against libopus (GStreamer's opusdec) by
 * test/audiotest/codecs.py.
 * ===========================================================================
 */
#include "decode.h"
#include "gen/opustab.h"
#include "dsp.h"

void *memcpy(void *d, const void *s, u32 n);

/* ---- maths without a library -------------------------------------------------- */
static double fsin(double x) { double r; __asm__("fsin" : "=t"(r) : "0"(x)); return r; }
static double fcos(double x) { double r; __asm__("fcos" : "=t"(r) : "0"(x)); return r; }
static double fsqrt(double x) { double r; __asm__("fsqrt" : "=t"(r) : "0"(x)); return r; }
#define PI 3.14159265358979323846

/* 2^x: the whole part in the float's exponent, the rest by its series. */
static float fexp2(float x)
{
    union { float f; u32 i; } u;
    int n;
    double f, s = 1.0, t = 1.0;
    int k;
    if (x < -120.0f) return 0.0f;
    if (x > 120.0f) x = 120.0f;
    n = (int)(x + 1024.0f) - 1024;               /* floor */
    f = (x - (float)n) * 0.69314718055994530942;
    for (k = 1; k < 12; k++) { t *= f / k; s += t; }
    u.f = (float)s;
    u.i += (u32)n << 23;
    return u.f;
}

static int ilog(u32 x) { int n = 0; while (x) { n++; x >>= 1; } return n; }   /* bits in x */
static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }

static u32 isqrt(u32 v)                          /* floor(sqrt(v)) */
{
    u32 g = 0, b;
    int sh = (ilog(v) - 1) >> 1;
    if (!v) return 0;
    b = 1u << sh;
    do {
        u32 t = ((g << 1) + b) << sh;
        if (t <= v) { g += b; v -= t; }
        b >>= 1; sh--;
    } while (sh >= 0);
    return g;
}

/* The integer cosine and log-tangent the angle splits use: part of the
 * format, so not floating point (RFC 6716 4.3.4.3). */
static int frac_mul16(int a, int b) { return (16384 + (s32)(short)a * (short)b) >> 15; }
static int bcos(int x)
{
    int x2 = (4096 + x * x) >> 13;
    x2 = (32767 - x2) + frac_mul16(x2, -7651 + frac_mul16(x2, 8277 + frac_mul16(-626, x2)));
    return 1 + x2;
}
static int blog2tan(int isin, int icos)
{
    int lc = ilog((u32)icos), ls = ilog((u32)isin);
    icos <<= 15 - lc;
    isin <<= 15 - ls;
    return (ls - lc) * (1 << 11) + frac_mul16(isin, frac_mul16(isin, -2597) + 7932)
         - frac_mul16(icos, frac_mul16(icos, -2597) + 7932);
}

/* ---- the range decoder (RFC 6716 4.1) ------------------------------------------ */
typedef struct {
    const u8 *buf;
    u32 len, offs, end_offs;
    u32 rng, val, rem;
    u32 end_win;
    int end_bits, total_bits;
} Rd;

static u32 rd_byte(Rd *d) { return d->offs < d->len ? d->buf[d->offs++] : 0; }
static u32 rd_byte_end(Rd *d) { return d->end_offs < d->len ? d->buf[d->len - ++d->end_offs] : 0; }

static void rd_norm(Rd *d)
{
    while (d->rng <= (1u << 23)) {
        u32 sym;
        d->total_bits += 8;
        d->rng <<= 8;
        sym = d->rem;
        d->rem = rd_byte(d);
        sym = (sym << 8 | d->rem) >> 1;
        d->val = ((d->val << 8) + (255 & ~sym)) & 0x7FFFFFFFu;
    }
}

static void rd_init(Rd *d, const u8 *p, u32 n)
{
    d->buf = p; d->len = n; d->offs = d->end_offs = 0;
    d->end_win = 0; d->end_bits = 0;
    d->total_bits = 9;
    d->rng = 128;
    d->rem = rd_byte(d);
    d->val = d->rng - 1 - (d->rem >> 1);
    rd_norm(d);
}

static u32 s_ext;                                /* rng / ft, from the last decode */
static u32 rd_decode(Rd *d, u32 ft)
{
    u32 s;
    s_ext = d->rng / ft;
    s = d->val / s_ext;
    return ft - (s + 1 < ft ? s + 1 : ft);
}
static u32 rd_decode_bin(Rd *d, int bits)
{
    u32 s;
    s_ext = d->rng >> bits;
    s = d->val / s_ext;
    return (1u << bits) - (s + 1 < (1u << bits) ? s + 1 : (1u << bits));
}
static void rd_update(Rd *d, u32 fl, u32 fh, u32 ft)
{
    u32 s = s_ext * (ft - fh);
    d->val -= s;
    d->rng = fl > 0 ? s_ext * (fh - fl) : d->rng - s;
    rd_norm(d);
}
static int rd_bit(Rd *d, int logp)
{
    u32 r = d->rng, v = d->val, s = r >> logp;
    int bit = v < s;
    if (!bit) d->val = v - s;
    d->rng = bit ? s : r - s;
    rd_norm(d);
    return bit;
}
static int rd_icdf(Rd *d, const u8 *icdf, int ftb)
{
    u32 s = d->rng, v = d->val, r = s >> ftb, t;
    int k = -1;
    do { t = s; s = r * icdf[++k]; } while (v < s);
    d->val = v - s;
    d->rng = t - s;
    rd_norm(d);
    return k;
}
static u32 rd_bits(Rd *d, int bits)              /* raw bits, from the end of the frame */
{
    u32 w = d->end_win, v;
    int have = d->end_bits;
    if (have < bits) {
        do { w |= rd_byte_end(d) << have; have += 8; } while (have <= 24);
    }
    v = w & ((1u << bits) - 1);
    d->end_win = w >> bits;
    d->end_bits = have - bits;
    d->total_bits += bits;
    return v;
}
static u32 rd_uint(Rd *d, u32 ft)
{
    int ftb;
    ft--;
    ftb = ilog(ft);
    if (ftb > 8) {
        u32 ft1, s, t;
        ftb -= 8;
        ft1 = (ft >> ftb) + 1;
        s = rd_decode(d, ft1);
        rd_update(d, s, s + 1, ft1);
        t = s << ftb | rd_bits(d, ftb);
        return t <= ft ? t : ft;
    }
    ft++;
    {
        u32 s = rd_decode(d, ft);
        rd_update(d, s, s + 1, ft);
        return s;
    }
}
static int rd_tell(Rd *d) { return d->total_bits - ilog(d->rng); }
static u32 rd_tell_frac(Rd *d)                   /* in 1/8 bits */
{
    u32 nbits = (u32)d->total_bits << 3, r;
    int l = ilog(d->rng), i;
    r = d->rng >> (l - 16);
    for (i = 3; i-- > 0;) {
        int b;
        r = r * r >> 15;
        b = (int)(r >> 16);
        l = l << 1 | b;
        r >>= b;
    }
    return nbits - (u32)l;
}

/* Laplace-distributed energy steps (4.3.2.1): fs the chance of 0 (in
 * 1/32768), decay how quickly the chances fall either side. */
static int laplace(Rd *d, u32 fs, int decay)
{
    int val = 0;
    u32 fl = 0, fm = rd_decode_bin(d, 15);
    if (fm >= fs) {
        val++;
        fl = fs;
        fs = ((32768 - 32 - fs) * (u32)(16384 - decay) >> 15) + 1;
        while (fs > 1 && fm >= fl + 2 * fs) {
            fs *= 2;
            fl += fs;
            fs = ((fs - 2) * (u32)decay >> 15) + 1;
            val++;
        }
        if (fs <= 1) {
            int di = (int)((fm - fl) >> 1);
            val += di;
            fl += 2 * (u32)di;
        }
        if (fm < fl + fs) val = -val;
        else fl += fs;
    }
    rd_update(d, fl, fl + fs < 32768 ? fl + fs : 32768, 32768);
    return val;
}

/* ---- the stream -------------------------------------------------------------- */
#define NB 21                                    /* bands                          */
#define MAXN 960                                 /* coefficients a frame (20 ms)   */
#define OV 120                                   /* the windows' overlap at 48 kHz */
#define OUTN (OPUS_HALF ? MAXN / 2 : MAXN)       /* samples a frame, at the rate in use */
#define HIST (OPUS_HALF ? 520 : 1030)            /* the post-filter's reach back (1024 + 2 at 48 kHz) */

static struct {
    int half;                                    /* working at 24 kHz              */
    float oldE[2 * NB], oldL[2 * NB], oldL2[2 * NB];
    u32 seed;
    int pf_t, pf_t_old, pf_set, pf_set_old;      /* the post-filter: period, tap set */
    float pf_g, pf_g_old;
    float syn[HIST + OUTN];                      /* past output, then this frame's */
    float tail[OV];                              /* the last block's falling edge  */
    float de_m, de_x;                            /* de-emphasis' memory            */
    float window[OV];                            /* the rising edge, at the rate in use */
    int ov;                                      /* its length                     */
} S;

/* ---- energy (4.3.2) ----------------------------------------------------------- */
static void coarse_energy(Rd *d, int end, int intra, int C, int LM, int budget)
{
    static const float pred[4] = { 29440 / 32768.f, 26112 / 32768.f, 21248 / 32768.f, 16384 / 32768.f };
    static const float beta_inter[4] = { 30147 / 32768.f, 22282 / 32768.f, 12124 / 32768.f, 6554 / 32768.f };
    static const u8 small_icdf[3] = { 2, 1, 0 };
    const u8 *model = OPUS_EPROB + (LM * 2 + intra) * 42;
    float prev[2] = { 0, 0 }, coef = intra ? 0 : pred[LM], beta = intra ? 4915 / 32768.f : beta_inter[LM];
    int i, c;
    for (i = 0; i < end; i++)
        for (c = 0; c < C; c++) {
            int qi, tell = rd_tell(d);
            float q, *e = &S.oldE[i + c * NB];
            if (budget - tell >= 15) {
                int pi = 2 * imin(i, 20);
                qi = laplace(d, (u32)model[pi] << 7, model[pi + 1] << 6);
            } else if (budget - tell >= 2) {
                qi = rd_icdf(d, small_icdf, 2);
                qi = (qi >> 1) ^ -(qi & 1);
            } else if (budget - tell >= 1) qi = -rd_bit(d, 1);
            else qi = -1;
            q = (float)qi;
            if (*e < -9.0f) *e = -9.0f;
            *e = coef * *e + prev[c] + q;
            prev[c] = prev[c] + q - beta * q;
        }
}

/* ---- the bit allocation (4.3.3) ----------------------------------------------- */
#define BITRES 3
static int bits2pulses(int band, int LM, int bits)
{
    const u8 *cache = OPUS_CACHE_BITS + OPUS_CACHE_INDEX[(LM + 1) * NB + band];
    int lo = 0, hi = cache[0], i;
    bits--;
    for (i = 0; i < 6; i++) {
        int mid = (lo + hi + 1) >> 1;
        if ((int)cache[mid] >= bits) hi = mid; else lo = mid;
    }
    return bits - (lo == 0 ? -1 : (int)cache[lo]) <= (int)cache[hi] - bits ? lo : hi;
}
static int pulses2bits(int band, int LM, int q)
{
    const u8 *cache = OPUS_CACHE_BITS + OPUS_CACHE_INDEX[(LM + 1) * NB + band];
    return q == 0 ? 0 : cache[q] + 1;
}
static int get_pulses(int i) { return i < 8 ? i : (8 + (i & 7)) << ((i >> 3) - 1); }
static int width(int i) { return OPUS_EBANDS[i + 1] - OPUS_EBANDS[i]; }

typedef struct {
    int pulses[NB], ebits[NB], prio[NB], cap[NB], offsets[NB];
    int coded, intensity, dual, balance;
} Alloc;

static void allocate(Rd *d, Alloc *a, int end, int trim, int total, int C, int LM)
{
    int bits1[NB], bits2[NB], thresh[NB], trim_off[NB];
    int lo, hi, j, skip_start = 0, skip_rsv, intensity_rsv = 0, dual_rsv = 0, psum, done, mid, i;
    int alloc_floor = C << BITRES, logM = LM << BITRES, coded, left, percoeff, balance;
    int *bits = a->pulses;

    if (total < 0) total = 0;
    skip_rsv = total >= 1 << BITRES ? 1 << BITRES : 0;
    total -= skip_rsv;
    if (C == 2) {
        intensity_rsv = OPUS_LOG2_FRAC[end];
        if (intensity_rsv > total) intensity_rsv = 0;
        else {
            total -= intensity_rsv;
            dual_rsv = total >= 1 << BITRES ? 1 << BITRES : 0;
            total -= dual_rsv;
        }
    }
    for (j = 0; j < end; j++) {
        thresh[j] = imax(C << BITRES, (3 * width(j) << LM << BITRES) >> 4);
        trim_off[j] = C * width(j) * (trim - 5 - LM) * (end - j - 1) * (1 << (LM + BITRES)) >> 6;
        if (width(j) << LM == 1) trim_off[j] -= C << BITRES;
    }
    /* The quality level: the best whose sum fits. */
    lo = 1; hi = 10;
    do {
        psum = 0; done = 0;
        mid = (lo + hi) >> 1;
        for (j = end; j-- > 0;) {
            int b = C * width(j) * OPUS_ALLOC[mid * NB + j] << LM >> 2;
            if (b > 0) b = imax(0, b + trim_off[j]);
            b += a->offsets[j];
            if (b >= thresh[j] || done) { done = 1; psum += imin(b, a->cap[j]); }
            else if (b >= C << BITRES) psum += C << BITRES;
        }
        if (psum > total) hi = mid - 1; else lo = mid + 1;
    } while (lo <= hi);
    hi = lo--;
    for (j = 0; j < end; j++) {
        int b1 = C * width(j) * OPUS_ALLOC[lo * NB + j] << LM >> 2;
        int b2 = hi >= 11 ? a->cap[j] : C * width(j) * OPUS_ALLOC[hi * NB + j] << LM >> 2;
        if (b1 > 0) b1 = imax(0, b1 + trim_off[j]);
        if (b2 > 0) b2 = imax(0, b2 + trim_off[j]);
        if (lo > 0) b1 += a->offsets[j];
        b2 += a->offsets[j];
        if (a->offsets[j] > 0) skip_start = j;
        bits1[j] = b1;
        bits2[j] = imax(0, b2 - b1);
    }

    /* Between the two levels, in 64 steps. */
    lo = 0; hi = 64;
    for (i = 0; i < 6; i++) {
        mid = (lo + hi) >> 1;
        psum = 0; done = 0;
        for (j = end; j-- > 0;) {
            int t = bits1[j] + (mid * bits2[j] >> 6);
            if (t >= thresh[j] || done) { done = 1; psum += imin(t, a->cap[j]); }
            else if (t >= alloc_floor) psum += alloc_floor;
        }
        if (psum > total) hi = mid; else lo = mid;
    }
    psum = 0; done = 0;
    for (j = end; j-- > 0;) {
        int t = bits1[j] + (lo * bits2[j] >> 6);
        if (t < thresh[j] && !done) t = t >= alloc_floor ? alloc_floor : 0;
        else done = 1;
        t = imin(t, a->cap[j]);
        bits[j] = t;
        psum += t;
    }

    /* Bands at the top are skipped, one flag each, while that frees bits. */
    for (coded = end;; coded--) {
        int bw, bb, rem;
        j = coded - 1;
        if (j <= skip_start) { total += skip_rsv; break; }
        left = total - psum;
        percoeff = (int)((u32)left / (u32)(OPUS_EBANDS[coded] - OPUS_EBANDS[0]));
        left -= (OPUS_EBANDS[coded] - OPUS_EBANDS[0]) * percoeff;
        rem = imax(left - (OPUS_EBANDS[j] - OPUS_EBANDS[0]), 0);
        bw = OPUS_EBANDS[coded] - OPUS_EBANDS[j];
        bb = bits[j] + percoeff * bw + rem;
        if (bb >= imax(thresh[j], alloc_floor + (1 << BITRES))) {
            if (rd_bit(d, 1)) break;
            psum += 1 << BITRES;
            bb -= 1 << BITRES;
        }
        psum -= bits[j] + intensity_rsv;
        if (intensity_rsv > 0) intensity_rsv = OPUS_LOG2_FRAC[j];
        psum += intensity_rsv;
        if (bb >= alloc_floor) { psum += alloc_floor; bits[j] = alloc_floor; }
        else bits[j] = 0;
    }
    a->intensity = intensity_rsv > 0 ? (int)rd_uint(d, (u32)coded + 1) : 0;
    if (a->intensity <= 0) { total += dual_rsv; dual_rsv = 0; }
    a->dual = dual_rsv > 0 ? rd_bit(d, 1) : 0;

    left = total - psum;
    percoeff = (int)((u32)left / (u32)(OPUS_EBANDS[coded] - OPUS_EBANDS[0]));
    left -= (OPUS_EBANDS[coded] - OPUS_EBANDS[0]) * percoeff;
    for (j = 0; j < coded; j++) bits[j] += percoeff * width(j);
    for (j = 0; j < coded; j++) { int t = imin(left, width(j)); bits[j] += t; left -= t; }

    /* Fine energy's share, and what is left for the shapes. */
    balance = 0;
    for (j = 0; j < coded; j++) {
        int N = width(j) << LM, bit = bits[j] + balance, excess;
        if (N > 1) {
            int den, nclogn, offset;
            excess = imax(bit - a->cap[j], 0);
            bits[j] = bit - excess;
            den = C * N + (C == 2 && N > 2 && !a->dual && j < a->intensity ? 1 : 0);
            nclogn = den * (OPUS_LOGN[j] + logM);
            offset = (nclogn >> 1) - den * 21;
            if (N == 2) offset += den << BITRES >> 2;
            if (bits[j] + offset < den * 2 << BITRES) offset += nclogn >> 2;
            else if (bits[j] + offset < den * 3 << BITRES) offset += nclogn >> 3;
            a->ebits[j] = imax(0, bits[j] + offset + (den << (BITRES - 1)));
            a->ebits[j] = (int)((u32)a->ebits[j] / (u32)den) >> BITRES;
            if (C * a->ebits[j] > (bits[j] >> BITRES)) a->ebits[j] = bits[j] >> (C - 1) >> BITRES;
            a->ebits[j] = imin(a->ebits[j], 8);
            a->prio[j] = a->ebits[j] * (den << BITRES) >= bits[j] + offset;
            bits[j] -= C * a->ebits[j] << BITRES;
        } else {
            excess = imax(0, bit - (C << BITRES));
            bits[j] = bit - excess;
            a->ebits[j] = 0;
            a->prio[j] = 1;
        }
        if (excess > 0) {
            int extra = imin(excess >> (C - 1 + BITRES), 8 - a->ebits[j]);
            a->ebits[j] += extra;
            a->prio[j] = extra * C << BITRES >= excess - balance;
            excess -= extra * C << BITRES;
        }
        balance = excess;
    }
    a->balance = balance;
    for (; j < end; j++) {
        a->ebits[j] = bits[j] >> (C - 1) >> BITRES;
        bits[j] = 0;
        a->prio[j] = a->ebits[j] < 1;
    }
    a->coded = coded;
}

/* ---- shapes (4.3.4) ------------------------------------------------------------- */
#define SPREAD_NONE 0
#define SPREAD_AGGRESSIVE 3

static u32 lcg(u32 s) { return 1664525u * s + 1013904223u; }

static void renormalise(float *x, int n, float gain)
{
    float e = 1e-15f;
    int i;
    for (i = 0; i < n; i++) e += x[i] * x[i];
    gain /= (float)fsqrt(e);
    for (i = 0; i < n; i++) x[i] *= gain;
}

/* The pulses' index into a vector (4.3.4.2).  U(n, k) -- vectors of n
 * numbers with k pulses whose first is positive, roughly -- is worked out
 * a row at a time instead of kept in a 5 KB table: u[k] holds U(n, k) for
 * the n in hand, and steps down a row as n does. */
static u32 s_u[260];
static int pulse_vector(Rd *d, int *y, int n, int k)
{
    u32 *u = s_u, i, p, q;
    int m, j, s, k0, val, yy = 0;
    if (k + 2 > (int)(sizeof s_u / sizeof *s_u)) { for (j = 0; j < n; j++) y[j] = 0; return 0; }
    /* U(2, j) = 2j - 1, then up to n: U(m+1, j) = U(m, j) + U(m+1, j-1) + U(m, j-1). */
    u[0] = 0;
    for (j = 1; j <= k + 1; j++) u[j] = 2u * (u32)j - 1;
    for (m = 2; m < n; m++) {
        u32 old_prev = u[0];
        u[0] = 0;
        for (j = 1; j <= k + 1; j++) { u32 old = u[j]; u[j] = old + u[j - 1] + old_prev; old_prev = old; }
    }
    i = rd_uint(d, u[k] + u[k + 1]);
    while (n > 2) {
        if (k >= n) {
            p = u[k + 1];
            s = -(i >= p);
            i -= p & (u32)s;
            k0 = k;
            q = u[n];
            if (q > i) { k = n; do p = u[--k]; while (p > i); }
            else for (p = u[k]; p > i; p = u[k]) k--;
            i -= p;
            val = (k0 - k + s) ^ s;
        } else {
            p = u[k];
            q = u[k + 1];
            if (p <= i && i < q) { i -= p; val = 0; }
            else {
                s = -(i >= q);
                i -= q & (u32)s;
                k0 = k;
                do p = u[--k]; while (p > i);
                i -= p;
                val = (k0 - k + s) ^ s;
            }
        }
        *y++ = val;
        yy += val * val;
        /* down a row: U(n-1, j) = U(n, j) - U(n, j-1) - U(n-1, j-1) */
        n--;
        {
            u32 prev_new = n == 0 ? 1 : 0;
            u32 prev_old = u[0];
            u[0] = prev_new;
            for (j = 1; j <= k + 1; j++) {
                u32 old = u[j];
                u[j] = old - prev_old - prev_new;
                prev_old = old;
                prev_new = u[j];
            }
        }
    }
    /* the last two numbers */
    p = 2u * (u32)k + 1;
    s = -(i >= p);
    i -= p & (u32)s;
    k0 = k;
    k = (int)((i + 1) >> 1);
    if (k) i -= 2u * (u32)k - 1;
    val = (k0 - k + s) ^ s;
    *y++ = val;
    yy += val * val;
    s = -(int)i;
    val = (k + s) ^ s;
    *y = val;
    yy += val * val;
    return yy;
}

static void rotate1(float *x, int len, int stride, float c, float s)
{
    int i;
    float *p = x;
    for (i = 0; i < len - stride; i++, p++) {
        float x1 = p[0], x2 = p[stride];
        p[stride] = c * x2 + s * x1;
        p[0] = c * x1 - s * x2;
    }
    p = x + len - 2 * stride - 1;
    for (i = len - 2 * stride - 1; i >= 0; i--, p--) {
        float x1 = p[0], x2 = p[stride];
        p[stride] = c * x2 + s * x1;
        p[0] = c * x1 - s * x2;
    }
}

/* Spreading: undoes the rotation that spread the pulses' energy. */
static void unspread(float *x, int len, int stride, int k, int spread)
{
    static const int factor[3] = { 15, 10, 5 };
    float gain, theta, c, s;
    int i, stride2 = 0;
    if (2 * k >= len || spread == SPREAD_NONE) return;
    gain = (float)len / (float)(len + factor[spread - 1] * k);
    theta = 0.5f * gain * gain;
    c = (float)fcos(0.5 * PI * theta);
    s = (float)fcos(0.5 * PI * (1.0f - theta));
    if (len >= 8 * stride) {
        stride2 = 1;
        while ((stride2 * stride2 + stride2) * stride + (stride >> 2) < len) stride2++;
    }
    len = (int)((u32)len / (u32)stride);
    for (i = 0; i < stride; i++) {
        if (stride2) rotate1(x + i * len, len, stride2, s, c);
        rotate1(x + i * len, len, 1, c, s);
    }
}

static unsigned pvq(Rd *d, float *x, int n, int k, int spread, int B, float gain)
{
    static int iy[176];
    int yy = pulse_vector(d, iy, n, k), i;
    unsigned mask = 0;
    float g = gain / (float)fsqrt((double)yy);
    for (i = 0; i < n; i++) x[i] = g * (float)iy[i];
    unspread(x, n, B, k, spread);
    if (B <= 1) return 1;
    {
        int n0 = (int)((u32)n / (u32)B), b, j;
        for (b = 0; b < B; b++) {
            int any = 0;
            for (j = 0; j < n0; j++) any |= iy[b * n0 + j];
            mask |= (unsigned)(any != 0) << b;
        }
    }
    return mask;
}

static void haar1(float *x, int n0, int stride)
{
    int i, j;
    n0 >>= 1;
    for (i = 0; i < stride; i++)
        for (j = 0; j < n0; j++) {
            float a = 0.70710678f * x[stride * 2 * j + i], b = 0.70710678f * x[stride * (2 * j + 1) + i];
            x[stride * 2 * j + i] = a + b;
            x[stride * (2 * j + 1) + i] = a - b;
        }
}

static const int ORDERY[30] = { 1, 0, 3, 0, 2, 1, 7, 0, 4, 3, 6, 1, 5, 2,
                                15, 0, 8, 7, 12, 3, 11, 4, 14, 1, 9, 6, 13, 2, 10, 5 };
static float s_tmp[176];                         /* a band: 22 x 8 at most          */
static void deinterleave(float *x, int n0, int stride, int hadamard)
{
    int i, j, n = n0 * stride;
    const int *ord = ORDERY + stride - 2;
    for (i = 0; i < stride; i++)
        for (j = 0; j < n0; j++) s_tmp[(hadamard ? ord[i] : i) * n0 + j] = x[j * stride + i];
    for (i = 0; i < n; i++) x[i] = s_tmp[i];
}
static void interleave(float *x, int n0, int stride, int hadamard)
{
    int i, j, n = n0 * stride;
    const int *ord = ORDERY + stride - 2;
    for (i = 0; i < stride; i++)
        for (j = 0; j < n0; j++) s_tmp[j * stride + i] = x[(hadamard ? ord[i] : i) * n0 + j];
    for (i = 0; i < n; i++) x[i] = s_tmp[i];
}

/* What one band's decoding needs to know. */
typedef struct {
    Rd *d;
    int i, intensity, spread, tf_change, remaining, avoid_split_noise;
    u32 seed;
} Band;

static int compute_qn(int N, int b, int offset, int pulse_cap, int stereo)
{
    static const short exp2_8[8] = { 16384, 17866, 19483, 21247, 23170, 25267, 27554, 30048 };
    int qn, qb, N2 = 2 * N - 1;
    if (stereo && N == 2) N2--;
    qb = (b + N2 * offset) / N2;                 /* rounds towards zero, as C does */
    qb = imin(b - pulse_cap - (4 << BITRES), qb);
    qb = imin(8 << BITRES, qb);
    if (qb < (1 << BITRES >> 1)) return 1;
    qn = exp2_8[qb & 7] >> (14 - (qb >> BITRES));
    return (qn + 1) >> 1 << 1;
}

typedef struct { int inv, imid, iside, delta, itheta, qalloc; } Split;

/* The angle between two halves, or between mid and side (4.3.4.3). */
static void theta(Band *bd, Split *sp, int N, int *b, int B, int B0, int LM, int stereo, int *fill)
{
    Rd *d = bd->d;
    int pulse_cap = OPUS_LOGN[bd->i] + LM * (1 << BITRES);
    int offset = (pulse_cap >> 1) - (stereo && N == 2 ? 16 : 4);
    int qn = compute_qn(N, *b, offset, pulse_cap, stereo), itheta = 0, inv = 0;
    u32 tell;
    if (stereo && bd->i >= bd->intensity) qn = 1;
    tell = rd_tell_frac(d);
    if (qn != 1) {
        if (stereo && N > 2) {
            int p0 = 3, x0 = qn / 2, ft = p0 * (x0 + 1) + x0, fs, x;
            fs = (int)rd_decode(d, (u32)ft);
            x = fs < (x0 + 1) * p0 ? fs / p0 : x0 + 1 + (fs - (x0 + 1) * p0);
            rd_update(d, (u32)(x <= x0 ? p0 * x : (x - 1 - x0) + (x0 + 1) * p0),
                         (u32)(x <= x0 ? p0 * (x + 1) : (x - x0) + (x0 + 1) * p0), (u32)ft);
            itheta = x;
        } else if (B0 > 1 || stereo) {
            itheta = (int)rd_uint(d, (u32)qn + 1);
        } else {
            int fs, fl, ft = ((qn >> 1) + 1) * ((qn >> 1) + 1), fm = (int)rd_decode(d, (u32)ft);
            if (fm < ((qn >> 1) * ((qn >> 1) + 1) >> 1)) {
                itheta = (int)(isqrt(8 * (u32)fm + 1) - 1) >> 1;
                fs = itheta + 1;
                fl = itheta * (itheta + 1) >> 1;
            } else {
                itheta = (2 * (qn + 1) - (int)isqrt(8 * (u32)(ft - fm - 1) + 1)) >> 1;
                fs = qn + 1 - itheta;
                fl = ft - ((qn + 1 - itheta) * (qn + 2 - itheta) >> 1);
            }
            rd_update(d, (u32)fl, (u32)(fl + fs), (u32)ft);
        }
        itheta = (int)((u32)itheta * 16384 / (u32)qn);
    } else if (stereo) {
        /* Read, but not done: the right channel upside down would cancel
         * the left when the two are added for mono (libopus does the same
         * for a mono output). */
        if (*b > 2 << BITRES && bd->remaining > 2 << BITRES) (void)rd_bit(d, 2);
        itheta = 0;
    }
    sp->qalloc = (int)(rd_tell_frac(d) - tell);
    *b -= sp->qalloc;
    if (itheta == 0) {
        sp->imid = 32767; sp->iside = 0; *fill &= (1 << B) - 1; sp->delta = -16384;
    } else if (itheta == 16384) {
        sp->imid = 0; sp->iside = 32767; *fill &= ((1 << B) - 1) << B; sp->delta = 16384;
    } else {
        sp->imid = bcos(itheta);
        sp->iside = bcos(16384 - itheta);
        sp->delta = frac_mul16((N - 1) << 7, blog2tan(sp->iside, sp->imid));
    }
    sp->inv = inv;
    sp->itheta = itheta;
}

/* A band (or part of one) of one channel: split in halves while its bits
 * would buy more pulses than the pulse cache goes to, else its pulses, or
 * with none, a lower band's shape folded in (or noise). */
static unsigned partition(Band *bd, float *X, int N, int b, int B, float *lowband, int LM, float gain, int fill)
{
    const u8 *cache = OPUS_CACHE_BITS + OPUS_CACHE_INDEX[(LM + 1) * NB + bd->i];
    int B0 = B;
    unsigned cm = 0;
    if (LM != -1 && b > cache[cache[0]] + 12 && N > 2) {
        Split sp;
        int mbits, sbits, delta, rebalance;
        float mid, side, *Y, *lowband2 = 0;
        N >>= 1;
        Y = X + N;
        LM--;
        if (B == 1) fill = (fill & 1) | (fill << 1);
        B = (B + 1) >> 1;
        theta(bd, &sp, N, &b, B, B0, LM, 0, &fill);
        mid = sp.imid / 32768.f;
        side = sp.iside / 32768.f;
        delta = sp.delta;
        if (B0 > 1 && (sp.itheta & 0x3fff)) {
            if (sp.itheta > 8192) delta -= delta >> (4 - LM);
            else delta = imin(0, delta + (N << BITRES >> (5 - LM)));
        }
        mbits = imax(0, imin(b, (b - delta) / 2));
        sbits = b - mbits;
        bd->remaining -= sp.qalloc;
        if (lowband) lowband2 = lowband + N;
        rebalance = bd->remaining;
        if (mbits >= sbits) {
            cm = partition(bd, X, N, mbits, B, lowband, LM, gain * mid, fill);
            rebalance = mbits - (rebalance - bd->remaining);
            if (rebalance > 3 << BITRES && sp.itheta != 0) sbits += rebalance - (3 << BITRES);
            cm |= partition(bd, Y, N, sbits, B, lowband2, LM, gain * side, fill >> B) << (B0 >> 1);
        } else {
            cm = partition(bd, Y, N, sbits, B, lowband2, LM, gain * side, fill >> B) << (B0 >> 1);
            rebalance = sbits - (rebalance - bd->remaining);
            if (rebalance > 3 << BITRES && sp.itheta != 16384) mbits += rebalance - (3 << BITRES);
            cm |= partition(bd, X, N, mbits, B, lowband, LM, gain * mid, fill);
        }
        return cm;
    }
    {
        int q = bits2pulses(bd->i, LM, b), cb = pulses2bits(bd->i, LM, q), j;
        bd->remaining -= cb;
        while (bd->remaining < 0 && q > 0) {
            bd->remaining += cb;
            q--;
            cb = pulses2bits(bd->i, LM, q);
            bd->remaining -= cb;
        }
        if (q) return pvq(bd->d, X, N, get_pulses(q), bd->spread, B, gain);
        fill &= (1 << B) - 1;
        if (!fill) { for (j = 0; j < N; j++) X[j] = 0; return 0; }
        if (!lowband) {
            for (j = 0; j < N; j++) { bd->seed = lcg(bd->seed); X[j] = (float)((s32)bd->seed >> 20); }
            cm = (1u << B) - 1;
        } else {
            for (j = 0; j < N; j++) {
                bd->seed = lcg(bd->seed);
                X[j] = lowband[j] + (bd->seed & 0x8000 ? 1.0f / 256 : -1.0f / 256);
            }
            cm = (unsigned)fill;
        }
        renormalise(X, N, gain);
        return cm;
    }
}

static unsigned band_n1(Band *bd, float *X, float *Y, float *lowband_out)
{
    float *x = X;
    int c;
    for (c = 0; c < 1 + (Y != 0); c++) {
        int sign = 0;
        if (bd->remaining >= 1 << BITRES) { sign = (int)rd_bits(bd->d, 1); bd->remaining -= 1 << BITRES; }
        x[0] = sign ? -1.0f : 1.0f;
        x = Y;
    }
    if (lowband_out) lowband_out[0] = X[0];
    return 1;
}

/* One channel's band, with the time/frequency changes around it. */
static unsigned band(Band *bd, float *X, int N, int b, int B, float *lowband, int LM, float *lowband_out,
                     float gain, float *scratch, int fill)
{
    static const u8 bit_interleave[16] = { 0, 1, 1, 1, 2, 3, 3, 3, 2, 3, 3, 3, 2, 3, 3, 3 };
    static const u8 bit_deinterleave[16] = { 0x00, 0x03, 0x0C, 0x0F, 0x30, 0x33, 0x3C, 0x3F,
                                             0xC0, 0xC3, 0xCC, 0xCF, 0xF0, 0xF3, 0xFC, 0xFF };
    int N0 = N, N_B = N, N_B0, B0 = B, time_divide = 0, recombine = 0, longBlocks = B0 == 1, k;
    int tf_change = bd->tf_change;
    unsigned cm;
    N_B = (int)((u32)N_B / (u32)B);
    if (N == 1) return band_n1(bd, X, 0, lowband_out);
    if (tf_change > 0) recombine = tf_change;
    if (scratch && lowband && (recombine || ((N_B & 1) == 0 && tf_change < 0) || B0 > 1)) {
        for (k = 0; k < N; k++) scratch[k] = lowband[k];
        lowband = scratch;
    }
    for (k = 0; k < recombine; k++) {
        if (lowband) haar1(lowband, N >> k, 1 << k);
        fill = bit_interleave[fill & 0xF] | bit_interleave[fill >> 4] << 2;
    }
    B >>= recombine;
    N_B <<= recombine;
    while ((N_B & 1) == 0 && tf_change < 0) {
        if (lowband) haar1(lowband, N_B, B);
        fill |= fill << B;
        B <<= 1;
        N_B >>= 1;
        time_divide++;
        tf_change++;
    }
    B0 = B;
    N_B0 = N_B;
    if (B0 > 1 && lowband) deinterleave(lowband, N_B >> recombine, B0 << recombine, longBlocks);
    cm = partition(bd, X, N, b, B, lowband, LM, gain, fill);
    if (B0 > 1) interleave(X, N_B >> recombine, B0 << recombine, longBlocks);
    N_B = N_B0;
    B = B0;
    for (k = 0; k < time_divide; k++) {
        B >>= 1;
        N_B <<= 1;
        cm |= cm >> B;
        haar1(X, N_B, B);
    }
    for (k = 0; k < recombine; k++) {
        cm = bit_deinterleave[cm];
        haar1(X, N0 >> k, 1 << k);
    }
    B <<= recombine;
    if (lowband_out) {
        float n = (float)fsqrt((double)N0);
        for (k = 0; k < N0; k++) lowband_out[k] = n * X[k];
    }
    return cm & ((1u << B) - 1);
}

static void stereo_merge(float *X, float *Y, float mid, int N)
{
    float xp = 0, side = 0, El, Er, lg, rg;
    int j;
    for (j = 0; j < N; j++) { xp += Y[j] * X[j]; side += Y[j] * Y[j]; }
    xp *= mid;
    El = mid * mid + side - 2 * xp;
    Er = mid * mid + side + 2 * xp;
    if (Er < 6e-4f || El < 6e-4f) { for (j = 0; j < N; j++) Y[j] = X[j]; return; }
    lg = 1.0f / (float)fsqrt(El);
    rg = 1.0f / (float)fsqrt(Er);
    for (j = 0; j < N; j++) {
        float l = mid * X[j], r = Y[j];
        X[j] = lg * (l - r);
        Y[j] = rg * (l + r);
    }
}

static unsigned band_stereo(Band *bd, float *X, float *Y, int N, int b, int B, float *lowband, int LM,
                            float *lowband_out, float *scratch, int fill)
{
    Split sp;
    int mbits, sbits, orig_fill = fill, j;
    float mid, side;
    unsigned cm;
    if (N == 1) return band_n1(bd, X, Y, lowband_out);
    theta(bd, &sp, N, &b, B, B, LM, 1, &fill);
    mid = sp.imid / 32768.f;
    side = sp.iside / 32768.f;
    if (N == 2) {
        int c, sign = 0;
        float *x2, *y2, tmp;
        mbits = b;
        sbits = sp.itheta != 0 && sp.itheta != 16384 ? 1 << BITRES : 0;
        mbits -= sbits;
        c = sp.itheta > 8192;
        bd->remaining -= sp.qalloc + sbits;
        x2 = c ? Y : X;
        y2 = c ? X : Y;
        if (sbits) sign = (int)rd_bits(bd->d, 1);
        sign = 1 - 2 * sign;
        cm = band(bd, x2, N, mbits, B, lowband, LM, lowband_out, 1.0f, scratch, orig_fill);
        y2[0] = -sign * x2[1];
        y2[1] = sign * x2[0];
        X[0] *= mid; X[1] *= mid; Y[0] *= side; Y[1] *= side;
        tmp = X[0]; X[0] = tmp - Y[0]; Y[0] = tmp + Y[0];
        tmp = X[1]; X[1] = tmp - Y[1]; Y[1] = tmp + Y[1];
    } else {
        int rebalance;
        mbits = imax(0, imin(b, (b - sp.delta) / 2));
        sbits = b - mbits;
        bd->remaining -= sp.qalloc;
        rebalance = bd->remaining;
        if (mbits >= sbits) {
            cm = band(bd, X, N, mbits, B, lowband, LM, lowband_out, 1.0f, scratch, fill);
            rebalance = mbits - (rebalance - bd->remaining);
            if (rebalance > 3 << BITRES && sp.itheta != 0) sbits += rebalance - (3 << BITRES);
            cm |= band(bd, Y, N, sbits, B, 0, LM, 0, side, 0, fill >> B);
        } else {
            cm = band(bd, Y, N, sbits, B, 0, LM, 0, side, 0, fill >> B);
            rebalance = sbits - (rebalance - bd->remaining);
            if (rebalance > 3 << BITRES && sp.itheta != 16384) mbits += rebalance - (3 << BITRES);
            cm |= band(bd, X, N, mbits, B, lowband, LM, lowband_out, 1.0f, scratch, fill);
        }
        stereo_merge(X, Y, mid, N);
    }
    if (sp.inv) for (j = 0; j < N; j++) Y[j] = -Y[j];
    return cm;
}

static float s_norm[2 * 8 * 78];                 /* every band's shape, for folding */

static void all_bands(Rd *d, float *X_, float *Y_, u8 *masks, Alloc *a, int end, int shortBlocks, int spread,
                      const int *tf_res, int total_bits, int LM)
{
    int M = 1 << LM, B = shortBlocks ? M : 1, C = Y_ ? 2 : 1, i, lowband_offset = 0, update_lowband = 1;
    int balance = a->balance, dual = a->dual;
    float *norm = s_norm, *norm2 = s_norm + M * OPUS_EBANDS[NB - 1], *scratch = X_ + M * OPUS_EBANDS[NB - 1];
    Band bd;
    bd.d = d; bd.intensity = a->intensity; bd.spread = spread; bd.seed = S.seed; bd.avoid_split_noise = B > 1;
    for (i = 0; i < end; i++) {
        int tell = (int)rd_tell_frac(d), b, N = M * width(i), last = i == end - 1, eff = -1;
        float *X = X_ + M * OPUS_EBANDS[i], *Y = Y_ ? Y_ + M * OPUS_EBANDS[i] : 0;
        unsigned x_cm, y_cm;
        bd.i = i;
        if (i != 0) balance -= tell;
        bd.remaining = total_bits - tell - 1;
        if (i <= a->coded - 1) {
            int cb = balance / imin(3, a->coded - i);
            b = imax(0, imin(16383, imin(bd.remaining + 1, a->pulses[i] + cb)));
        } else b = 0;
        if ((M * OPUS_EBANDS[i] - N >= 0 || i == 1) && (update_lowband || lowband_offset == 0)) lowband_offset = i;
        if (i == 1) {                                /* the second band folds from a copy of the first */
            int n1 = M * width(0), n2 = M * width(1), k;
            for (k = 0; k < n2 - n1; k++) norm[n1 + k] = norm[2 * n1 - n2 + k];
            if (dual) for (k = 0; k < n2 - n1; k++) norm2[n1 + k] = norm2[2 * n1 - n2 + k];
        }
        bd.tf_change = tf_res[i];
        if (last) scratch = 0;
        if (lowband_offset != 0 && (spread != SPREAD_AGGRESSIVE || B > 1 || bd.tf_change < 0)) {
            int fs, fe, fi;
            eff = imax(0, M * OPUS_EBANDS[lowband_offset] - N);
            fs = lowband_offset;
            while (M * OPUS_EBANDS[--fs] > eff) { }
            fe = lowband_offset - 1;
            while (++fe < i && M * OPUS_EBANDS[fe] < eff + N) { }
            x_cm = y_cm = 0;
            fi = fs;
            do { x_cm |= masks[fi * C]; y_cm |= masks[fi * C + C - 1]; } while (++fi < fe);
        } else x_cm = y_cm = (1u << B) - 1;
        if (dual && i == a->intensity) {
            int j;
            dual = 0;
            for (j = 0; j < M * OPUS_EBANDS[i]; j++) norm[j] = 0.5f * (norm[j] + norm2[j]);
        }
        if (dual) {
            x_cm = band(&bd, X, N, b / 2, B, eff != -1 ? norm + eff : 0, LM, last ? 0 : norm + M * OPUS_EBANDS[i],
                        1.0f, scratch, (int)x_cm);
            y_cm = band(&bd, Y, N, b / 2, B, eff != -1 ? norm2 + eff : 0, LM, last ? 0 : norm2 + M * OPUS_EBANDS[i],
                        1.0f, scratch, (int)y_cm);
        } else {
            if (Y) x_cm = band_stereo(&bd, X, Y, N, b, B, eff != -1 ? norm + eff : 0, LM,
                                      last ? 0 : norm + M * OPUS_EBANDS[i], scratch, (int)(x_cm | y_cm));
            else x_cm = band(&bd, X, N, b, B, eff != -1 ? norm + eff : 0, LM, last ? 0 : norm + M * OPUS_EBANDS[i],
                             1.0f, scratch, (int)(x_cm | y_cm));
            y_cm = x_cm;
        }
        masks[i * C] = (u8)x_cm;
        masks[i * C + C - 1] = (u8)y_cm;
        balance += a->pulses[i] + tell;
        update_lowband = b > (N << BITRES);
        bd.avoid_split_noise = 0;
    }
    S.seed = bd.seed;
}

static void anti_collapse(float *X_, u8 *masks, int LM, int C, int size, int end, const int *pulses, u32 seed)
{
    int c, i, j, k;
    for (i = 0; i < end; i++) {
        int N0 = width(i), depth = (int)((u32)(1 + pulses[i]) / (u32)(N0 << LM));
        float thresh = 0.5f * fexp2(-0.125f * depth), sqrt_1 = 1.0f / (float)fsqrt((double)(N0 << LM));
        for (c = 0; c < C; c++) {
            float p1 = S.oldL[c * NB + i], p2 = S.oldL2[c * NB + i], ediff, r, *X;
            int renorm = 0;
            if (C == 1) {
                if (S.oldL[NB + i] > p1) p1 = S.oldL[NB + i];
                if (S.oldL2[NB + i] > p2) p2 = S.oldL2[NB + i];
            }
            ediff = S.oldE[c * NB + i] - (p1 < p2 ? p1 : p2);
            if (ediff < 0) ediff = 0;
            r = 2.0f * fexp2(-ediff);
            if (LM == 3) r *= 1.41421356f;
            if (r > thresh) r = thresh;
            r *= sqrt_1;
            X = X_ + c * size + (OPUS_EBANDS[i] << LM);
            for (k = 0; k < 1 << LM; k++)
                if (!(masks[i * C + c] & 1 << k)) {
                    for (j = 0; j < N0; j++) {
                        seed = lcg(seed);
                        X[(j << LM) + k] = seed & 0x8000 ? r : -r;
                    }
                    renorm = 1;
                }
            if (renorm) renormalise(X, N0 << LM, 1.0f);
        }
    }
}

/* ---- the transform: a DCT-IV by an FFT of a quarter its size ------------------------
 * CELT's blocks are 120 x 2^k long, so the FFT is mixed radix (2, 3, 4, 5).
 * Its twiddles are for the longest, 480 points (a 960 block at 48 kHz), and
 * the shorter ones step through them. */
typedef struct { float r, i; } Cpx;
#define FFTMAX (OUTN / 2)                        /* 240 at 24 kHz, 480 at 48        */
static Cpx s_tw[FFTMAX];                         /* exp(-2 pi i k / FFTMAX)         */
static Cpx s_pre[FFTMAX * 2], s_post[FFTMAX * 2];   /* the DCT-IV's twiddles, for L = 60 .. OUTN */
static int s_pre_at[5];                          /* where each size's start         */
static Cpx s_fa[FFTMAX], s_fb[FFTMAX];

static void fft(Cpx *out, const Cpx *in, int n, int stride, int twstep)
{
    int p = n % 4 == 0 && n != 8 ? 4 : n % 2 == 0 ? 2 : n % 3 == 0 ? 3 : 5, m = n / p, k, q, u;
    if (m == 1) { for (q = 0; q < p; q++) out[q] = in[q * stride]; }
    else for (q = 0; q < p; q++) fft(out + q * m, in + q * stride, m, stride * p, twstep * p);
    for (k = 0; k < m; k++) {
        Cpx t[5];
        for (q = 0; q < p; q++) {
            Cpx a = out[k + q * m], w = s_tw[(q * k * twstep) % FFTMAX];
            t[q].r = a.r * w.r - a.i * w.i;
            t[q].i = a.r * w.i + a.i * w.r;
        }
        for (u = 0; u < p; u++) {
            Cpx s = { 0, 0 };
            for (q = 0; q < p; q++) {
                Cpx w = s_tw[(q * u * (FFTMAX / p)) % FFTMAX];
                s.r += t[q].r * w.r - t[q].i * w.i;
                s.i += t[q].r * w.i + t[q].i * w.r;
            }
            out[k + u * m] = s;
        }
    }
}

static int size_index(int L) { int s = 0; while ((60 << s) != L && s < 4) s++; return s; }

/* y[n] = sum over k < L of x[k] cos(pi/L (n + 1/2)(k + 1/2)), x read with a stride. */
static void dct4(const float *x, int stride, float *y, int L)
{
    int h = L / 2, m, at = s_pre_at[size_index(L)];
    for (m = 0; m < h; m++) {
        float a = x[2 * m * stride], b = x[(L - 1 - 2 * m) * stride];
        Cpx w = s_pre[at + m];
        s_fa[m].r = a * w.r - b * w.i;
        s_fa[m].i = a * w.i + b * w.r;
    }
    fft(s_fb, s_fa, h, 1, FFTMAX / h);
    for (m = 0; m < h; m++) {
        Cpx w = s_post[at + m], z = s_fb[m];
        y[2 * m] = z.r * w.r - z.i * w.i;
        y[L - 1 - 2 * m] = -(z.r * w.i + z.i * w.r);
    }
}

static void transform_tables(void)
{
    int k, s, at = 0;
    for (k = 0; k < FFTMAX; k++) {
        s_tw[k].r = (float)fcos(2 * PI * k / FFTMAX);
        s_tw[k].i = (float)-fsin(2 * PI * k / FFTMAX);
    }
    for (s = 0; (60 << s) <= OUTN; s++) {        /* L = 60, 120, 240, 480 (and 960) */
        int L = 60 << s, h = L / 2, m;
        s_pre_at[s] = at;
        for (m = 0; m < h; m++) {
            s_pre[at + m].r = (float)fcos(PI * (m + 0.25) / L);
            s_pre[at + m].i = (float)-fsin(PI * (m + 0.25) / L);
            s_post[at + m].r = (float)fcos(PI * m / L);
            s_post[at + m].i = (float)-fsin(PI * m / L);
        }
        at += h;
    }
}

/* One block's L coefficients (strided) into L new samples at out, with the
 * last block's tail; its own tail kept.  CELT's windows overlap by ov only:
 * the block's 2L-long inverse MDCT is zero outside its middle L + ov. */
static float s_dct[OUTN];
static void imdct_block(const float *x, int stride, int L, float *out)
{
    int ov = S.ov, i, a = L / 2 - ov / 2;        /* where the window rises, in the 2L */
    dct4(x, stride, s_dct, L);
    /* y[n], n in [0, 2L): with m = n + L/2, u[m] for m < L, -u[2L-1-m] up to 2L, -u[m-2L] after. */
#define Y(n) ((n) + L / 2 < L ? s_dct[(n) + L / 2] : (n) + L / 2 < 2 * L ? -s_dct[2 * L - 1 - ((n) + L / 2)] : -s_dct[(n) + L / 2 - 2 * L])
    for (i = 0; i < ov; i++) out[i] = S.tail[i] + S.window[i] * Y(a + i);
    for (i = ov; i < L; i++) out[i] = Y(a + i);
    for (i = 0; i < ov; i++) S.tail[i] = S.window[ov - 1 - i] * Y(a + L + i);
#undef Y
}

/* ---- the pitch post-filter ----------------------------------------------------------- */
static void comb(float *y, int T0, int T1, int N, float g0, float g1, int ts0, int ts1)
{
    float g00, g01, g02, g10, g11, g12;
    int i, ov = S.ov;
    if (g0 == 0 && g1 == 0) return;
    g00 = g0 * OPUS_COMB_GAINS[ts0 * 3]; g01 = g0 * OPUS_COMB_GAINS[ts0 * 3 + 1]; g02 = g0 * OPUS_COMB_GAINS[ts0 * 3 + 2];
    g10 = g1 * OPUS_COMB_GAINS[ts1 * 3]; g11 = g1 * OPUS_COMB_GAINS[ts1 * 3 + 1]; g12 = g1 * OPUS_COMB_GAINS[ts1 * 3 + 2];
    if (S.half) {                                /* taps two 48 kHz samples apart: fold the middle ones in */
        g00 += g01; g01 = 0.5f * g01 + g02; g02 = 0;
        g10 += g11; g11 = 0.5f * g11 + g12; g12 = 0;
        T0 = (T0 + 1) >> 1; T1 = (T1 + 1) >> 1;
    }
    if (g0 == g1 && T0 == T1 && ts0 == ts1) ov = 0;
    for (i = 0; i < N; i++) {
        float f = i < ov ? S.window[i] * S.window[i] : 1.0f;
        y[i] = y[i] + (1 - f) * (g00 * y[i - T0] + g01 * (y[i - T0 + 1] + y[i - T0 - 1]) + g02 * (y[i - T0 + 2] + y[i - T0 - 2]))
                    + f * (g10 * y[i - T1] + g11 * (y[i - T1 + 1] + y[i - T1 - 1]) + g12 * (y[i - T1 + 2] + y[i - T1 - 2]));
        if (i >= ov && g1 == 0) break;
    }
}

/* ---- a frame (4.3) ---------------------------------------------------------------- */
static float s_X[2 * MAXN];
static float s_freq[OUTN];

/* One CELT frame of 120 << LM samples (at 48 kHz) from len bytes, with C
 * channels coded and bands up to end; n samples into out (half as many at
 * 24 kHz).  Returns how many. */
static int celt_frame(const u8 *p, int len, int LM, int C, int end, s16 *out)
{
    static const u8 trim_default = 5;
    Rd d;
    Alloc a;
    int M = 1 << LM, N = 120 << LM, total, tell, silence, pitch = 0, tapset = 0, transient = 0, intra, i, c;
    int tf_res[NB], spread, dyn_logp = 6, trim, bits, ac_rsv, ac_on = 0;
    float pf_gain = 0;
    u8 masks[2 * NB];
    int Nout = S.half ? N / 2 : N;

    rd_init(&d, p, (u32)len);
    if (C == 1) for (i = 0; i < NB; i++) if (S.oldE[NB + i] > S.oldE[i]) S.oldE[i] = S.oldE[NB + i];
    total = len * 8;
    tell = rd_tell(&d);
    silence = tell >= total ? 1 : tell == 1 ? rd_bit(&d, 15) : 0;
    if (silence) { d.total_bits += total - rd_tell(&d); tell = total; }
    if (tell + 16 <= total) {
        if (rd_bit(&d, 1)) {
            int oct = (int)rd_uint(&d, 6), qg;
            pitch = (16 << oct) + (int)rd_bits(&d, 4 + oct) - 1;
            qg = (int)rd_bits(&d, 3);
            if (rd_tell(&d) + 2 <= total) tapset = rd_icdf(&d, OPUS_TAPSET_ICDF, 2);
            pf_gain = 0.09375f * (qg + 1);
        }
        tell = rd_tell(&d);
    }
    if (LM > 0 && tell + 3 <= total) { transient = rd_bit(&d, 3); tell = rd_tell(&d); }
    intra = tell + 3 <= total ? rd_bit(&d, 3) : 0;
    coarse_energy(&d, end, intra, C, LM, total);

    /* time/frequency resolution */
    {
        int budget = total, curr = 0, changed = 0, logp = transient ? 2 : 4, sel_rsv, sel = 0;
        tell = rd_tell(&d);
        sel_rsv = LM > 0 && tell + logp + 1 <= budget;
        budget -= sel_rsv;
        for (i = 0; i < end; i++) {
            if (tell + logp <= budget) { curr ^= rd_bit(&d, logp); tell = rd_tell(&d); changed |= curr; }
            tf_res[i] = curr;
            logp = transient ? 4 : 5;
        }
        if (sel_rsv && OPUS_TF_SELECT[LM * 8 + 4 * transient + changed] != OPUS_TF_SELECT[LM * 8 + 4 * transient + 2 + changed])
            sel = rd_bit(&d, 1);
        for (i = 0; i < end; i++) tf_res[i] = OPUS_TF_SELECT[LM * 8 + 4 * transient + 2 * sel + tf_res[i]];
    }
    tell = rd_tell(&d);
    spread = tell + 4 <= total ? rd_icdf(&d, OPUS_SPREAD_ICDF, 5) : 2;

    for (i = 0; i < NB; i++) a.cap[i] = (OPUS_CACHE_CAPS[NB * (2 * LM + C - 1) + i] + 64) * C * (width(i) << LM) >> 2;
    total <<= BITRES;
    tell = (int)rd_tell_frac(&d);
    for (i = 0; i < end; i++) {
        int w = C * width(i) << LM, quanta = imin(w << BITRES, imax(6 << BITRES, w)), loop_logp = dyn_logp, boost = 0;
        while (tell + (loop_logp << BITRES) < total && boost < a.cap[i]) {
            int flag = rd_bit(&d, loop_logp);
            tell = (int)rd_tell_frac(&d);
            if (!flag) break;
            boost += quanta;
            total -= quanta;
            loop_logp = 1;
        }
        a.offsets[i] = boost;
        if (boost > 0) dyn_logp = imax(2, dyn_logp - 1);
    }
    trim = tell + (6 << BITRES) <= total ? rd_icdf(&d, OPUS_TRIM_ICDF, 7) : trim_default;
    bits = ((len * 8) << BITRES) - (int)rd_tell_frac(&d) - 1;
    ac_rsv = transient && LM >= 2 && bits >= (LM + 2) << BITRES ? 1 << BITRES : 0;
    bits -= ac_rsv;
    for (i = end; i < NB; i++) a.offsets[i] = 0;
    allocate(&d, &a, end, trim, bits, C, LM);

    for (i = 0; i < end; i++)                    /* fine energy */
        if (a.ebits[i] > 0)
            for (c = 0; c < C; c++) {
                int q2 = (int)rd_bits(&d, a.ebits[i]);
                S.oldE[i + c * NB] += (q2 + 0.5f) * (float)(1 << (14 - a.ebits[i])) * (1.0f / 16384) - 0.5f;
            }

    for (i = 0; i < C * N; i++) s_X[i] = 0;
    all_bands(&d, s_X, C == 2 ? s_X + N : 0, masks, &a, end, transient ? M : 0, spread, tf_res,
              len * (8 << BITRES) - ac_rsv, LM);
    if (ac_rsv > 0) ac_on = (int)rd_bits(&d, 1);
    {                                            /* the last bits: more fine energy */
        int left = len * 8 - rd_tell(&d), prio;
        for (prio = 0; prio < 2; prio++)
            for (i = 0; i < end && left >= C; i++) {
                if (a.ebits[i] >= 8 || a.prio[i] != prio) continue;
                for (c = 0; c < C; c++) {
                    int q2 = (int)rd_bits(&d, 1);
                    S.oldE[i + c * NB] += (q2 - 0.5f) * (float)(1 << (14 - a.ebits[i] - 1)) * (1.0f / 16384);
                    left--;
                }
            }
    }
    if (ac_on) anti_collapse(s_X, masks, LM, C, N, end, a.pulses, S.seed);
    if (silence) for (i = 0; i < 2 * NB; i++) S.oldE[i] = -28.0f;

    /* The spectrum, both channels added (halved), then into time. */
    {
        int top = S.half ? N / 2 : N, B = transient ? M : 1, L = transient ? 120 : N, b;
        for (i = 0; i < top; i++) s_freq[i] = 0;
        if (!silence)
            for (c = 0; c < C; c++)
                for (i = 0; i < end; i++) {
                    int j, e = M * OPUS_EBANDS[i + 1];
                    float lg = S.oldE[c * NB + i] + OPUS_EMEANS[i] / 16.0f, g;
                    if (M * OPUS_EBANDS[i] >= top) break;
                    g = fexp2(lg < 32.0f ? lg : 32.0f) * (C == 2 ? 0.5f : 1.0f);
                    for (j = M * OPUS_EBANDS[i]; j < e && j < top; j++) s_freq[j] += g * s_X[c * N + j];
                }
        {
            float *o = S.syn + HIST;
            int Lout = S.half ? L / 2 : L;
            for (b = 0; b < B; b++) imdct_block(s_freq + b, B, Lout, o + b * Lout);
        }
    }

    /* the post-filter: last frame's settings faded into this one's */
    {
        float *o = S.syn + HIST;
        int sh = S.half ? 60 : 120;
        if (S.pf_t < 15) S.pf_t = 15;
        if (S.pf_t_old < 15) S.pf_t_old = 15;
        comb(o, S.pf_t_old, S.pf_t, sh, S.pf_g_old, S.pf_g, S.pf_set_old, S.pf_set);
        if (LM != 0) comb(o + sh, S.pf_t, pitch < 15 ? 15 : pitch, Nout - sh, S.pf_g, pf_gain, S.pf_set, tapset);
        S.pf_t_old = S.pf_t; S.pf_g_old = S.pf_g; S.pf_set_old = S.pf_set;
        S.pf_t = pitch; S.pf_g = pf_gain; S.pf_set = tapset;
        if (LM != 0) { S.pf_t_old = S.pf_t; S.pf_g_old = S.pf_g; S.pf_set_old = S.pf_set; }
    }

    /* what the next frame remembers */
    if (C == 1) for (i = 0; i < NB; i++) S.oldE[NB + i] = S.oldE[i];
    if (!transient) {
        for (i = 0; i < 2 * NB; i++) { S.oldL2[i] = S.oldL[i]; S.oldL[i] = S.oldE[i]; }
    } else for (i = 0; i < 2 * NB; i++) if (S.oldE[i] < S.oldL[i]) S.oldL[i] = S.oldE[i];
    for (c = 0; c < 2; c++)
        for (i = end; i < NB; i++) { S.oldE[c * NB + i] = 0; S.oldL[c * NB + i] = S.oldL2[c * NB + i] = -28.0f; }
    S.seed = d.rng;

    /* de-emphasis, out, and the history moved on */
    {
        float *o = S.syn + HIST;
        for (i = 0; i < Nout; i++) {
            float x = o[i], v;
            if (S.half) {                        /* the 48 kHz filter, its odd input guessed between */
                v = x + 0.85000610f * 0.5f * (x + S.de_x) + 0.72250999f * S.de_m;
                S.de_x = x;
            } else v = x + 0.85000610f * S.de_m;
            S.de_m = v;
            {
                int s = dsp_round(v * AUDIO_HEADROOM);
                out[i] = (s16)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
            }
        }
        for (i = 0; i < HIST; i++) S.syn[i] = S.syn[i + Nout];
    }
    return Nout;
}

/* ---- packets (RFC 6716 section 3) ------------------------------------------------------ */
static int frame_len(const u8 *p, int n, int *len)
{
    if (n < 1) return -1;
    if (p[0] < 252) { *len = p[0]; return 1; }
    if (n < 2) return -1;
    *len = 4 * p[1] + p[0];
    return 2;
}

/* A packet into out (room for 120 ms); how many samples, 0 if it cannot be
 * read.  SILK and hybrid frames come out silent. */
int opus_packet(const u8 *p, int n, s16 *out)
{
    int toc, cfg, code, count, sizes[48], i, at = 1, LM, C, bw, end, total = 0, fn;
    if (n < 1) return 0;
    toc = p[0];
    cfg = toc >> 3;
    C = 1 + ((toc >> 2) & 1);
    code = toc & 3;
    if (cfg >= 16) { LM = cfg & 3; bw = (cfg - 16) >> 2; }     /* CELT: NB, WB, SWB, FB */
    else if (cfg >= 12) { LM = 2 + (cfg & 1); bw = -1; }       /* hybrid: 10 or 20 ms */
    else { LM = (cfg & 3) == 3 ? 5 : 2 + (cfg & 3); bw = -1; } /* SILK: 10, 20, 40, 60 ms */
    fn = 120 << (LM > 3 ? 3 : LM);
    if (LM == 4) fn = 1920; else if (LM == 5) fn = 2880;
    end = bw == 0 ? 13 : bw == 1 ? 17 : bw == 2 ? 19 : 21;
    if (code == 0) { count = 1; sizes[0] = n - 1; }
    else if (code == 1) { if ((n - 1) & 1) return 0; count = 2; sizes[0] = sizes[1] = (n - 1) / 2; }
    else if (code == 2) {
        int k = frame_len(p + 1, n - 1, &sizes[0]);
        if (k < 0) return 0;
        at += k;
        count = 2;
        sizes[1] = n - at - sizes[0];
        if (sizes[1] < 0) return 0;
    } else {
        int vbr, pad = 0, last;
        if (n < 2) return 0;
        count = p[1] & 63; vbr = p[1] & 128;
        at = 2;
        if (!count || count * fn > 5760) return 0;
        if (p[1] & 64) {                         /* padding */
            int b;
            do {
                if (at >= n) return 0;
                b = p[at++];
                pad += b == 255 ? 254 : b;
            } while (b == 255);
        }
        last = n - at - pad;
        if (vbr) {
            for (i = 0; i < count - 1; i++) {
                int k = frame_len(p + at, n - at, &sizes[i]);
                if (k < 0) return 0;
                at += k;
                last -= k + sizes[i];
            }
            sizes[count - 1] = last;
        } else {
            for (i = 0; i < count; i++) sizes[i] = (n - at - pad) / count;
        }
        if (last < 0) return 0;
    }
    for (i = 0; i < count; i++) {
        int got;
        if (at + sizes[i] > n || sizes[i] < 0) return total;
        if (bw < 0 || sizes[i] <= 1) {           /* not CELT, or lost: silence */
            int k, nn = S.half ? fn / 2 : fn;
            for (k = 0; k < nn; k++) out[total + k] = 0;
            got = nn;
        } else got = celt_frame(p + at, sizes[i], LM, C, end, out + total);
        at += sizes[i];
        total += got;
    }
    return total;
}

/* Is this packet music (CELT), which this reads? */
int opus_playable(const u8 *p, int n) { return n >= 1 && (p[0] >> 3) >= 16; }

void opus_start(int half)
{
    int i;
    static int ready;
    if (!ready) { transform_tables(); ready = 1; }
    for (i = 0; i < (int)sizeof S; i++) ((u8 *)&S)[i] = 0;
    for (i = 0; i < 2 * NB; i++) S.oldL[i] = S.oldL2[i] = -28.0f;
    S.half = OPUS_HALF ? 1 : half;               /* the buffers are sized for one or the other */
    S.ov = S.half ? OV / 2 : OV;
    for (i = 0; i < S.ov; i++) {
        double s = fsin(0.5 * PI * (i + 0.5) / S.ov);
        S.window[i] = (float)fsin(0.5 * PI * s * s);
    }
}
