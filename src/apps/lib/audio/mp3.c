/* ===========================================================================
 * mp3.c -- MPEG audio layers III and II, into mono samples.
 *
 * An mp3 is frames of 1152 samples (576 for the lower rates), each frame
 * a header, some "side information" saying how its bits are spent, and
 * the samples themselves: frequency lines, Huffman-packed and scaled.
 * Undoing that, per granule of 576 lines:
 *
 *   Huffman decode -> requantize (|x|^4/3, times the scale factors)
 *   -> stereo (mid/side back to left/right) -> reorder short blocks
 *   -> alias reduction -> inverse MDCT, overlapped with the last granule
 *   -> the polyphase synthesis filterbank -> 576 samples
 *
 * Built for a 120 MHz Pentium, so with two economies.  It is mono: both
 * channels are decoded to subband samples and added before the synthesis,
 * which is the expensive part, so that runs once.  And for 44.1 and 48 kHz
 * recordings it works at half rate: only the lower 16 subbands (under 11 or
 * 12 kHz) are transformed, and the synthesis makes every other sample --
 * 22 kHz out, which is what the Sound Blaster here plays anyway.
 *
 * Floating point, on the FPU, which on a Pentium is much quicker than its
 * integer multiply.  MPEG-1 and the low-rate MPEG-2 and 2.5 are read;
 * intensity stereo is not undone (for mono it changes the level of a few
 * bands at most).  The fixed tables are in mp3tab.h.
 *
 * Layer II (mp2), older and simpler, is read too (see layer2() below): it
 * shares the frame headers and the synthesis filterbank, and nothing else.
 * ===========================================================================
 */
#include "decode.h"
#include "gen/mp3tab.h"
#include "dsp.h"                         /* src/apps/lib/dsp: the fast DCT, quick rounding */
#include "prefix.h"                      /* src/apps/lib/bits: codebooks by table */

/* ---- maths without a library ---------------------------------------------- */
static double fsin(double x) { double r; __asm__("fsin" : "=t"(r) : "0"(x)); return r; }
static double fcos(double x) { double r; __asm__("fcos" : "=t"(r) : "0"(x)); return r; }
static double fsqrt(double x) { double r; __asm__("fsqrt" : "=t"(r) : "0"(x)); return r; }
#define PI 3.14159265358979323846

/* ---- the fixed tables, worked out once -------------------------------------- */
static float s_pow43[8207];              /* |x|^(4/3)                       */
static float s_pow2q[512];               /* 2^(e/4), e from -400            */
#define POW2Q(e) s_pow2q[(e) + 400 < 0 ? 0 : (e) + 400 > 511 ? 511 : (e) + 400]
static float s_dct4[18][18];             /* the long transform, as a DCT-IV  */
static float s_imdct_s[6][12];
static float s_win[4][36];               /* windows by block type           */
static float s_cs[8], s_ca[8];           /* alias reduction                 */
static float s_sf2[64];                  /* layer II's scale factors: 2^(1 - i/3) */
static int   s_ready;
static int   s_lay;                      /* the last header's layer: 3 or 2 */

/* Huffman: each table decoded by lookup on the next nine bits (prefix.h),
 * not walked a bit at a time.  Tables that share their codes share one. */
static Prefix s_hp[34];
static int    s_hp_ok[34];
static u32    s_hstore[32768];

static void build_codes(void)
{
    static u32 codes[256];
    static u8 lens[256];
    u32 used = 0;
    int t, k, i;
    for (t = 0; t < 34; t++) {
        if (!HTAB[t].codes) continue;
        for (k = 0; k < t; k++)
            if (HTAB[k].codes == HTAB[t].codes && s_hp_ok[k]) { s_hp[t] = s_hp[k]; s_hp_ok[t] = 1; break; }
        if (s_hp_ok[t]) continue;
        for (i = 0; i < 256; i++) lens[i] = 0;
        for (i = 0; i < HTAB[t].n; i++) {
            u32 c = HTAB[t].codes[i];
            codes[c & 0xFF] = (c >> 8) & 0xFFFF;
            lens[c & 0xFF] = (u8)(c >> 24);
        }
        s_hp_ok[t] = prefix_build(&s_hp[t], s_hstore + used, (u32)(sizeof s_hstore / 4) - used, 9, codes, lens, 256, 0);
        if (s_hp_ok[t]) used += s_hp[t].used;
    }
}

static void tables(void)
{
    int i, k;
    if (s_ready) return;
    for (i = 0; i < 8207; i++) {
        /* x^(4/3) = x * cbrt(x); the cube root by Newton from a guess. */
        double x = i, y = i < 2 ? i : x / 3.0;
        for (k = 0; k < 40 && i > 1; k++) y = (2.0 * y + x / (y * y)) / 3.0;
        s_pow43[i] = (float)(x * y);
    }
    for (i = 0; i < 512; i++) {
        int e = i - 400, q = e >> 2 >= 0 ? e >> 2 : -((-e + 3) >> 2);
        double v = 1.0;
        int f = e - q * 4;               /* 0..3                            */
        static const double frac[4] = { 1.0, 1.189207115002721, 1.414213562373095, 1.681792830507429 };
        v = frac[f];
        while (q > 0) { v *= 2.0; q--; }
        while (q < 0) { v *= 0.5; q++; }
        s_pow2q[i] = (float)v;
    }
    for (i = 0; i < 18; i++)
        for (k = 0; k < 18; k++) s_dct4[i][k] = (float)fcos(PI / 18.0 * (i + 0.5) * (k + 0.5));
    for (i = 0; i < 12; i++)
        for (k = 0; k < 6; k++)
            s_imdct_s[k][i] = (float)fcos(PI / 24.0 * (2 * i + 1 + 6) * (2 * k + 1));
    for (i = 0; i < 36; i++) {
        s_win[0][i] = (float)fsin(PI / 36.0 * (i + 0.5));
        s_win[1][i] = i < 18 ? s_win[0][i] : i < 24 ? 1.0f : i < 30 ? (float)fsin(PI / 12.0 * (i - 18 + 0.5)) : 0.0f;
        s_win[3][i] = i < 6 ? 0.0f : i < 12 ? (float)fsin(PI / 12.0 * (i - 6 + 0.5)) : i < 18 ? 1.0f : s_win[0][i];
        s_win[2][i] = i < 12 ? (float)fsin(PI / 12.0 * (i + 0.5)) : 0.0f;
    }
    {
        static const double ci[8] = { -0.6, -0.535, -0.33, -0.185, -0.095, -0.041, -0.0142, -0.0037 };
        for (i = 0; i < 8; i++) {
            double sq = fsqrt(1.0 + ci[i] * ci[i]);
            s_cs[i] = (float)(1.0 / sq);
            s_ca[i] = (float)(ci[i] / sq);
        }
    }
    for (i = 0; i < 64; i++) {
        double v = 2.0;
        for (k = 0; k < i; k++) v *= 0.79370052598409973738;   /* 2^(-1/3) */
        s_sf2[i] = (float)v;
    }
    build_codes();
    s_ready = 1;
}

/* ---- the lower rates' scalefactor bands (MPEG-2, 2.5) ------------------------- */
static const u16 SFB_LONG2[6][23] = {
    { 0,6,12,18,24,30,36,44,54,66,80,96,116,140,168,200,238,284,336,396,464,522,576 },   /* 22.05 */
    { 0,6,12,18,24,30,36,44,54,66,80,96,114,136,162,194,232,278,332,394,464,540,576 },   /* 24    */
    { 0,6,12,18,24,30,36,44,54,66,80,96,116,140,168,200,238,284,336,396,464,522,576 },   /* 16    */
    { 0,6,12,18,24,30,36,44,54,66,80,96,116,140,168,200,238,284,336,396,464,522,576 },   /* 11.025 */
    { 0,6,12,18,24,30,36,44,54,66,80,96,116,140,168,200,238,284,336,396,464,522,576 },   /* 12    */
    { 0,12,24,36,48,60,72,88,108,132,160,192,232,280,336,400,476,566,568,570,572,574,576 }, /* 8 */
};
static const u16 SFB_SHORT2[6][14] = {
    { 0,4,8,12,18,24,32,42,56,74,100,132,174,192 },
    { 0,4,8,12,18,26,36,48,62,80,104,136,180,192 },
    { 0,4,8,12,18,26,36,48,62,80,104,134,174,192 },
    { 0,4,8,12,18,26,36,48,62,80,104,134,174,192 },
    { 0,4,8,12,18,26,36,48,62,80,104,134,174,192 },
    { 0,8,16,24,36,52,72,96,124,160,162,164,166,192 },
};
/* How many scalefactors in each of four parts, MPEG-2 (ISO 13818-3). */
static const u8 NR_SFB[6][3][4] = {
    { {6,5,5,5},   {9,9,9,9},    {6,9,9,9}   },
    { {6,5,7,3},   {9,9,12,6},   {6,9,12,6}  },
    { {11,10,0,0}, {18,18,0,0},  {15,18,0,0} },
    { {7,7,7,0},   {12,12,12,0}, {6,15,12,0} },
    { {6,6,6,3},   {12,9,9,6},   {6,12,9,6}  },
    { {8,8,5,0},   {15,12,9,0},  {6,18,9,0}  },
};
static const u8 PRETAB[22] = { 0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,2,2,3,3,3,2,0 };

/* ---- one decoder's state ------------------------------------------------------- */
typedef struct {
    int part23, big_values, global_gain, sfc, window_switching, block_type, mixed;
    int table[3], subblock_gain[3], region0, region1, preflag, sf_scale, count1table;
} Granule;

static struct {
    int ver, lsf, sr_index, rate, nch, mode, mode_ext, kbps, half, crc, frame_len;
    int main_begin, scfsi[2][4];
    Granule gr[2][2];
    u8  sf_l[2][22];                     /* long-block scalefactors         */
    u8  sf_s[2][13][3];                  /* short, by window                */
    u8  res[4096 + 4];                   /* the bit reservoir, and room to read past it */
    int res_len;
    float xr[2][576];                    /* frequency lines                 */
    float overlap[2][32][18];
    float sb[18][32];                    /* subband samples, both channels  */
    float v[2048];                       /* the synthesis's history, twice over */
    int   voff;
    s16  out[1152];                      /* a frame's samples, not yet taken */
    int   out_n, out_at;
    const u8 *bits;                      /* main data being read            */
    u32   bitpos, bitend;
    int   nz[2];                         /* lines past this are zero, per channel */
    int   sblim;                         /* subbands this granule needs transformed */
    int   aalim;                         /* and the alias reduction's reach (not cut to half rate) */
} M;

/* Up to 25 bits at once, with one load (bits.h); the buffers have four
 * zero bytes after their end, and from past the end it reads nothing. */
static inline u32 peekbits(int n)
{
    return (bits_be32(M.bits + (M.bitpos >> 3)) << (M.bitpos & 7)) >> (32 - n);
}

static u32 getbits(int n)
{
    u32 v;
    if (n <= 0) return 0;
    if (M.bitpos >= M.bitend) { M.bitpos += (u32)n; return 0; }
    v = peekbits(n);
    M.bitpos += (u32)n;
    return v;
}

/* ---- frame headers ------------------------------------------------------------ */
static const u16 BITRATE[2][15] = {
    { 0,32,40,48,56,64,80,96,112,128,160,192,224,256,320 },      /* MPEG-1        */
    { 0,8,16,24,32,40,48,56,64,80,96,112,128,144,160 },          /* MPEG-2, 2.5   */
};
static const u16 BITRATE_L2[15] = { 0,32,48,56,64,80,96,112,128,160,192,224,256,320,384 };   /* MPEG-1 layer II */
static const u16 RATES[3][3] = { {44100,48000,32000}, {22050,24000,16000}, {11025,12000,8000} };

/* A layer III or II header at p?  Fills in what it says (and s_lay); the
 * frame's length. */
static int header(const u8 *p, int *ver, int *sri, int *nch, int *kbps)
{
    int v, lay, bri, s;
    if (p[0] != 0xFF || (p[1] & 0xE0) != 0xE0) return 0;
    v = (p[1] >> 3) & 3;                 /* 3: MPEG-1, 2: MPEG-2, 0: 2.5    */
    lay = (p[1] >> 1) & 3;               /* 1: layer III, 2: layer II       */
    bri = p[2] >> 4;
    s = (p[2] >> 2) & 3;
    if (v == 1 || (lay != 1 && lay != 2) || bri == 0 || bri == 15 || s == 3) return 0;
    *ver = v == 3 ? 0 : v == 2 ? 1 : 2;
    *sri = s;
    *nch = ((p[3] >> 6) & 3) == 3 ? 1 : 2;
    s_lay = lay == 1 ? 3 : 2;
    if (s_lay == 2) {                    /* 1152 samples a frame, at any rate */
        *kbps = *ver ? BITRATE[1][bri] : BITRATE_L2[bri];
        return 144000 * *kbps / RATES[*ver][s] + ((p[2] >> 1) & 1);
    }
    *kbps = BITRATE[*ver ? 1 : 0][bri];
    return (*ver ? 72000 : 144000) * *kbps / RATES[*ver][s] + ((p[2] >> 1) & 1);
}

/* ---- side information --------------------------------------------------------- */
static void side_info(const u8 *p, int si)
{
    static u8 side[32 + 4];
    int ch, g, ng = M.lsf ? 1 : 2, i;
    for (i = 0; i < 36; i++) side[i] = i < si ? p[i] : 0;   /* padded, for reading past */
    M.bits = side; M.bitpos = 0; M.bitend = 32 * 8;
    M.main_begin = (int)getbits(M.lsf ? 8 : 9);
    getbits(M.lsf ? (M.nch == 1 ? 1 : 2) : (M.nch == 1 ? 5 : 3));     /* private */
    if (!M.lsf) for (ch = 0; ch < M.nch; ch++) for (i = 0; i < 4; i++) M.scfsi[ch][i] = (int)getbits(1);
    for (g = 0; g < ng; g++)
        for (ch = 0; ch < M.nch; ch++) {
            Granule *G = &M.gr[g][ch];
            G->part23 = (int)getbits(12);
            G->big_values = (int)getbits(9);
            G->global_gain = (int)getbits(8);
            G->sfc = (int)getbits(M.lsf ? 9 : 4);
            G->window_switching = (int)getbits(1);
            if (G->window_switching) {
                G->block_type = (int)getbits(2);
                G->mixed = (int)getbits(1);
                for (i = 0; i < 2; i++) G->table[i] = (int)getbits(5);
                G->table[2] = 0;
                for (i = 0; i < 3; i++) G->subblock_gain[i] = (int)getbits(3);
                G->region0 = G->block_type == 2 && !G->mixed ? 8 : 7;
                G->region1 = 20 - G->region0;
            } else {
                G->block_type = 0; G->mixed = 0;
                for (i = 0; i < 3; i++) G->table[i] = (int)getbits(5);
                for (i = 0; i < 3; i++) G->subblock_gain[i] = 0;
                G->region0 = (int)getbits(4);
                G->region1 = (int)getbits(3);
            }
            G->preflag = M.lsf ? 0 : (int)getbits(1);
            G->sf_scale = (int)getbits(1);
            G->count1table = (int)getbits(1);
            if (G->big_values > 288) G->big_values = 288;
        }
}

/* ---- scalefactors ---------------------------------------------------------------- */
static void scalefactors1(int g, int ch)
{
    static const u8 slen1[16] = { 0,0,0,0,3,1,1,1,2,2,2,3,3,3,4,4 };
    static const u8 slen2[16] = { 0,1,2,3,0,1,2,3,1,2,3,1,2,3,2,3 };
    Granule *G = &M.gr[g][ch];
    int s1 = slen1[G->sfc], s2 = slen2[G->sfc], sfb, w;

    if (G->window_switching && G->block_type == 2) {
        if (G->mixed) {
            for (sfb = 0; sfb < 8; sfb++) M.sf_l[ch][sfb] = (u8)getbits(s1);
            for (sfb = 3; sfb < 6; sfb++) for (w = 0; w < 3; w++) M.sf_s[ch][sfb][w] = (u8)getbits(s1);
        } else
            for (sfb = 0; sfb < 6; sfb++) for (w = 0; w < 3; w++) M.sf_s[ch][sfb][w] = (u8)getbits(s1);
        for (sfb = 6; sfb < 12; sfb++) for (w = 0; w < 3; w++) M.sf_s[ch][sfb][w] = (u8)getbits(s2);
        for (w = 0; w < 3; w++) M.sf_s[ch][12][w] = 0;
        return;
    }
    {
        static const u8 edge[5] = { 0, 6, 11, 16, 21 };
        int k;
        for (k = 0; k < 4; k++) {
            if (g == 1 && M.scfsi[ch][k]) continue;         /* kept from granule 0 */
            for (sfb = edge[k]; sfb < edge[k + 1]; sfb++) M.sf_l[ch][sfb] = (u8)getbits(k < 2 ? s1 : s2);
        }
        M.sf_l[ch][21] = 0;
    }
}

static void scalefactors2(int ch)
{
    Granule *G = &M.gr[0][ch];
    int sfc = G->sfc, slen[4] = { 0, 0, 0, 0 }, tab, bt, part, k, n = 0;
    u8 vals[40];

    if ((M.mode_ext & 1) && ch == 1) {
        int s = sfc >> 1;
        if (s < 180) { slen[0] = s / 36; slen[1] = (s % 36) / 6; slen[2] = (s % 36) % 6; tab = 3; }
        else if (s < 244) { s -= 180; slen[0] = (s & 63) >> 4; slen[1] = (s & 15) >> 2; slen[2] = s & 3; tab = 4; }
        else { s -= 244; slen[0] = s / 3; slen[1] = s % 3; tab = 5; }
    } else if (sfc < 400) {
        slen[0] = (sfc >> 4) / 5; slen[1] = (sfc >> 4) % 5; slen[2] = (sfc & 15) >> 2; slen[3] = sfc & 3; tab = 0;
    } else if (sfc < 500) {
        sfc -= 400; slen[0] = (sfc >> 2) / 5; slen[1] = (sfc >> 2) % 5; slen[2] = sfc & 3; tab = 1;
    } else {
        sfc -= 500; slen[0] = sfc / 3; slen[1] = sfc % 3; tab = 2; G->preflag = 1;
    }
    bt = G->window_switching && G->block_type == 2 ? (G->mixed ? 2 : 1) : 0;
    for (part = 0; part < 4; part++)
        for (k = 0; k < NR_SFB[tab][bt][part] && n < 40; k++)
            vals[n++] = (u8)getbits(slen[part]);
    while (n < 40) vals[n++] = 0;

    n = 0;
    if (bt == 0) {
        for (k = 0; k < 21; k++) M.sf_l[ch][k] = vals[n++];
        M.sf_l[ch][21] = 0;
    } else {
        int sfb = 0, w;
        if (bt == 2) { for (k = 0; k < 6; k++) M.sf_l[ch][k] = vals[n++]; sfb = 3; }
        for (; sfb < 12; sfb++) for (w = 0; w < 3; w++) M.sf_s[ch][sfb][w] = vals[n++];
        for (w = 0; w < 3; w++) M.sf_s[ch][12][w] = 0;
    }
}

/* ---- Huffman, and putting the numbers back to scale ------------------------------ */
static int huff(int t)
{
    const Prefix *hp = &s_hp[t];
    u32 e;
    if (!s_hp_ok[t] || M.bitpos >= M.bitend) return 0;
    e = hp->tab[peekbits(hp->root)];
    if (e & PREFIX_SUB) {
        M.bitpos += (u32)hp->root;
        e = hp->tab[(e >> 8) + peekbits((int)(e & 0x3F))];
    }
    if (!(e & 0x3F)) return 0;                      /* a code that is not one */
    M.bitpos += e & 0x3F;
    return (int)(e >> 8);
}

static const u16 *sfb_long(void)  { return M.lsf ? SFB_LONG2[(M.ver - 1) * 3 + M.sr_index] : SFB_LONG1[M.sr_index]; }
static const u16 *sfb_short(void) { return M.lsf ? SFB_SHORT2[(M.ver - 1) * 3 + M.sr_index] : SFB_SHORT1[M.sr_index]; }

static void huffman(int g, int ch, u32 end, int *is)
{
    Granule *G = &M.gr[g][ch];
    const u16 *bl = sfb_long();
    int r1, r2, i = 0, lim = G->big_values * 2;

    if (G->window_switching && G->block_type == 2) { r1 = 3 * sfb_short()[3]; r2 = 576; }
    else if (G->window_switching) { r1 = bl[8]; r2 = 576; }
    else {
        int a = G->region0 + 1, b = G->region0 + G->region1 + 2;
        r1 = bl[a > 22 ? 22 : a];
        r2 = bl[b > 22 ? 22 : b];
    }
    for (i = 0; i < lim; i += 2) {
        int t = G->table[i < r1 ? 0 : i < r2 ? 1 : 2], v = huff(t), x = v >> 4, y = v & 15, lb = HTAB[t].linbits;
        if (lb && x == 15) x += (int)getbits(lb);
        if (x && getbits(1)) x = -x;
        if (lb && y == 15) y += (int)getbits(lb);
        if (y && getbits(1)) y = -y;
        is[i] = x; is[i + 1] = y;
    }
    /* count1: quads of -1, 0, 1, until this granule's bits run out. */
    while (i + 4 <= 576 && M.bitpos < end) {
        u32 at = M.bitpos;
        int v = huff(32 + G->count1table), k, q[4];
        q[0] = (v >> 3) & 1; q[1] = (v >> 2) & 1; q[2] = (v >> 1) & 1; q[3] = v & 1;
        for (k = 0; k < 4; k++) if (q[k] && getbits(1)) q[k] = -1;
        if (M.bitpos > end) { M.bitpos = at; break; }     /* ran over: not a quad */
        for (k = 0; k < 4; k++) is[i + k] = q[k];
        i += 4;
    }
    M.nz[ch] = i;
    for (; i < 576; i++) is[i] = 0;
    M.bitpos = end;
}

static void requantize(int g, int ch, const int *is)
{
    Granule *G = &M.gr[g][ch];
    const u16 *bl = sfb_long(), *bs = sfb_short();
    float *xr = M.xr[ch];
    int mult = G->sf_scale ? 4 : 2, gg = G->global_gain - 210, i, sfb, w, k;

    if (G->window_switching && G->block_type == 2) {
        int start = 0;
        i = 0;
        if (G->mixed) {
            for (sfb = 0; sfb < 8 && i < 36; sfb++)
                for (; i < bl[sfb + 1]; i++) {
                    int e = gg - mult * (M.sf_l[ch][sfb] + (G->preflag ? PRETAB[sfb] : 0)), a = is[i] < 0 ? -is[i] : is[i];
                    float v = s_pow43[a > 8206 ? 8206 : a] * POW2Q(e);
                    xr[i] = is[i] < 0 ? -v : v;
                }
            start = 3;
        }
        for (sfb = start; sfb < 13; sfb++) {
            int width = bs[sfb + 1] - bs[sfb];
            for (w = 0; w < 3; w++)
                for (k = 0; k < width && i < 576; k++, i++) {
                    int e = gg - 8 * G->subblock_gain[w] - mult * M.sf_s[ch][sfb][w], a = is[i] < 0 ? -is[i] : is[i];
                    float v = s_pow43[a > 8206 ? 8206 : a] * POW2Q(e);
                    xr[i] = is[i] < 0 ? -v : v;
                }
        }
        for (; i < 576; i++) xr[i] = 0;
        return;
    }
    /* Only up to the last line Huffman gave anything for; one scale a band. */
    for (sfb = 0, i = 0; sfb < 22 && i < M.nz[ch]; sfb++) {
        float sc = POW2Q(gg - mult * (M.sf_l[ch][sfb] + (G->preflag ? PRETAB[sfb] : 0)));
        for (; i < bl[sfb + 1] && i < M.nz[ch]; i++) {
            int v = is[i], a;
            if (!v) { xr[i] = 0; continue; }
            a = v < 0 ? -v : v;
            xr[i] = v < 0 ? -(s_pow43[a > 8206 ? 8206 : a] * sc) : s_pow43[a > 8206 ? 8206 : a] * sc;
        }
    }
    for (; i < 576; i++) xr[i] = 0;
}

/* Short blocks come window by window inside each band; the transform wants
 * them subband by subband. */
static void reorder(int g, int ch)
{
    Granule *G = &M.gr[g][ch];
    const u16 *bs = sfb_short();
    float tmp[576], *xr = M.xr[ch];
    int sfb, w, k, i, start = G->mixed ? 3 : 0, at;
    if (!(G->window_switching && G->block_type == 2)) return;
    at = G->mixed ? 36 : 0;
    i = at;
    for (sfb = start; sfb < 13; sfb++) {
        int width = bs[sfb + 1] - bs[sfb];
        for (w = 0; w < 3; w++)
            for (k = 0; k < width; k++)
                tmp[3 * bs[sfb] + w + 3 * k - (G->mixed ? 0 : 0)] = xr[i++];
    }
    for (i = at; i < 576; i++) xr[i] = tmp[i];
}

static void antialias(int g, int ch)
{
    Granule *G = &M.gr[g][ch];
    float *xr = M.xr[ch];
    int sb, i, lim;
    if (G->window_switching && G->block_type == 2 && !G->mixed) return;
    lim = G->window_switching && G->block_type == 2 ? 2 : 32;
    if (M.half && lim > 17) lim = 17;
    if (lim > M.aalim) lim = M.aalim;
    for (sb = 1; sb < lim; sb++)
        for (i = 0; i < 8; i++) {
            float a = xr[18 * sb - 1 - i], b = xr[18 * sb + i];
            xr[18 * sb - 1 - i] = a * s_cs[i] - b * s_ca[i];
            xr[18 * sb + i] = b * s_cs[i] + a * s_ca[i];
        }
}

/* Inverse MDCT, windowed and overlapped: 18 new time samples per subband,
 * added into the shared subband buffer. */
static void imdct(int g, int ch)
{
    Granule *G = &M.gr[g][ch];
    float *xr = M.xr[ch];
    int sb, i, k, w, nsb = M.half ? 16 : 32;

    for (sb = 0; sb < nsb; sb++) {
        float z[36];
        const float *X = xr + 18 * sb;
        if (sb >= M.sblim) {             /* silent: only what overlaps from before */
            for (i = 0; i < 18; i++) {
                float v = M.overlap[ch][sb][i];
                M.overlap[ch][sb][i] = 0;
                if ((sb & 1) && (i & 1)) v = -v;
                M.sb[i][sb] += v;
            }
            continue;
        }
        int bt = G->window_switching && G->mixed && sb < 2 ? 0 : G->block_type;
        if (bt == 2) {
            for (i = 0; i < 36; i++) z[i] = 0;
            for (w = 0; w < 3; w++)
                for (i = 0; i < 12; i++) {
                    float s = 0;
                    for (k = 0; k < 6; k++) s += X[w + 3 * k] * s_imdct_s[k][i];
                    z[6 * w + i + 6] += s * s_win[2][i];
                }
        } else {
            /* The 36-point inverse MDCT is an 18-point DCT-IV read out
             * from its middle, carried on past its ends by the DCT-IV's
             * own symmetries (as in mdct.c): half the multiplications. */
            float u[18];
            for (i = 0; i < 18; i++) {
                const float *c = s_dct4[i];
                float s = 0;
                for (k = 0; k < 18; k++) s += X[k] * c[k];
                u[i] = s;
            }
            for (i = 0; i < 36; i++) {
                int j = i + 9;
                float y = j < 18 ? u[j] : j < 36 ? -u[35 - j] : -u[j - 36];
                z[i] = y * s_win[bt][i];
            }
        }
        for (i = 0; i < 18; i++) {
            float v = z[i] + M.overlap[ch][sb][i];
            M.overlap[ch][sb][i] = z[i + 18];
            if ((sb & 1) && (i & 1)) v = -v;              /* frequency inversion */
            M.sb[i][sb] += v;
        }
    }
}

/* The polyphase synthesis: 32 subband samples -> 32 samples, or with half
 * set, the lower 16 -> 16 samples (every other one). */
static void synth(s16 *out)
{
    int slot, i, k, nsb = M.half ? 16 : 32, step = M.half ? 2 : 1, n = 0;
    float scale = 32768.0f * AUDIO_HEADROOM / (float)M.nch;
    for (slot = 0; slot < 18; slot++) {
        float *v, X[32];
        const float *S = M.sb[slot];
        M.voff = (M.voff - 64) & 1023;
        v = M.v + M.voff;
        /* The matrixing -- 64 sums over k of cos((16 + i)(2k + 1) pi/64) S[k]
         * -- is one 32-point cosine transform X laid out by its symmetries:
         * v[i] is X[16 + i] below 16, nothing at 16, -X[48 - i] up to 47 and
         * -X[i - 48] after.  80 multiplications instead of 2048. */
        for (k = 0; k < nsb; k++) X[k] = S[k];
        for (; k < 32; k++) X[k] = 0;
        dsp_dct2(X, 32);
        for (i = 0; i < 16; i++) v[i] = X[16 + i];
        v[16] = 0;
        for (i = 17; i < 48; i++) v[i] = -X[48 - i];
        for (i = 48; i < 64; i++) v[i] = -X[i - 48];
        /* The history is kept twice over, so the window below reads it in
         * one straight run from voff, with no wrapping round. */
        for (i = 0; i < 64; i++) M.v[M.voff + 1024 + i] = v[i];
        for (k = 0; k < 32; k += step) {
            const float *a = M.v + M.voff + k, *d = SYNTH_D + k;
            float s = 0;
            int o;
            for (i = 0; i < 8; i++, a += 128, d += 64) s += a[0] * d[0] + a[96] * d[32];
            o = dsp_round(s * scale);
            out[n++] = (s16)(o > 32767 ? 32767 : o < -32768 ? -32768 : o);
        }
    }
}

/* ---- layer II ------------------------------------------------------------------------
 * No Huffman codes and no transform: each subband (up to 30 of the 32) is
 * given a few bits a sample by its "allocation", read first, then scale
 * factors for each third of the frame, then twelve groups of three samples
 * a subband, which go straight into the synthesis.  Which allocations a
 * subband may have depends on the subband and on one of five tables,
 * chosen by the rate and the bitrate a channel (ISO 11172-3 tables B.2a-d,
 * and 13818-3's for the lower rates).  Above `bound', joint stereo sends
 * one set of samples for both channels, each with its own scale factors. */
typedef struct { u8 nbal, q[15]; } L2Class;    /* bits in the allocation; the quantisations it picks */
static const L2Class L2C[8] = {
    { 4, { 0,2,4,5,6,7,8,9,10,11,12,13,14,15,16 } },   /* MPEG-1, the lowest subbands */
    { 4, { 0,1,2,3,4,5,6,7,8,9,10,11,12,13,16 } },
    { 3, { 0,1,2,3,4,5,16 } },
    { 2, { 0,1,16 } },
    { 4, { 0,1,3,4,5,6,7,8,9,10,11,12,13,14,15 } },   /* MPEG-1 at low bitrates */
    { 3, { 0,1,3,4,5,6,7 } },
    { 4, { 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14 } },    /* the lower rates (MPEG-2) */
    { 2, { 0,1,3 } },
};
static const u8 L2_TABLE[5][30] = {      /* each subband's class, by table */
    { 0,0,0,1,1,1,1,1,1,1,1,2,2,2,2,2,2,2,2,2,2,2,2,3,3,3,3 },
    { 0,0,0,1,1,1,1,1,1,1,1,2,2,2,2,2,2,2,2,2,2,2,2,3,3,3,3,3,3,3 },
    { 4,4,5,5,5,5,5,5 },
    { 4,4,5,5,5,5,5,5,5,5,5,5 },
    { 6,6,6,6,5,5,5,5,5,5,5,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7 },
};
static const u8  L2_LIMIT[5] = { 27, 30, 8, 12, 30 };
static const u16 L2_LEVELS[17] = { 3,5,7,9,15,31,63,127,255,511,1023,2047,4095,8191,16383,32767,65535 };
static const u8  L2_BITS[17] = { 5,7,3,10,4,5,6,7,8,9,10,11,12,13,14,15,16 };   /* 3, 5 and 9 come three to a code */

/* Three samples of a subband, as fractions of its scale: (2v - (levels-1)) / levels. */
static void l2_samples(int q, float f[3])
{
    int lev = L2_LEVELS[q], k;
    float inv = 1.0f / (float)lev;
    if (q == 0 || q == 1 || q == 3) {
        u32 c = getbits(L2_BITS[q]);
        for (k = 0; k < 3; k++) { int v = (int)(c % (u32)lev); c /= (u32)lev; f[k] = (float)(2 * v - (lev - 1)) * inv; }
    } else
        for (k = 0; k < 3; k++) f[k] = (float)(2 * (int)getbits(L2_BITS[q]) - (lev - 1)) * inv;
}

static int layer2(const u8 *frame, int len)
{
    static u8 alloc[2][32], scfsi[2][32], sfi[2][32][3];
    static u8 p[1729 + 4];                   /* the frame, with the four zero bytes the bit reader looks past */
    int t, lim, bound, ch, sb, gr, nch = M.nch, nsb = M.half ? 16 : 32;

    if (M.lsf) t = 4;
    else {
        int per = M.kbps / nch, rate = RATES[0][M.sr_index];
        t = per <= 48 ? (rate == 32000 ? 3 : 2) : per <= 80 ? 0 : rate == 48000 ? 0 : 1;
    }
    if (len > 1729) return 2;
    for (sb = 0; sb < len; sb++) p[sb] = frame[sb];
    p[len] = p[len + 1] = p[len + 2] = p[len + 3] = 0;
    lim = L2_LIMIT[t];
    bound = nch == 2 && M.mode == 1 ? 4 + M.mode_ext * 4 : lim;
    if (bound > lim) bound = lim;
    M.bits = p;
    M.bitpos = (u32)(4 + (M.crc ? 2 : 0)) * 8;
    M.bitend = (u32)len * 8;

    for (sb = 0; sb < lim; sb++) {
        int nb = L2C[L2_TABLE[t][sb]].nbal;
        if (sb < bound) for (ch = 0; ch < nch; ch++) alloc[ch][sb] = (u8)getbits(nb);
        else alloc[0][sb] = alloc[1][sb] = (u8)getbits(nb);
    }
    for (sb = 0; sb < lim; sb++)
        for (ch = 0; ch < nch; ch++) if (alloc[ch][sb]) scfsi[ch][sb] = (u8)getbits(2);
    for (sb = 0; sb < lim; sb++)
        for (ch = 0; ch < nch; ch++) {
            u8 *f = sfi[ch][sb];
            if (!alloc[ch][sb]) continue;
            switch (scfsi[ch][sb]) {
            case 0: f[0] = (u8)getbits(6); f[1] = (u8)getbits(6); f[2] = (u8)getbits(6); break;
            case 1: f[0] = f[1] = (u8)getbits(6); f[2] = (u8)getbits(6); break;
            case 2: f[0] = f[1] = f[2] = (u8)getbits(6); break;
            default: f[0] = (u8)getbits(6); f[1] = f[2] = (u8)getbits(6); break;
            }
        }

    for (gr = 0; gr < 12; gr++) {
        int part = gr >> 2, slot = (gr % 6) * 3, k;
        for (sb = 0; sb < lim; sb++) {
            const L2Class *c = &L2C[L2_TABLE[t][sb]];
            float f[3];
            for (ch = 0; ch < nch; ch++) {
                float sc;
                if (!alloc[ch][sb]) continue;
                if (ch == 0 || sb < bound) l2_samples(c->q[alloc[ch][sb] - 1], f);   /* above bound: the same samples */
                if (sb >= nsb) continue;         /* read, but above what half rate keeps */
                sc = s_sf2[sfi[ch][sb][part] & 63];
                for (k = 0; k < 3; k++) M.sb[slot + k][sb] += f[k] * sc;
            }
        }
        if (gr % 6 == 5) {
            int i, j;
            synth(M.out + (gr / 6) * (M.half ? 288 : 576));
            for (i = 0; i < 18; i++) for (j = 0; j < 32; j++) M.sb[i][j] = 0;
        }
    }
    M.out_n = 2 * (M.half ? 288 : 576);
    M.out_at = 0;
    return 1;
}

/* ---- a frame ---------------------------------------------------------------------- */
static int skip_id3(AudioIn *in)
{
    const u8 *b = in->buf + in->pos;
    u32 size;
    if (in->len - in->pos < 10) return in->done ? 0 : AUDIO_MORE;
    if (!(b[0] == 'I' && b[1] == 'D' && b[2] == '3')) return 0;
    size = ((u32)(b[6] & 127) << 21) | ((u32)(b[7] & 127) << 14) | ((u32)(b[8] & 127) << 7) | (b[9] & 127);
    size += 10 + ((b[5] & 0x10) ? 10 : 0);
    if (in->len - in->pos < size) {
        if (in->done) { in->pos = in->len; return 0; }
        return AUDIO_MORE;
    }
    in->pos += size;
    return 1;
}

/* Find the next frame that looks real -- and, when the bytes are there, is
 * followed by another of the same kind. */
static int sync(AudioIn *in)
{
    for (;;) {
        int ver, sri, nch, kbps, len, v2, s2, n2, k2;
        const u8 *p;
        if (in->pos >= in->len || in->len - in->pos < 4) return in->done ? AUDIO_END : AUDIO_MORE;
        p = in->buf + in->pos;
        if (p[0] == 'I' && p[1] == 'D' && p[2] == '3') {
            int r = skip_id3(in);
            if (r == AUDIO_MORE) return r;
            if (r || in->buf + in->pos != p) continue;      /* past it, or a tag that ran off the end */
        }
        len = header(p, &ver, &sri, &nch, &kbps);
        if (len >= 21) {
            if (in->len - in->pos >= (u32)len + 4) {
                int lay = s_lay;
                if (header(p + len, &v2, &s2, &n2, &k2) && v2 == ver && s2 == sri && s_lay == lay) { s_lay = lay; return len; }
                s_lay = lay;
            } else if (in->done || M.frame_len) {
                return len;                                 /* the last, or known */
            } else
                return AUDIO_MORE;
        }
        in->pos++;
    }
}

static int frame(AudioIn *in)
{
    int len = sync(in), ver, sri, nch, kbps, si, g, ch, ng, avail;
    const u8 *p;
    static int is[576];
    u32 start;

    if (len <= 0) return len;
    if (in->len - in->pos < (u32)len) return in->done ? AUDIO_END : AUDIO_MORE;
    p = in->buf + in->pos;
    header(p, &ver, &sri, &nch, &kbps);
    M.ver = ver; M.lsf = ver != 0; M.sr_index = sri; M.nch = nch; M.kbps = kbps;
    M.mode = (p[3] >> 6) & 3; M.mode_ext = (p[3] >> 4) & 3;
    M.crc = !(p[1] & 1);
    M.frame_len = len;
    if (s_lay == 2) { in->pos += (u32)len; return layer2(p, len); }
    si = M.lsf ? (nch == 1 ? 9 : 17) : (nch == 1 ? 17 : 32);
    if (4 + (M.crc ? 2 : 0) + si > len) { in->pos += (u32)len; return 2; }   /* skipped */
    side_info(p + 4 + (M.crc ? 2 : 0), si);

    /* The main data: this frame's, after what earlier frames left in the
     * reservoir; where it starts is main_begin bytes back. */
    avail = M.res_len;
    {
        int body = len - 4 - (M.crc ? 2 : 0) - si, k;
        const u8 *b = p + 4 + (M.crc ? 2 : 0) + si;
        if (M.res_len + body > (int)sizeof M.res) {
            int keep = 512;
            for (k = 0; k < keep; k++) M.res[k] = M.res[M.res_len - keep + k];
            M.res_len = keep;
            avail = keep;
        }
        for (k = 0; k < body; k++) M.res[M.res_len + k] = b[k];
        M.res_len += body;
        for (k = 0; k < 4; k++) M.res[M.res_len + k] = 0;
    }
    in->pos += (u32)len;
    for (g = 0; g < 18; g++) for (ch = 0; ch < 32; ch++) M.sb[g][ch] = 0;
    ng = M.lsf ? 1 : 2;
    if (M.main_begin > avail) {                     /* the start of a stream */
        M.out_n = ng * (M.half ? 288 : 576);
        for (g = 0; g < M.out_n; g++) M.out[g] = 0;
        M.out_at = 0;
        return 1;
    }
    M.bits = M.res;
    M.bitend = (u32)M.res_len * 8;
    start = (u32)(avail - M.main_begin) * 8;
    M.bitpos = start;
    for (g = 0; g < ng; g++) {
        for (ch = 0; ch < M.nch; ch++) {
            u32 part_start = M.bitpos;
            if (M.lsf) scalefactors2(ch); else scalefactors1(g, ch);
            huffman(g, ch, part_start + (u32)M.gr[g][ch].part23, is);
            requantize(g, ch, is);
        }
        /* Above the last line with anything in it, in every channel, the
         * spectrum is silent, and so are those subbands after the
         * transform: they need none (but one, which the alias reduction
         * reaches into).  Short blocks move lines about, so not then. */
        {
            int nzmax = 0, shortb = 0, nsb = M.half ? 16 : 32;
            for (ch = 0; ch < M.nch; ch++) {
                if (M.nz[ch] > nzmax) nzmax = M.nz[ch];
                if (M.gr[g][ch].window_switching && M.gr[g][ch].block_type == 2) shortb = 1;
            }
            M.aalim = shortb ? 32 : (nzmax + 17) / 18 + 1;
            M.sblim = M.aalim > nsb ? nsb : M.aalim;
            if (shortb) M.nz[0] = M.nz[1] = 576;
            else M.nz[0] = M.nz[1] = nzmax;
        }
        /* Mid/side back to left/right -- for mono, only their sum counts. */
        if (M.nch == 2 && M.mode == 1 && (M.mode_ext & 2)) {
            int i;
            for (i = 0; i < M.nz[0]; i++) {
                float m = M.xr[0][i], s = M.xr[1][i];
                M.xr[0][i] = (m + s) * 0.70710678f;
                M.xr[1][i] = (m - s) * 0.70710678f;
            }
        }
        for (ch = 0; ch < M.nch; ch++) {
            reorder(g, ch);
            antialias(g, ch);
            imdct(g, ch);
        }
        synth(M.out + g * (M.half ? 288 : 576));
        { int i, j; for (i = 0; i < 18; i++) for (j = 0; j < 32; j++) M.sb[i][j] = 0; }
    }
    M.out_n = ng * (M.half ? 288 : 576);
    M.out_at = 0;
    return 1;
}

static int mp3_decode(Decoder *d, AudioIn *in, s16 *out, int max)
{
    int n = 0;
    (void)d;
    while (n < max) {
        if (M.out_at < M.out_n) {
            while (n < max && M.out_at < M.out_n) out[n++] = M.out[M.out_at++];
            continue;
        }
        {
            int r = frame(in);
            if (r == AUDIO_MORE || r == AUDIO_END) return n ? n : r;
            if (r < 0) return n ? n : AUDIO_BAD;
        }
    }
    return n;
}

int mp3_open(AudioIn *in, Decoder *d)
{
    int r, ver, sri, nch, kbps;
    u32 keep;
    tables();
    {
        int i;
        for (i = 0; i < (int)sizeof M; i++) ((u8 *)&M)[i] = 0;
    }
    keep = in->pos;
    r = sync(in);
    if (r <= 0) { if (r == AUDIO_MORE) in->pos = keep; return r == AUDIO_MORE ? AUDIO_MORE : 0; }
    header(in->buf + in->pos, &ver, &sri, &nch, &kbps);
    M.frame_len = 0;
    M.half = RATES[ver][sri] >= 32000;
    d->kind = s_lay == 2 ? "MP2" : "MP3";
    d->rate = RATES[ver][sri] / (M.half ? 2 : 1);
    d->channels = nch;
    d->kbps = kbps;
    d->decode = mp3_decode;
    return 1;
}
