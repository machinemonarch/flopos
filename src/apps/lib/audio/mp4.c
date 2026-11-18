/* ===========================================================================
 * mp4.c -- AAC in MP4 files (.m4a, .mp4), and the box reading HLS shares.
 *
 * An MP4 file (ISO 14496-12) is boxes: a size, a four-letter type, and the
 * body, some of them boxes in turn.  moov is the index -- each track, what
 * its sound is (an esds holding AAC's AudioSpecificConfig), and where each
 * sample lies: their sizes (stsz), how many to a chunk (stsc), and where
 * each chunk starts (stco, co64).  mdat holds the samples themselves.
 *
 * Played as the bytes arrive: the index is read when it is whole, then the
 * samples taken in order, skipping what lies between (another track's, in
 * a video), each handed to aac.c as a bare block.  So the index must come
 * before the samples ("fast start", as podcasts and shops send them); a
 * file with it at the end plays only when all of it is in hand.
 *
 * The index is copied out -- two bytes a sample, four a chunk -- into
 * memory the app lends (mp4_room), or, without that, into a small table
 * here that holds a few minutes.  Only the first AAC track is played.
 * ===========================================================================
 */
#include "decode.h"

void *memcpy(void *d, const void *s, u32 n);

static u32 be32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static int is4(const u8 *p, const char *t) { return p[0] == t[0] && p[1] == t[1] && p[2] == t[2] && p[3] == t[3]; }

/* ---- boxes ------------------------------------------------------------------- */
const u8 *mp4_box(const u8 *p, u32 n, u32 *at, const char *t, u32 *len, const u8 **type)
{
    while (*at + 8 <= n) {
        u32 i = *at, sz = be32(p + i), h = 8;
        if (sz == 1) {                               /* a 64-bit size */
            if (i + 16 > n || be32(p + i + 8)) return 0;
            sz = be32(p + i + 12); h = 16;
        } else if (sz == 0) sz = n - i;              /* to the end */
        if (sz < h || sz > n - i) return 0;
        *at = i + sz;
        if (!t || is4(p + i + 4, t)) {
            *len = sz - h;
            if (type) *type = p + i + 4;
            return p + i + h;
        }
    }
    return 0;
}

const u8 *mp4_in(const u8 *p, u32 n, const char *t, u32 *len)
{
    u32 at = 0;
    return mp4_box(p, n, &at, t, len, 0);
}

/* An MPEG-4 descriptor's length: up to four bytes of seven bits. */
static u32 desc_len(const u8 *p, u32 n, u32 *at)
{
    u32 v = 0, k;
    for (k = 0; k < 4 && *at < n; k++) {
        u8 b = p[(*at)++];
        v = v << 7 | (b & 127);
        if (!(b & 128)) return v;
    }
    return 0xFFFFFFFFu;
}

static u32 bits_at(const u8 *c, u32 *pos, int k)
{
    u32 v = 0;
    for (; k > 0; k--, (*pos)++) v = v << 1 | ((c[*pos >> 3] >> (7 - (*pos & 7))) & 1);
    return v;
}

/* esds: mp3, or AAC and its AudioSpecificConfig.  HE-AAC gives its LC
 * core (the treble it adds is not played: see aac.c). */
static int esds(const u8 *p, u32 n, Mp4Sound *s)
{
    u32 at = 4, l, oti, pos = 0, aot, sfi, ch;
    u8 f;
    const u8 *c;
    if (at >= n || p[at++] != 3) return 0;          /* ES_Descriptor */
    if ((l = desc_len(p, n, &at)) > n - at || at + 3 > n) return 0;
    f = p[at + 2]; at += 3;
    if (f & 0x80) at += 2;
    if (f & 0x40) { if (at >= n) return 0; at += 1 + p[at]; }
    if (f & 0x20) at += 2;
    if (at >= n || p[at++] != 4) return 0;          /* DecoderConfigDescriptor */
    if ((l = desc_len(p, n, &at)) > n - at || l < 13) return 0;
    oti = p[at];
    if (oti == 0x69 || oti == 0x6B) { s->mp3 = 1; return 1; }
    if (oti != 0x40 && oti != 0x66 && oti != 0x67) return 0;
    at += 13;
    if (at >= n || p[at++] != 5) return 0;          /* DecoderSpecificInfo */
    if ((l = desc_len(p, n, &at)) > n - at || l < 2) return 0;
    c = p + at;
    aot = bits_at(c, &pos, 5);
    if (aot == 31) { if (l < 3) return 0; aot = 32 + bits_at(c, &pos, 6); }
    if ((sfi = bits_at(c, &pos, 4)) == 15) return 0;   /* a rate ADTS and aac.c cannot say */
    ch = bits_at(c, &pos, 4);
    if (aot == 5 || aot == 29) {                    /* HE-AAC: its core underneath */
        if (l < 4) return 0;
        if (bits_at(c, &pos, 4) == 15) { if (l < 7) return 0; (void)bits_at(c, &pos, 24); }
        aot = bits_at(c, &pos, 5);
    }
    if (aot < 1 || aot > 4 || ch < 1 || ch > 7 || sfi >= 12) return 0;
    s->mp3 = 0; s->prof = (int)aot; s->sfi = (int)sfi; s->ch = (int)ch;
    return 1;
}

int mp4_sound(const u8 *trak, u32 n, Mp4Sound *s)
{
    u32 l, k, dl, sl, el;
    const u8 *mdia = mp4_in(trak, n, "mdia", &l), *b, *e;
    if (!mdia) return 0;
    b = mp4_in(mdia, l, "hdlr", &k);
    if (!b || k < 12 || !is4(b + 8, "soun")) return 0;
    b = mp4_in(trak, n, "tkhd", &k);
    if (!b || k < 24) return 0;
    s->track = be32(b + (b[0] == 1 ? 20 : 12));
    if (!(b = mp4_in(mdia, l, "minf", &dl)) || !(b = mp4_in(b, dl, "stbl", &dl)) ||
        !(b = mp4_in(b, dl, "stsd", &dl)) || dl < 8 + 8 + 28)
        return 0;
    b += 8; dl -= 8;                                 /* the first sample entry */
    sl = be32(b);
    if (sl < 8 + 28 || sl > dl) return 0;
    if (is4(b + 4, ".mp3")) { s->mp3 = 1; return 1; }
    if (!is4(b + 4, "mp4a")) return 0;
    k = 28 + (b[16] == 0 && b[17] == 1 ? 16 : b[16] == 0 && b[17] == 2 ? 36 : 0);   /* the entry's version */
    if (8 + k > sl) return 0;
    e = mp4_in(b + 8 + k, sl - 8 - k, "esds", &el);
    if (!e) {                                        /* QuickTime's way: inside a wave box */
        const u8 *w = mp4_in(b + 8 + k, sl - 8 - k, "wave", &dl);
        e = w ? mp4_in(w, dl, "esds", &el) : 0;
    }
    return e && esds(e, el, s);
}

/* ---- a file, as it arrives ---------------------------------------------------- */
#define OWN_MAX (24u << 10)                  /* the table here: about four minutes of samples */
static u8  s_own[OWN_MAX];
static u8 *s_lent;
static u32 s_lent_cap;

void mp4_room(u8 *room, u32 cap) { s_lent = room; s_lent_cap = cap; }

static struct {
    u8  *mem;                            /* where the index went            */
    u16 *size;                           /* each sample's length            */
    u32 *chunk;                          /* each chunk's place in the file  */
    u32 *run;                            /* stsc: first chunk (from 1), samples a chunk */
    u32 nsamp, nchunk, nrun, fixed;      /* fixed: every sample this long, or 0 */
    u32 abs;                             /* the file's offset of in->pos    */
    u32 i, c, in_c, left_c, at;          /* the next sample, its chunk, which in the chunk, how many left, where */
} X;

/* The sample table of the AAC track, copied out.  0 if there is none or it
 * will not fit. */
static int take_index(const u8 *moov, u32 n, Mp4Sound *s)
{
    u32 at = 0, tl, l, k, cap, need, j;
    const u8 *trak, *st, *b;
    while ((trak = mp4_box(moov, n, &at, "trak", &tl, 0))) {
        int is64 = 0;
        if (!mp4_sound(trak, tl, s) || s->mp3) continue;
        if (!(st = mp4_in(trak, tl, "mdia", &l)) || !(st = mp4_in(st, l, "minf", &l)) || !(st = mp4_in(st, l, "stbl", &l)))
            continue;
        {
            u32 zl, cl, ol;
            const u8 *z = mp4_in(st, l, "stsz", &zl), *c = mp4_in(st, l, "stsc", &cl), *o = mp4_in(st, l, "stco", &ol);
            if (!o) { o = mp4_in(st, l, "co64", &ol); is64 = 1; }
            if (!z || !c || !o || zl < 12 || cl < 8 || ol < 8) continue;
            X.fixed = be32(z + 4);
            X.nsamp = be32(z + 8);
            X.nrun = be32(c + 4);
            X.nchunk = be32(o + 4);
            if (X.nrun > (cl - 8) / 12 || X.nchunk > (ol - 8) / (is64 ? 8 : 4) || !X.nrun || !X.nchunk) continue;
            if (!X.fixed && X.nsamp > (zl - 12) / 4) continue;
            X.mem = s_lent ? s_lent : s_own;
            cap = s_lent ? s_lent_cap : OWN_MAX;
            need = (X.fixed ? 0 : X.nsamp * 2) + X.nchunk * 4 + X.nrun * 8;
            if (X.nsamp > 0x10000000u || need > cap) return 0;
            X.chunk = (u32 *)X.mem;
            X.run = X.chunk + X.nchunk;
            X.size = (u16 *)(X.run + 2 * X.nrun);
            for (j = 0; j < X.nchunk; j++) {
                b = o + 8 + j * (is64 ? 8 : 4);
                if (is64 && be32(b)) return 0;       /* past 4 GB */
                X.chunk[j] = be32(b + (is64 ? 4 : 0));
            }
            for (j = 0; j < X.nrun; j++) { X.run[2 * j] = be32(c + 8 + j * 12); X.run[2 * j + 1] = be32(c + 12 + j * 12); }
            if (!X.fixed)
                for (j = 0; j < X.nsamp; j++) { k = be32(z + 12 + j * 4); X.size[j] = (u16)(k > 8192 ? 0 : k); }
            else if (X.fixed > 8192) return 0;
            return 1;
        }
    }
    return 0;
}

/* Samples in chunk c (from 0), by the runs. */
static u32 per_chunk(u32 c)
{
    u32 r, n = 0;
    for (r = 0; r < X.nrun && X.run[2 * r] <= c + 1; r++) n = X.run[2 * r + 1];
    return n;
}

/* Moves to the next sample: X.at is where it lies. */
static void next_place(void)
{
    while (X.left_c == 0) {
        if (++X.c >= X.nchunk) { X.i = X.nsamp; return; }
        X.left_c = per_chunk(X.c);
        X.at = X.chunk[X.c];
    }
}

static void begin_samples(void)
{
    X.i = 0; X.c = 0;
    X.left_c = per_chunk(0);
    X.at = X.chunk[0];
    next_place();
}

/* Steps in->pos on by k bytes (they are there). */
static void step(AudioIn *in, u32 k) { in->pos += k; X.abs += k; }

/* Looks through the boxes from in->pos, using none of them, for the
 * index.  1 with in->pos moved to where the samples are to be looked for
 * (just past the index, or the start of mdat if that came first), 0 if
 * there is no index to be had, or AUDIO_MORE. */
static int find_index(AudioIn *in, Mp4Sound *s)
{
    u32 at = 0, n = in->len - in->pos;
    const u8 *p = in->buf + in->pos;
    for (;;) {
        u32 sz, h = 8;
        if (n - at < 16 && !in->done) return AUDIO_MORE;
        if (n - at < 8) return 0;
        sz = be32(p + at);
        if (sz == 1) { if (n - at < 16 || be32(p + at + 8)) return 0; sz = be32(p + at + 12); h = 16; }
        else if (sz == 0) sz = 0xFFFFFFFFu;          /* to the end of the file */
        if (sz < h) return 0;
        if (is4(p + at + 4, "moov")) {
            if (sz > n - at) return in->done ? 0 : AUDIO_MORE;
            if (!take_index(p + at + h, sz - h, s)) return 0;
            in->pos += at + sz; X.abs = at + sz;
            return 1;
        }
        if (is4(p + at + 4, "mdat")) {
            /* The samples first: the index is after them, so all of it must be here. */
            u32 k = at, l;
            const u8 *t, *m;
            if (!in->done) return 0;
            while ((m = mp4_box(p, n, &k, 0, &l, &t)))
                if (is4(t, "moov")) {
                    if (!take_index(m, l, s)) return 0;
                    in->pos += at; X.abs = at;
                    return 1;
                }
            return 0;
        }
        if (sz > n - at) return in->done ? 0 : AUDIO_MORE;   /* something else first, not all here yet */
        at += sz;
    }
}

static int mp4_decode(Decoder *d, AudioIn *in, s16 *out, int max)
{
    int n = 0;
    (void)d;
    while (n < max) {
        u32 len, avail;
        int k = aac_take(out + n, max - n);
        if (k) { n += k; continue; }
        if (X.i >= X.nsamp) return n ? n : AUDIO_END;
        len = X.fixed ? X.fixed : X.size[X.i];
        if (X.at < X.abs) {                          /* behind us: not to be had */
            X.at += len; X.i++; X.left_c--; next_place();
            continue;
        }
        avail = in->len - in->pos;
        if (X.at > X.abs) {                          /* pass over what lies between */
            u32 gap = X.at - X.abs;
            step(in, gap < avail ? gap : avail);
            if (gap > avail) return n ? n : (in->done ? AUDIO_END : AUDIO_MORE);
            avail = in->len - in->pos;
        }
        if (len > avail) return n ? n : (in->done ? AUDIO_END : AUDIO_MORE);
        if (len) (void)aac_raw(in->buf + in->pos, len);
        step(in, len);
        X.at += len; X.i++; X.left_c--;
        next_place();
    }
    return n;
}

/* An MP4 file starting at in->pos: ready once its index is read. */
int mp4_open(AudioIn *in, Decoder *d)
{
    Mp4Sound s;
    int r = find_index(in, &s);
    if (r != 1) return r;
    if (!aac_raw_open(s.sfi, s.ch, d)) return 0;
    begin_samples();
    d->kind = "AAC (MP4)";
    d->decode = mp4_decode;
    return 1;
}
