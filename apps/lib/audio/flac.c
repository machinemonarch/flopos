/* ===========================================================================
 * flac.c -- FLAC, into mono samples (the FLAC format specification).
 *
 * A FLAC file is "fLaC", some metadata blocks -- the first, STREAMINFO,
 * says the rate, channels and bits a sample; the rest (tags, pictures) are
 * passed over -- and then frames.  A frame is a header, one subframe for
 * each channel, and a checksum.  A subframe is its channel's samples
 * predicted from the ones before (by a fixed polynomial, or by a set of
 * coefficients the encoder chose), plus what the prediction got wrong,
 * Rice coded.  Stereo may come as left and side, side and right, or mid
 * and side, and is put back together first.  Nothing is lost: what comes
 * out is what went in, mixed down to mono, 16 bits.
 *
 * Frames say nothing of their length, so one is decoded from wherever the
 * bytes are, and if they run out half way it is tried again when more have
 * come.  Every frame's checksums are checked; that is also how the next
 * frame is found in a station joined part way.  Ogg FLAC (how stations
 * usually send it) is unwrapped in vorbis.c, which hands its frames here.
 * ===========================================================================
 */
#include "decode.h"

typedef long long s64;

#define MAXBS 16384                      /* samples a frame; the streamable
                                            subset never goes past it      */
static struct {
    int rate, ch, bps;                   /* STREAMINFO's, for frames that
                                            refer to it                    */
    int more_meta;                       /* another metadata block follows */
    u32 skip;                            /* metadata bytes still to pass   */
    s16 out[MAXBS];
    int out_n, out_at;
} F;

static s32 s_a[MAXBS], s_b[MAXBS];       /* two channels' worth            */

/* ---- checksums --------------------------------------------------------------- */
static u8  s_crc8[256];
static u16 s_crc16[256];

static void crc_tables(void)
{
    int i, j;
    if (s_crc16[1]) return;
    for (i = 0; i < 256; i++) {
        u32 c8 = (u32)i, c16 = (u32)i << 8;
        for (j = 0; j < 8; j++) {
            c8 = (c8 & 0x80) ? (c8 << 1) ^ 0x07 : c8 << 1;
            c16 = (c16 & 0x8000) ? (c16 << 1) ^ 0x8005 : c16 << 1;
        }
        s_crc8[i] = (u8)c8;
        s_crc16[i] = (u16)c16;
    }
}

static u32 crc8(const u8 *p, u32 n)
{
    u32 c = 0;
    while (n--) c = s_crc8[c ^ *p++];
    return c;
}

static u32 crc16(const u8 *p, u32 n)
{
    u32 c = 0;
    while (n--) c = ((c << 8) ^ s_crc16[((c >> 8) ^ *p++) & 0xFF]) & 0xFFFF;
    return c;
}

/* ---- reading bits, most significant first -------------------------------------- */
static const u8 *s_p;
static u32 s_bit, s_end;                 /* in bits                         */
static int s_eof;                        /* ran past the bytes there are    */

static u32 gb(int n)
{
    u32 v = 0;
    if (s_bit + (u32)n > s_end) { s_eof = 1; s_bit = s_end; return 0; }
    while (n > 0) {
        int off = (int)(s_bit & 7), avail = 8 - off, take = n < avail ? n : avail;
        v = (v << take) | (((u32)s_p[s_bit >> 3] >> (avail - take)) & ((1u << take) - 1));
        n -= take;
        s_bit += (u32)take;
    }
    return v;
}

static s32 sgb(int n)
{
    u32 v = gb(n), top;
    if (n >= 32) return (s32)v;
    top = 1u << (n - 1);
    return (s32)(v ^ top) - (s32)top;              /* sign-extended            */
}

/* How many 0 bits before the next 1. */
static u32 unary(void)
{
    u32 q = 0;
    for (;;) {
        u32 byte;
        if (s_bit >= s_end) { s_eof = 1; return 0; }
        byte = ((u32)s_p[s_bit >> 3] << (s_bit & 7)) & 0xFF;
        if (!byte) { q += 8 - (s_bit & 7); s_bit = (s_bit | 7) + 1; continue; }
        while (!(byte & 0x80)) { byte <<= 1; q++; s_bit++; }
        s_bit++;
        return q;
    }
}

/* ---- the frame header -------------------------------------------------------------- */
typedef struct { int bs, rate, ch, assign, bps, len; } Hdr;

static const int s_rates[12] = { 0, 88200, 176400, 192000, 8000, 16000, 22050, 24000, 32000, 44100, 48000, 96000 };
static const int s_sizes[8] = { 0, 8, 12, 0, 16, 20, 24, 0 };

/* 1 and h filled in; 0 if this is not a frame header; -1 if the bytes
 * stop before the header does. */
static int header(const u8 *p, u32 len, Hdr *h)
{
    u32 x, bsc, src, chc, ssc;
    int extra = 0;
    s_p = p; s_bit = 0; s_end = len * 8; s_eof = 0;
    if (gb(15) != 0x7FFC) return s_eof ? -1 : 0;
    gb(1);                                         /* fixed or variable sizes */
    bsc = gb(4); src = gb(4); chc = gb(4); ssc = gb(3);
    if (gb(1) || !bsc || src == 15 || chc > 10 || ssc == 3 || ssc == 7) return s_eof ? -1 : 0;
    x = gb(8);                                     /* the frame's number, UTF-8 style */
    if (x & 0x80) {
        if      ((x & 0xE0) == 0xC0) extra = 1;
        else if ((x & 0xF0) == 0xE0) extra = 2;
        else if ((x & 0xF8) == 0xF0) extra = 3;
        else if ((x & 0xFC) == 0xF8) extra = 4;
        else if ((x & 0xFE) == 0xFC) extra = 5;
        else if (x == 0xFE)          extra = 6;
        else return 0;
    }
    while (extra--) if ((gb(8) & 0xC0) != 0x80) return s_eof ? -1 : 0;
    h->bs = bsc == 1 ? 192 : bsc <= 5 ? 576 << (bsc - 2) : bsc == 6 ? (int)gb(8) + 1 :
            bsc == 7 ? (int)gb(16) + 1 : 256 << (bsc - 8);
    h->rate = src < 12 ? s_rates[src] : src == 12 ? (int)gb(8) * 1000 : src == 13 ? (int)gb(16) : (int)gb(16) * 10;
    if (!src) h->rate = F.rate;
    h->assign = (int)chc;
    h->ch = chc < 8 ? (int)chc + 1 : 2;
    h->bps = ssc ? s_sizes[ssc] : F.bps;
    if (s_eof) return -1;
    h->len = (int)(s_bit >> 3);
    if ((u32)h->len >= len) return -1;
    if (crc8(p, (u32)h->len) != p[h->len]) return 0;
    h->len++;
    if (h->bs > MAXBS || h->bps < 4 || h->bps > 24 || h->rate <= 0) return 0;
    return 1;
}

/* ---- subframes ------------------------------------------------------------------- */
static int residual(s32 *s, int bs, int order)
{
    int method = (int)gb(2), pbits = method == 0 ? 4 : method == 1 ? 5 : 0, porder, parts, p, i = order;
    if (!pbits) return 0;
    porder = (int)gb(4);
    parts = 1 << porder;
    if ((bs >> porder) < order || (bs & (parts - 1))) return 0;
    for (p = 0; p < parts; p++) {
        int n = (bs >> porder) - (p ? 0 : order), k = (int)gb(pbits), j;
        if (k == (1 << pbits) - 1) {               /* this part is sent plain */
            int raw = (int)gb(5);
            for (j = 0; j < n; j++) s[i++] = raw ? sgb(raw) : 0;
        } else
            for (j = 0; j < n; j++) {
                u32 v = (unary() << k) | gb(k);
                s[i++] = (s32)(v >> 1) ^ -(s32)(v & 1);
            }
        if (s_eof) return 0;
    }
    return 1;
}

static int subframe(s32 *s, int bs, int bps)
{
    u32 type;
    int wasted = 0, order, i, j;
    if (gb(1)) return 0;
    type = gb(6);
    if (gb(1)) { wasted = 1; while (!gb(1)) { if (s_eof) return 0; wasted++; } }
    bps -= wasted;
    if (bps < 1) return 0;

    if (type == 0) {                               /* one value throughout    */
        s32 v = sgb(bps);
        for (i = 0; i < bs; i++) s[i] = v;
    } else if (type == 1) {                        /* the samples as they are */
        for (i = 0; i < bs; i++) s[i] = sgb(bps);
    } else if (type >= 8 && type <= 12) {          /* a fixed polynomial      */
        order = (int)type - 8;
        if (order > bs) return 0;
        for (i = 0; i < order; i++) s[i] = sgb(bps);
        if (!residual(s, bs, order)) return 0;
        switch (order) {
        case 1: for (i = 1; i < bs; i++) s[i] += s[i - 1]; break;
        case 2: for (i = 2; i < bs; i++) s[i] += 2 * s[i - 1] - s[i - 2]; break;
        case 3: for (i = 3; i < bs; i++) s[i] += 3 * (s[i - 1] - s[i - 2]) + s[i - 3]; break;
        case 4: for (i = 4; i < bs; i++) s[i] += 4 * (s[i - 1] + s[i - 3]) - 6 * s[i - 2] - s[i - 4]; break;
        }
    } else if (type >= 32) {                       /* the encoder's coefficients */
        s32 c[32];
        int prec, shift;
        order = (int)type - 31;
        if (order > bs) return 0;
        for (i = 0; i < order; i++) s[i] = sgb(bps);
        prec = (int)gb(4) + 1;
        shift = sgb(5);
        if (prec == 16 || shift < 0) return 0;
        for (i = 0; i < order; i++) c[i] = sgb(prec);
        if (!residual(s, bs, order)) return 0;
        if (bps + prec + 5 <= 32)                  /* the sum fits 32 bits    */
            for (i = order; i < bs; i++) {
                s32 acc = 0;
                const s32 *h = s + i - 1;
                for (j = 0; j < order; j++) acc += c[j] * h[-j];
                s[i] += acc >> shift;
            }
        else
            for (i = order; i < bs; i++) {
                s64 acc = 0;
                const s32 *h = s + i - 1;
                for (j = 0; j < order; j++) acc += (s64)c[j] * h[-j];
                s[i] += (s32)(acc >> shift);
            }
    } else return 0;

    if (wasted) for (i = 0; i < bs; i++) s[i] = (s32)((u32)s[i] << wasted);
    return !s_eof;
}

/* ---- a frame -------------------------------------------------------------------------
 * Its samples (mono, 16 bits) into out: how many; 0 if it is not a good
 * frame; -1 if the bytes stop part way.  *used: its length. */
static int frame(const u8 *p, u32 len, s16 *out, u32 *used)
{
    Hdr h;
    int r = header(p, len, &h), c, i, side;
    if (r != 1) return r;
    s_bit = (u32)h.len * 8;
    for (c = 0; c < h.ch; c++) {
        side = (h.assign == 8 && c == 1) || (h.assign == 9 && c == 0) || (h.assign == 10 && c == 1);
        if (!subframe(c ? s_b : s_a, h.bs, h.bps + side)) return s_eof ? -1 : 0;
        if (c && h.assign < 8) for (i = 0; i < h.bs; i++) s_a[i] += s_b[i];
    }
    s_bit = (s_bit + 7) & ~7u;
    if ((s_bit >> 3) + 2 > len) return -1;
    i = (int)(s_bit >> 3);
    if (crc16(p, (u32)i) != ((u32)p[i] << 8 | p[i + 1])) return 0;
    *used = (u32)i + 2;

    /* left and right, added */
    if (h.assign == 8)      for (i = 0; i < h.bs; i++) s_a[i] = 2 * s_a[i] - s_b[i];
    else if (h.assign == 9) for (i = 0; i < h.bs; i++) s_a[i] = s_a[i] + 2 * s_b[i];
    else if (h.assign == 10)
        for (i = 0; i < h.bs; i++) {
            s32 m = (s32)((u32)s_a[i] << 1) | (s_b[i] & 1);
            s_a[i] = ((m + s_b[i]) >> 1) + ((m - s_b[i]) >> 1);
        }
    for (i = 0; i < h.bs; i++) {
        s32 v = s_a[i] / h.ch;
        out[i] = (s16)(h.bps > 16 ? v >> (h.bps - 16) : v * (1 << (16 - h.bps)));
    }
    return h.bs;
}

/* ---- the file, or a station --------------------------------------------------------- */
static int flac_decode(Decoder *d, AudioIn *in, s16 *out, int max)
{
    int n = 0;
    (void)d;
    while (n < max) {
        u32 a = in->len - in->pos, used = 0;
        int r;
        if (F.out_at < F.out_n) {
            while (n < max && F.out_at < F.out_n) out[n++] = F.out[F.out_at++];
            continue;
        }
        if (F.skip) {                              /* tags, pictures, ...     */
            u32 t = a < F.skip ? a : F.skip;
            in->pos += t; F.skip -= t;
            if (F.skip) return n ? n : in->done ? AUDIO_END : AUDIO_MORE;
            continue;
        }
        if (F.more_meta) {
            const u8 *b = in->buf + in->pos;
            if (a < 4) return n ? n : in->done ? AUDIO_END : AUDIO_MORE;
            F.more_meta = !(b[0] & 0x80);
            F.skip = (u32)b[1] << 16 | (u32)b[2] << 8 | b[3];
            in->pos += 4;
            continue;
        }
        if (a < 2) return n ? n : in->done ? AUDIO_END : AUDIO_MORE;
        r = frame(in->buf + in->pos, a, F.out, &used);
        if (r > 0) { in->pos += used; F.out_n = r; F.out_at = 0; continue; }
        if (r < 0 && !in->done && a < 128 * 1024) return n ? n : AUDIO_MORE;
        /* Not a frame here, or one cut off: on to the next place one could start. */
        in->pos++;
        while (in->pos + 1 < in->len && !(in->buf[in->pos] == 0xFF && (in->buf[in->pos + 1] & 0xFE) == 0xF8))
            in->pos++;
        if (in->done && in->pos + 1 >= in->len) in->pos = in->len;
    }
    return n;
}

int flac_info(const u8 *si, int *channels)
{
    crc_tables();
    F.rate = (int)((u32)si[10] << 12 | (u32)si[11] << 4 | si[12] >> 4);
    F.ch = ((si[12] >> 1) & 7) + 1;
    F.bps = ((si[12] & 1) << 4 | si[13] >> 4) + 1;
    F.out_n = F.out_at = 0;
    *channels = F.ch;
    return F.bps >= 4 && F.bps <= 24 ? F.rate : 0;
}

int flac_one(const u8 *p, u32 len, s16 *out)
{
    u32 used;
    int r = frame(p, len, out, &used);
    return r > 0 ? r : 0;
}

/* "fLaC" and STREAMINFO; or, from a station that is already under way,
 * the first frame header to be found, which says enough itself. */
int flac_open(AudioIn *in, Decoder *d)
{
    const u8 *b = in->buf + in->pos;
    u32 a = in->len - in->pos, i;
    crc_tables();
    F.skip = 0; F.out_n = F.out_at = 0;
    if (a >= 4 && b[0] == 'f' && b[1] == 'L' && b[2] == 'a' && b[3] == 'C') {
        if (a < 8 + 34) return in->done ? 0 : AUDIO_MORE;
        if ((b[4] & 0x7F) != 0 || !flac_info(b + 8, &d->channels)) return 0;
        d->kbps = 0;
        if (in->done && !(b[8 + 13] & 0x0F)) {     /* a whole file: its size and length */
            u32 secs = ((u32)b[8 + 14] << 24 | (u32)b[8 + 15] << 16 | (u32)b[8 + 16] << 8 | b[8 + 17]) / (u32)F.rate;
            if (secs) d->kbps = (int)(a / 125 / secs);
        }
        in->pos += 4;
        F.more_meta = 1;
    } else {
        Hdr h;
        F.rate = 0; F.bps = 0;
        for (i = 0; i + 16 <= a; i++)
            if (b[i] == 0xFF && (b[i + 1] & 0xFE) == 0xF8 && header(b + i, a - i, &h) == 1) break;
        if (i + 16 > a) return in->done || a > 16384 ? 0 : AUDIO_MORE;
        F.rate = h.rate; F.ch = h.ch; F.bps = h.bps;
        d->kbps = 0;
        in->pos += i;
        F.more_meta = 0;
    }
    d->kind = "FLAC";
    d->rate = F.rate;
    d->channels = F.ch;
    d->decode = flac_decode;
    return 1;
}
