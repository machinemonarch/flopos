/* ===========================================================================
 * vorbis.c -- Ogg Vorbis, into mono samples (the Vorbis I specification).
 *
 * An Ogg file is pages, each carrying pieces of packets.  A Vorbis stream
 * starts with three header packets -- what it is (rate, channels, block
 * sizes), its comments, and its "setup": the codebooks every later packet
 * is read with, and how floors, residues and channel coupling are put
 * together.  Every packet after that is one block of audio:
 *
 *   for each channel, the floor (a coarse spectral envelope, as points
 *   joined by lines) and the residue (the fine detail, vector-quantised);
 *   coupled channels uncoupled; floor times residue gives the spectrum;
 *   then an inverse MDCT, a window, and overlap with the block before.
 *
 * Mono, by adding the channels' spectra before the transform -- it is
 * linear, so that is the same as mixing afterwards, and it runs once.  At
 * 44.1 and 48 kHz it works at half rate: the transform is half the size,
 * on the lower half of the spectrum, giving 22 or 24 kHz.  The transform is
 * in mdct.c; everything else is here, from the specification.  A new
 * stream starting part way (an Icecast station changing track) starts the
 * decoder again.  Floor type 0, which no encoder has used since 2000, is
 * not read.  Ogg can also carry FLAC (most FLAC stations send it so); then
 * the packets are handed to flac.c instead.
 * ===========================================================================
 */
#include "decode.h"
#include "mdct.h"

void *memcpy(void *d, const void *s, u32 n);

#define MAXCH 8

/* ---- memory: set up once per stream, out of one pool ------------------------- */
static u8  s_pool[960u << 10];             /* the largest setups seen need under 512 KB */
static u32 s_pool_used;
static void *take(u32 n)
{
    void *p;
    if (n > sizeof s_pool - s_pool_used) return 0;
    n = (n + 7) & ~7u;
    if (n > sizeof s_pool - s_pool_used) return 0;
    p = s_pool + s_pool_used;
    s_pool_used += n;
    return p;
}

/* ---- reading bits, least significant first ------------------------------------- */
static const u8 *s_bp;
static u32 s_bpos, s_bend;
static int s_eop;                                  /* ran off the packet's end */

static u32 rb(int n)
{
    u32 v = 0;
    int i;
    for (i = 0; i < n; i++) {
        if (s_bpos >= s_bend) { s_eop = 1; return 0; }
        v |= (u32)((s_bp[s_bpos >> 3] >> (s_bpos & 7)) & 1) << i;
        s_bpos++;
    }
    return v;
}

static int ilog(u32 v) { int n = 0; while (v) { n++; v >>= 1; } return n; }

static double fexp(double x)                       /* small x only            */
{
    double s = 1.0, t = 1.0;
    int k;
    for (k = 1; k < 30; k++) { t *= x / k; s += t; }
    return s;
}

static float unpack_float(u32 x)
{
    double m = (double)(x & 0x1FFFFF);
    int e = (int)((x & 0x7FE00000) >> 21) - 788;
    if (x & 0x80000000) m = -m;
    while (e > 0) { m *= 2.0; e--; }
    while (e < 0) { m *= 0.5; e++; }
    return (float)m;
}

static double fsin(double x) { double r; __asm__("fsin" : "=t"(r) : "0"(x)); return r; }

/* ---- codebooks --------------------------------------------------------------- */
typedef struct {
    int    dim, entries, nodes;
    int   *tree;                       /* pairs; leaf = -(1 + entry), 0 = none */
    int    cap;                        /* how many ints tree has room for      */
    float *vq;                         /* entries x dim, or 0                  */
    int    single, single_len;         /* only one entry is used: which, +1    */
} Codebook;

static int tree_add(Codebook *c, u32 code, int len, int entry)
{
    int node = 0, b;
    for (b = len - 1; b >= 0; b--) {
        int slot = node + (int)((code >> b) & 1);
        if (b == 0) { c->tree[slot] = -1 - entry; return 1; }
        if (c->tree[slot] < 0) return 0;          /* a codeword runs on past another */
        if (c->tree[slot] == 0) {
            if (c->nodes + 2 > c->cap) return 0;  /* more than a full tree needs */
            c->tree[slot] = c->nodes;
            c->tree[c->nodes] = c->tree[c->nodes + 1] = 0;
            c->nodes += 2;
        }
        node = c->tree[slot];
    }
    return 0;
}

/* Codewords, given only their lengths: each entry in turn takes the next
 * free codeword of its length, the leftmost still open in the tree
 * (libvorbis's "marker" way of following the specification's rule). */
static int book_build(Codebook *c, const u8 *len)
{
    u32 marker[33];
    int i, j, used = 0;
    c->cap = (c->entries * 2 + 4) * 2;
    c->tree = (int *)take((u32)c->cap * sizeof(int));
    if (!c->tree) return 0;
    c->nodes = 2;
    c->tree[0] = c->tree[1] = 0;
    c->single = 0;
    for (i = 0; i < 33; i++) marker[i] = 0;
    for (i = 0; i < c->entries; i++) if (len[i]) { used++; c->single = i + 1; c->single_len = len[i]; }
    if (used != 1) c->single = 0;
    else return 1;                     /* one codeword: nothing to look up    */
    for (i = 0; i < c->entries; i++) {
        int L = len[i];
        u32 entry;
        if (!L) continue;
        entry = marker[L];
        if (L < 32 && (entry >> L)) return 0;
        if (!tree_add(c, entry, L, i)) return 0;
        for (j = L; j > 0; j--) {
            if (marker[j] & 1) { if (j == 1) marker[1]++; else marker[j] = marker[j - 1] << 1; break; }
            marker[j]++;
        }
        for (j = L + 1; j < 33; j++) {
            if ((marker[j] >> 1) == entry) { entry = marker[j]; marker[j] = marker[j - 1] << 1; }
            else break;
        }
    }
    return 1;
}

static int decode_scalar(const Codebook *c)
{
    int node = 0;
    if (c->single) { rb(c->single_len); return c->single - 1; }
    for (;;) {
        int next = c->tree[node + (int)rb(1)];
        if (s_eop) return -1;
        if (next < 0) return -1 - next;
        if (next == 0) return -1;
        node = next;
    }
}

static int lookup1_values(int entries, int dim)
{
    int r = 1;
    for (;;) {
        double p = 1.0;
        int k;
        for (k = 0; k < dim; k++) p *= (double)(r + 1);
        if (p > (double)entries) return r;
        r++;
    }
}

static int read_codebook(Codebook *c)
{
    static u8 len[65536];
    int i, lookup;
    if (rb(24) != 0x564342) return 0;
    c->dim = (int)rb(16);
    c->entries = (int)rb(24);
    if (c->entries > 65536 || c->dim < 1) return 0;
    if ((u32)c->entries * (u32)c->dim > (1u << 22)) return 0;   /* more than the pool holds */
    if (!rb(1)) {                                  /* not ordered            */
        int sparse = (int)rb(1);
        for (i = 0; i < c->entries; i++)
            len[i] = (u8)(!sparse || rb(1) ? rb(5) + 1 : 0);
    } else {
        int cur = 0, l = (int)rb(5) + 1;
        while (cur < c->entries) {
            int n;
            if (l > 32) return 0;
            n = (int)rb(ilog((u32)(c->entries - cur)));
            for (i = 0; i < n && cur < c->entries; i++) len[cur++] = (u8)l;
            l++;
            if (s_eop) return 0;
        }
    }
    if (!book_build(c, len)) return 0;
    lookup = (int)rb(4);
    c->vq = 0;
    if (lookup == 1 || lookup == 2) {
        float mn = unpack_float(rb(32)), delta = unpack_float(rb(32));
        int bits = (int)rb(4) + 1, seq = (int)rb(1), nv, e, k;
        u32 *mult;
        nv = lookup == 1 ? lookup1_values(c->entries, c->dim) : c->entries * c->dim;
        mult = (u32 *)take((u32)nv * 4);
        c->vq = (float *)take((u32)(c->entries * c->dim) * 4);
        if (!mult || !c->vq) return 0;
        for (i = 0; i < nv; i++) mult[i] = rb(bits);
        for (e = 0; e < c->entries; e++) {
            float last = 0;
            u32 div = 1;
            for (k = 0; k < c->dim; k++) {
                u32 off = lookup == 1 ? (u32)(e / div) % (u32)nv : (u32)(e * c->dim + k);
                float v = (float)mult[off] * delta + mn + last;
                c->vq[e * c->dim + k] = v;
                if (seq) last = v;
                if (lookup == 1) div *= (u32)nv;
            }
        }
    } else if (lookup != 0) return 0;
    return !s_eop;
}

/* ---- floors (type 1) ------------------------------------------------------------- */
typedef struct {
    int  parts, values, mult;
    u8   pclass[32];
    u8   cdim[16], csub[16];
    int  cmaster[16], subbooks[16][8];
    int  x[66], sorted[66], lo[66], hi[66];
} Floor;

static int read_floor(Floor *f, int nbooks)
{
    int i, j, maxc = -1, rangebits;
    if (rb(16) != 1) return 0;                     /* type 0: not read        */
    f->parts = (int)rb(5);
    for (i = 0; i < f->parts; i++) { f->pclass[i] = (u8)rb(4); if (f->pclass[i] > maxc) maxc = f->pclass[i]; }
    for (i = 0; i <= maxc; i++) {
        f->cdim[i] = (u8)(rb(3) + 1);
        f->csub[i] = (u8)rb(2);
        f->cmaster[i] = f->csub[i] ? (int)rb(8) : -1;
        if (f->cmaster[i] >= nbooks) return 0;
        for (j = 0; j < (1 << f->csub[i]); j++) {
            f->subbooks[i][j] = (int)rb(8) - 1;
            if (f->subbooks[i][j] >= nbooks) return 0;
        }
    }
    f->mult = (int)rb(2) + 1;
    rangebits = (int)rb(4);
    f->x[0] = 0; f->x[1] = 1 << rangebits; f->values = 2;
    for (i = 0; i < f->parts; i++)
        for (j = 0; j < f->cdim[f->pclass[i]]; j++) {
            if (f->values >= 65) return 0;
            f->x[f->values++] = (int)rb(rangebits);
        }
    for (i = 1; i < f->values; i++)                /* no two at the same place */
        for (j = 0; j < i; j++) if (f->x[i] == f->x[j]) return 0;
    /* the order to draw them in, and each point's neighbours */
    for (i = 0; i < f->values; i++) f->sorted[i] = i;
    for (i = 1; i < f->values; i++)
        for (j = i; j > 0 && f->x[f->sorted[j - 1]] > f->x[f->sorted[j]]; j--) {
            int t = f->sorted[j]; f->sorted[j] = f->sorted[j - 1]; f->sorted[j - 1] = t;
        }
    for (i = 2; i < f->values; i++) {
        int lo = 0, hi = 1, lx = -1, hx = 1 << 30;
        for (j = 0; j < i; j++) {
            if (f->x[j] < f->x[i] && f->x[j] > lx) { lx = f->x[j]; lo = j; }
            if (f->x[j] > f->x[i] && f->x[j] < hx) { hx = f->x[j]; hi = j; }
        }
        f->lo[i] = lo; f->hi[i] = hi;
    }
    return !s_eop;
}

static float s_invdb[256];

static int render_point(int x0, int y0, int x1, int y1, int x)
{
    int dy = y1 - y0, adx = x1 - x0, ady = dy < 0 ? -dy : dy, off = ady * (x - x0) / adx;
    return dy < 0 ? y0 - off : y0 + off;
}

static void render_line(int x0, int y0, int x1, int y1, float *v, int n)
{
    int dy = y1 - y0, adx = x1 - x0, ady = dy < 0 ? -dy : dy, base = dy / adx, x = x0, y = y0, err = 0, sy;
    sy = dy < 0 ? base - 1 : base + 1;
    ady -= (base < 0 ? -base : base) * adx;
    if (x < n) v[x] = s_invdb[y < 0 ? 0 : y > 255 ? 255 : y];
    for (x = x0 + 1; x < x1 && x < n; x++) {
        err += ady;
        if (err >= adx) { err -= adx; y += sy; } else y += base;
        v[x] = s_invdb[y < 0 ? 0 : y > 255 ? 255 : y];
    }
}

/* Read a channel's floor: 0 if it is not used this block. */
static int floor_decode(const Floor *f, const Codebook *books, int *Y)
{
    static const int ranges[4] = { 256, 128, 86, 64 };
    int range = ranges[f->mult - 1], bits = ilog((u32)range - 1), off = 2, i, j;
    if (!rb(1)) return 0;
    Y[0] = (int)rb(bits); Y[1] = (int)rb(bits);
    for (i = 0; i < f->parts; i++) {
        int c = f->pclass[i], cdim = f->cdim[c], cbits = f->csub[c], csub = (1 << cbits) - 1, cval = 0;
        if (cbits) cval = decode_scalar(&books[f->cmaster[c]]);
        for (j = 0; j < cdim; j++) {
            int book = f->subbooks[c][cval & csub];
            cval >>= cbits;
            Y[off++] = book >= 0 ? decode_scalar(&books[book]) : 0;
        }
    }
    return !s_eop;
}

/* The floor as a curve over n spectral lines. */
static void floor_curve(const Floor *f, const int *Y, float *out, int n)
{
    static const int ranges[4] = { 256, 128, 86, 64 };
    int fy[66], step2[66], range = ranges[f->mult - 1], i, lx = 0, ly, hx = 0, hy = 0;
    fy[0] = Y[0]; fy[1] = Y[1]; step2[0] = step2[1] = 1;
    for (i = 2; i < f->values; i++) {
        int lo = f->lo[i], hi = f->hi[i];
        int pred = render_point(f->x[lo], fy[lo], f->x[hi], fy[hi], f->x[i]);
        int val = Y[i], highroom = range - pred, lowroom = pred;
        int room = (highroom < lowroom ? highroom : lowroom) * 2;
        if (val) {
            step2[lo] = step2[hi] = step2[i] = 1;
            if (val >= room) fy[i] = highroom > lowroom ? val - lowroom + pred : pred - val + highroom - 1;
            else fy[i] = (val & 1) ? pred - (val + 1) / 2 : pred + val / 2;
        } else { step2[i] = 0; fy[i] = pred; }
    }
    ly = fy[f->sorted[0]] * f->mult;
    for (i = 1; i < f->values; i++) {
        int k = f->sorted[i];
        if (!step2[k]) continue;
        hy = fy[k] * f->mult;
        hx = f->x[k];
        if (hx > lx) render_line(lx, ly, hx, hy, out, n);
        lx = hx; ly = hy;
    }
    for (i = hx; i < n; i++) out[i] = s_invdb[hy < 0 ? 0 : hy > 255 ? 255 : hy];
}

/* ---- residues ----------------------------------------------------------------------- */
typedef struct {
    int type, begin, end, psize, classes, classbook;
    u8  cascade[64];
    int books[64][8];
} Residue;

static int read_residue(Residue *r, int nbooks)
{
    int i, j;
    r->type = (int)rb(16);
    if (r->type > 2) return 0;
    r->begin = (int)rb(24); r->end = (int)rb(24);
    r->psize = (int)rb(24) + 1;
    r->classes = (int)rb(6) + 1;
    r->classbook = (int)rb(8);
    if (r->classbook >= nbooks) return 0;
    for (i = 0; i < r->classes; i++) {
        int low = (int)rb(3), high = rb(1) ? (int)rb(5) : 0;
        r->cascade[i] = (u8)(high * 8 + low);
    }
    for (i = 0; i < r->classes; i++)
        for (j = 0; j < 8; j++) {
            r->books[i][j] = (r->cascade[i] >> j) & 1 ? (int)rb(8) : -1;
            if (r->books[i][j] >= nbooks) return 0;
        }
    return !s_eop;
}

/* Decode a residue into ch vectors of n lines each (added to what is there). */
static void residue_decode(const Residue *r, const Codebook *books, float **v, int ch, const int *skip, int n)
{
    static u8 cls[MAXCH][4096];
    const Codebook *cb = &books[r->classbook];
    int size = r->type == 2 ? n * ch : n, begin = r->begin < size ? r->begin : size, end = r->end < size ? r->end : size;
    int parts = (end - begin) / r->psize, cpw = cb->dim, pass, c, i, j, k, nch = r->type == 2 ? 1 : ch;
    static float flat[MAXCH * 4096];
    float *vv[MAXCH];

    if (parts > 4096) parts = 4096;
    if (size > MAXCH * 4096) return;
    if (r->type == 2) {
        int any = 0;
        for (c = 0; c < ch; c++) if (!skip[c]) any = 1;
        if (!any) return;
        for (i = 0; i < size; i++) flat[i] = 0;
        vv[0] = flat;
    } else
        for (c = 0; c < ch; c++) vv[c] = v[c];

    for (pass = 0; pass < 8; pass++) {
        int pc = 0;
        while (pc < parts) {
            if (pass == 0)
                for (c = 0; c < nch; c++) {
                    int temp;
                    if (r->type != 2 && skip[c]) continue;
                    temp = decode_scalar(cb);
                    if (temp < 0) return;
                    for (i = cpw - 1; i >= 0; i--) {
                        if (pc + i < parts) cls[c][pc + i] = (u8)(temp % r->classes);
                        temp /= r->classes;
                    }
                }
            for (i = 0; i < cpw && pc < parts; i++, pc++)
                for (c = 0; c < nch; c++) {
                    int book, off = begin + pc * r->psize;
                    const Codebook *b;
                    if (r->type != 2 && skip[c]) continue;
                    book = r->books[cls[c][pc]][pass];
                    if (book < 0) continue;
                    b = &books[book];
                    if (!b->vq) continue;
                    if (r->type == 0) {
                        int step = r->psize / b->dim;
                        for (j = 0; j < step; j++) {
                            int e = decode_scalar(b);
                            if (e < 0) return;
                            for (k = 0; k < b->dim; k++) vv[c][off + j + k * step] += b->vq[e * b->dim + k];
                        }
                    } else {
                        for (j = 0; j < r->psize; ) {
                            int e = decode_scalar(b);
                            if (e < 0) return;
                            for (k = 0; k < b->dim && j < r->psize; k++, j++) vv[c][off + j] += b->vq[e * b->dim + k];
                        }
                    }
                }
        }
    }
    if (r->type == 2)
        for (i = 0; i < n; i++)
            for (c = 0; c < ch; c++) v[c][i] += flat[i * ch + c];
}

/* ---- the stream ---------------------------------------------------------------------- */
typedef struct { int submaps, steps; u8 mag[256], ang[256], mux[MAXCH], sfloor[16], sres[16]; } Mapping;

static struct {
    int ch, rate, bs[2], half, kbps;
    int nbooks, nfloors, nres, nmaps, nmodes;
    Codebook *books;
    Floor *floors;
    Residue *res;
    Mapping *maps;
    struct { int block, map; } modes[64];
    int headers;                         /* how many of the three have come */
    int flac;                            /* the Ogg holds FLAC, not Vorbis  */
    u32 serial;
    float *prev;                          /* the last block, windowed         */
    float *slope[2];                      /* the window's rising edges, worked out once */
    int    slope_n[2];
    int prev_n, have_prev;
    float *vec[MAXCH];
    s16 out[16384];
    int out_n, out_at;
    /* Ogg: a packet being put together from page segments */
    u8  pkt[65536];
    int pkt_len, pkt_bad;
} V;

static int setup_ident(const u8 *p, int len)
{
    int b0, b1;
    if (len < 30 || p[0] != 1) return 0;
    s_bp = p; s_bpos = 7 * 8; s_bend = (u32)len * 8; s_eop = 0;
    if (rb(32) != 0) return 0;                     /* version                */
    V.ch = (int)rb(8);
    V.rate = (int)rb(32);
    rb(32); V.kbps = (int)(rb(32) / 1000); rb(32);
    b0 = (int)rb(4); b1 = (int)rb(4);
    if (V.ch < 1 || V.ch > MAXCH || V.rate < 1000 || b0 < 6 || b1 > 13 || b0 > b1) return 0;
    V.bs[0] = 1 << b0; V.bs[1] = 1 << b1;
    V.half = V.rate >= 32000;
    return 1;
}

static float wslope(int i, int n);

static int setup_main(const u8 *p, int len)
{
    int i, j, k;
    if (len < 7 || p[0] != 5) return 0;
    s_bp = p; s_bpos = 7 * 8; s_bend = (u32)len * 8; s_eop = 0;
    V.nbooks = (int)rb(8) + 1;
    V.books = (Codebook *)take((u32)V.nbooks * sizeof(Codebook));
    if (!V.books) return 0;
    for (i = 0; i < V.nbooks; i++) if (!read_codebook(&V.books[i])) return 0;
    k = (int)rb(6) + 1;                            /* time domain: placeholders */
    for (i = 0; i < k; i++) if (rb(16)) return 0;
    V.nfloors = (int)rb(6) + 1;
    V.floors = (Floor *)take((u32)V.nfloors * sizeof(Floor));
    if (!V.floors) return 0;
    for (i = 0; i < V.nfloors; i++) if (!read_floor(&V.floors[i], V.nbooks)) return 0;
    V.nres = (int)rb(6) + 1;
    V.res = (Residue *)take((u32)V.nres * sizeof(Residue));
    if (!V.res) return 0;
    for (i = 0; i < V.nres; i++) if (!read_residue(&V.res[i], V.nbooks)) return 0;
    V.nmaps = (int)rb(6) + 1;
    V.maps = (Mapping *)take((u32)V.nmaps * sizeof(Mapping));
    if (!V.maps) return 0;
    for (i = 0; i < V.nmaps; i++) {
        Mapping *m = &V.maps[i];
        if (rb(16) != 0) return 0;
        m->submaps = rb(1) ? (int)rb(4) + 1 : 1;
        m->steps = 0;
        if (rb(1)) {
            m->steps = (int)rb(8) + 1;
            for (j = 0; j < m->steps; j++) {
                m->mag[j] = (u8)rb(ilog((u32)V.ch - 1));
                m->ang[j] = (u8)rb(ilog((u32)V.ch - 1));
                if (m->mag[j] >= V.ch || m->ang[j] >= V.ch || m->mag[j] == m->ang[j]) return 0;
            }
        }
        if (rb(2)) return 0;
        for (j = 0; j < V.ch; j++) {
            m->mux[j] = (u8)(m->submaps > 1 ? rb(4) : 0);
            if (m->mux[j] >= m->submaps) return 0;
        }
        for (j = 0; j < m->submaps; j++) {
            rb(8);
            m->sfloor[j] = (u8)rb(8);
            m->sres[j] = (u8)rb(8);
            if (m->sfloor[j] >= V.nfloors || m->sres[j] >= V.nres) return 0;
        }
    }
    V.nmodes = (int)rb(6) + 1;
    if (V.nmodes > 64) return 0;
    for (i = 0; i < V.nmodes; i++) {
        V.modes[i].block = (int)rb(1);
        rb(16); rb(16);
        V.modes[i].map = (int)rb(8);
        if (V.modes[i].map >= V.nmaps) return 0;
    }
    if (!rb(1)) return 0;                          /* framing                */
    for (i = 0; i < V.ch; i++) {
        V.vec[i] = (float *)take((u32)(V.bs[1] / 2) * 4);
        if (!V.vec[i]) return 0;
    }
    V.prev = (float *)take((u32)V.bs[1] * 4);
    if (!V.prev) return 0;
    /* The window's edges come in only two lengths, half a short block and
     * half a long one (at half rate, half that): worked out here once, not
     * with two sines at every sample of every block. */
    for (i = 0; i < 2; i++) {
        int len = (V.half ? V.bs[i] / 2 : V.bs[i]) / 2;
        V.slope[i] = (float *)take((u32)len * 4);
        if (!V.slope[i]) return 0;
        V.slope_n[i] = len;
        for (j = 0; j < len; j++) V.slope[i][j] = wslope(j, len);
    }
    return 1;
}

/* The window's rising edge, n samples long. */
static float wslope(int i, int n)
{
    double x = fsin((i + 0.5) / n * 1.5707963267948966);
    return (float)fsin(1.5707963267948966 * x * x);
}

static void audio_packet(const u8 *p, int len)
{
    static float spec[4096], mix[4096], tbuf[8192], fl[4096];
    static int Y[MAXCH][66], nonzero[MAXCH], skip[MAXCH];
    const Mapping *m;
    int mode, blk, n, prevw = 0, nextw = 0, c, i, s, half, n2, use;
    float *vv[MAXCH];

    s_bp = p; s_bpos = 0; s_bend = (u32)len * 8; s_eop = 0;
    if (rb(1)) return;                             /* not audio              */
    mode = (int)rb(ilog((u32)V.nmodes - 1));
    if (mode >= V.nmodes) return;
    blk = V.modes[mode].block;
    n = V.bs[blk];
    if (blk) { prevw = (int)rb(1); nextw = (int)rb(1); }
    m = &V.maps[V.modes[mode].map];
    half = n / 2;

    for (c = 0; c < V.ch; c++) {
        const Floor *f = &V.floors[m->sfloor[m->mux[c]]];
        nonzero[c] = floor_decode(f, V.books, Y[c]);
        for (i = 0; i < half; i++) V.vec[c][i] = 0;
    }
    for (s = 0; s < m->steps; s++)
        if (nonzero[m->mag[s]] || nonzero[m->ang[s]]) nonzero[m->mag[s]] = nonzero[m->ang[s]] = 1;
    for (s = 0; s < m->submaps; s++) {
        int nch = 0;
        for (c = 0; c < V.ch; c++)
            if (m->mux[c] == s) { vv[nch] = V.vec[c]; skip[nch] = !nonzero[c]; nch++; }
        residue_decode(&V.res[m->sres[s]], V.books, vv, nch, skip, half);
    }
    for (s = m->steps - 1; s >= 0; s--) {
        float *M = V.vec[m->mag[s]], *A = V.vec[m->ang[s]];
        for (i = 0; i < half; i++) {
            float mg = M[i], an = A[i], nm, na;
            if (mg > 0) { if (an > 0) { nm = mg; na = mg - an; } else { na = mg; nm = mg + an; } }
            else        { if (an > 0) { nm = mg; na = mg + an; } else { na = mg; nm = mg - an; } }
            M[i] = nm; A[i] = na;
        }
    }

    /* Floor times residue, all channels added: the mono spectrum.  At half
     * rate only the lower half of it is wanted. */
    n2 = V.half ? n / 2 : n;                       /* the transform's size   */
    use = n2 / 2;
    for (i = 0; i < use; i++) mix[i] = 0;
    for (c = 0; c < V.ch; c++) {
        if (!nonzero[c]) continue;
        floor_curve(&V.floors[m->sfloor[m->mux[c]]], Y[c], fl, use);
        for (i = 0; i < use; i++) mix[i] += fl[i] * V.vec[c][i];
    }
    for (i = 0; i < use; i++) spec[i] = mix[i] / (float)V.ch;
    imdct(spec, tbuf, n2);

    /* The window: its left and right edges follow the blocks on either side. */
    {
        int bs0 = V.half ? V.bs[0] / 2 : V.bs[0];
        int ln = blk && !prevw ? bs0 / 2 : n2 / 2, rn = blk && !nextw ? bs0 / 2 : n2 / 2;
        int ls = n2 / 4 - ln / 2, rs = n2 * 3 / 4 - rn / 2;
        const float *lw = ln == V.slope_n[0] ? V.slope[0] : V.slope[1];
        const float *rw = rn == V.slope_n[0] ? V.slope[0] : V.slope[1];
        for (i = 0; i < ls; i++) tbuf[i] = 0;
        for (; i < ls + ln; i++) tbuf[i] *= lw[i - ls];
        for (i = rs; i < rs + rn; i++) tbuf[i] *= rw[rs + rn - 1 - i];
        for (; i < n2; i++) tbuf[i] = 0;
    }

    /* Overlap with the block before: from its centre to this one's. */
    V.out_n = 0; V.out_at = 0;
    if (V.have_prev) {
        int pn = V.prev_n, R = pn / 4 + n2 / 4, k;
        if (R > (int)(sizeof V.out / sizeof V.out[0])) R = (int)(sizeof V.out / sizeof V.out[0]);
        for (k = 0; k < R; k++) {
            int pi = pn / 2 + k, ci = k - pn / 4 + n2 / 4;
            float v = (pi < pn ? V.prev[pi] : 0) + (ci >= 0 && ci < n2 ? tbuf[ci] : 0);
            v *= 32767.0f * AUDIO_HEADROOM;
            V.out[k] = (s16)(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : (int)v);
        }
        V.out_n = R;
    }
    for (i = 0; i < n2; i++) V.prev[i] = tbuf[i];
    V.prev_n = n2;
    V.have_prev = 1;
}

static int packet(const u8 *p, int len)
{
    /* Ogg FLAC: 0x7F "FLAC", its version, how many header packets, then
     * "fLaC" and STREAMINFO as in a FLAC file; later packets are the other
     * metadata blocks, then a frame each. */
    if (!V.headers && len >= 51 && p[0] == 0x7F && p[1] == 'F' && p[2] == 'L' && p[3] == 'A' && p[4] == 'C' &&
        p[9] == 'f' && p[10] == 'L' && p[11] == 'a' && p[12] == 'C') {
        V.rate = flac_info(p + 17, &V.ch);
        if (!V.rate) return 0;
        V.flac = 1; V.half = 0; V.kbps = 0; V.headers = 3;
        return 1;
    }
    if (V.flac) {
        if (len >= 2 && p[0] == 0xFF && (p[1] & 0xFE) == 0xF8) { V.out_n = flac_one(p, (u32)len, V.out); V.out_at = 0; }
        return 1;
    }
    if (V.headers < 3) {
        if (len < 7 || !(p[1] == 'v' && p[2] == 'o' && p[3] == 'r' && p[4] == 'b' && p[5] == 'i' && p[6] == 's'))
            return V.headers ? 1 : 0;
        if (V.headers == 0) { if (!setup_ident(p, len)) return 0; V.headers = 1; }
        else if (V.headers == 1) { if (p[0] != 3) return 0; V.headers = 2; }
        else { if (!setup_main(p, len)) return 0; V.headers = 3; }
        return 1;
    }
    audio_packet(p, len);
    return 1;
}

static void fresh(void)
{
    int i;
    s_pool_used = 0;
    V.headers = 0; V.flac = 0; V.have_prev = 0; V.out_n = V.out_at = 0; V.pkt_len = 0; V.pkt_bad = 0;
    if (!s_invdb[255]) {
        /* 0.546875 dB a step, from -139.5 dB to 0 */
        double r = fexp(2.302585092994046 * 7.0 / 256.0), v = 1.0;
        for (i = 255; i >= 0; i--) { s_invdb[i] = (float)v; v /= r; }
    }
}

/* ---- Ogg ----------------------------------------------------------------------------
 * A page is copied out whole (a stream's buffer may move under it), then
 * its segments are handed on a packet at a time, so one packet's audio is
 * given out before the next is decoded. */
static u8  s_page[255 * 255];
static u8  s_lace[255];
static int s_nseg, s_seg, s_at;

static int load_page(AudioIn *in)
{
    const u8 *b;
    u32 left, hl, body = 0, serial;
    int i;
    for (;;) {
        left = in->len - in->pos;
        if (left < 27) return in->done ? AUDIO_END : AUDIO_MORE;
        b = in->buf + in->pos;
        if (b[0] == 'O' && b[1] == 'g' && b[2] == 'g' && b[3] == 'S' && b[4] == 0) break;
        in->pos++;
    }
    hl = 27 + (u32)b[26];
    if (left < hl) return in->done ? AUDIO_END : AUDIO_MORE;
    for (i = 0; i < b[26]; i++) body += b[27 + i];
    if (left < hl + body) return in->done ? AUDIO_END : AUDIO_MORE;
    serial = (u32)b[14] | ((u32)b[15] << 8) | ((u32)b[16] << 16) | ((u32)b[17] << 24);
    if (b[5] & 2) {                                /* a stream begins        */
        if (V.headers >= 3 || !V.headers) { fresh(); V.serial = serial; }
    } else if (serial != V.serial && V.headers) {
        in->pos += hl + body;                      /* another stream's page  */
        s_nseg = 0;
        return 1;
    }
    if (!(b[5] & 1)) { V.pkt_len = 0; V.pkt_bad = 0; }   /* not a continuation */
    s_nseg = b[26];
    for (i = 0; i < s_nseg; i++) s_lace[i] = b[27 + i];
    memcpy(s_page, b + hl, body);
    s_seg = 0; s_at = 0;
    in->pos += hl + body;
    return 1;
}

/* Segments into the packet being built until one is complete and handled.
 * 1 when one was; 0 when the page has none left; AUDIO_BAD for headers
 * that are not Vorbis. */
static int next_packet(void)
{
    while (s_seg < s_nseg) {
        int seg = s_lace[s_seg++];
        if (V.pkt_len + seg <= (int)sizeof V.pkt) {
            memcpy(V.pkt + V.pkt_len, s_page + s_at, (u32)seg);
            V.pkt_len += seg;
        } else V.pkt_bad = 1;
        s_at += seg;
        if (seg < 255) {
            int ok = 1;
            if (!V.pkt_bad) ok = packet(V.pkt, V.pkt_len);
            V.pkt_len = 0; V.pkt_bad = 0;
            if (!ok && V.headers < 3) return AUDIO_BAD;
            return 1;
        }
    }
    return 0;
}

static int vorbis_decode(Decoder *d, AudioIn *in, s16 *out, int max)
{
    int n = 0;
    (void)d;
    while (n < max) {
        int r;
        if (V.out_at < V.out_n) {
            while (n < max && V.out_at < V.out_n) out[n++] = V.out[V.out_at++];
            continue;
        }
        V.out_n = V.out_at = 0;
        r = next_packet();
        if (r == AUDIO_BAD) return n ? n : AUDIO_BAD;
        if (r) continue;
        r = load_page(in);
        if (r != 1) return n ? n : r;
    }
    return n;
}

/* Ready when the three headers have been read, which takes the first page
 * or two. */
int vorbis_open(AudioIn *in, Decoder *d)
{
    u32 keep = in->pos;
    fresh();
    s_nseg = s_seg = 0;
    V.serial = 0;
    while (V.headers < 3) {
        int r = next_packet();
        if (r == AUDIO_BAD) return 0;
        if (r) continue;
        r = load_page(in);
        if (r == AUDIO_MORE) { in->pos = keep; fresh(); s_nseg = 0; return AUDIO_MORE; }
        if (r != 1) return 0;
    }
    d->kind = V.flac ? "FLAC" : "Vorbis";
    d->rate = V.half ? V.rate / 2 : V.rate;
    d->channels = V.ch;
    d->kbps = V.kbps;
    d->decode = vorbis_decode;
    return 1;
}
