/* ===========================================================================
 * inflate.c -- programs kept packed on the floppy, unpacked as they load.
 *
 * The build (tools/build/mkimage.py) packs ROOM.BIN and every .APP as
 * "FLZ1", the unpacked size (32 bits), then a zlib stream: deflate (RFC
 * 1951) inside zlib's wrapper (RFC 1950), whose Adler-32 at the end is
 * checked.  Written here from those two specifications.
 *
 * load_packed() reads the file into the top of the program's area and
 * unpacks it from the bottom up: with packed + unpacked no bigger than the
 * area, what is written never reaches what is still to be read.  Anything
 * malformed is refused, never trusted.
 * ===========================================================================
 */
#include "flopos.h"
#include "io.h"
#include "inflate.h"

typedef struct {
    const u8 *in, *in_end;
    u32 bits, nbits;
    u8 *out, *out_start, *out_end;
    int bad;
} Z;

static u32 need(Z *z, u32 n)
{
    u32 v;
    while (z->nbits < n) {
        if (z->in >= z->in_end) { z->bad = 1; return 0; }
        z->bits |= (u32)*z->in++ << z->nbits;
        z->nbits += 8;
    }
    v = z->bits & ((1u << n) - 1);
    z->bits >>= n;
    z->nbits -= n;
    return v;
}

/* A canonical Huffman code: counts per length, symbols by code order. */
typedef struct { u16 count[16], sym[320]; } Huff;

static int build(Huff *h, const u8 *len, u32 n)
{
    u16 offs[16];
    u32 i, left = 1;
    for (i = 0; i < 16; i++) h->count[i] = 0;
    for (i = 0; i < n; i++) h->count[len[i]]++;
    h->count[0] = 0;
    for (i = 1; i < 16; i++) {                    /* not oversubscribed */
        left <<= 1;
        if (h->count[i] > left) return 0;
        left -= h->count[i];
    }
    offs[1] = 0;
    for (i = 1; i < 15; i++) offs[i + 1] = offs[i] + h->count[i];
    for (i = 0; i < n; i++) if (len[i]) h->sym[offs[len[i]]++] = (u16)i;
    return 1;
}

static int decode(Z *z, const Huff *h)
{
    int code = 0, first = 0, index = 0, len;
    for (len = 1; len < 16; len++) {
        int count;
        code |= (int)need(z, 1);
        if (z->bad) return -1;
        count = h->count[len];
        if (code - count < first) return h->sym[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    z->bad = 1;
    return -1;
}

static const u16 LBASE[29] = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
static const u8  LEXTRA[29] = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
static const u16 DBASE[30] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
static const u8  DEXTRA[30] = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };

static int codes(Z *z, const Huff *lit, const Huff *dist)
{
    for (;;) {
        int s = decode(z, lit);
        if (s < 0) return 0;
        if (s < 256) {
            if (z->out >= z->out_end) return 0;
            *z->out++ = (u8)s;
        } else if (s == 256) {
            return 1;
        } else {
            u32 len, d;
            s -= 257;
            if (s >= 29) return 0;
            len = LBASE[s] + need(z, LEXTRA[s]);
            s = decode(z, dist);
            if (s < 0 || s >= 30) return 0;
            d = DBASE[s] + need(z, DEXTRA[s]);
            if (z->bad || d > (u32)(z->out - z->out_start) || len > (u32)(z->out_end - z->out)) return 0;
            while (len--) { *z->out = z->out[-(int)d]; z->out++; }
        }
    }
}

static int fixed(Z *z)
{
    static Huff lit, dist;
    static int made;
    if (!made) {
        u8 l[288];
        u32 i;
        for (i = 0; i < 144; i++) l[i] = 8;
        for (; i < 256; i++) l[i] = 9;
        for (; i < 280; i++) l[i] = 7;
        for (; i < 288; i++) l[i] = 8;
        build(&lit, l, 288);
        for (i = 0; i < 30; i++) l[i] = 5;
        build(&dist, l, 30);
        made = 1;
    }
    return codes(z, &lit, &dist);
}

static int dynamic(Z *z)
{
    static const u8 order[19] = { 16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };
    static Huff lit, dist, cl;
    u8 lens[320];
    u32 nlen = need(z, 5) + 257, ndist = need(z, 5) + 1, ncode = need(z, 4) + 4, i;
    if (z->bad || nlen > 286 || ndist > 30) return 0;
    for (i = 0; i < 19; i++) lens[order[i]] = i < ncode ? (u8)need(z, 3) : 0;
    if (z->bad || !build(&cl, lens, 19)) return 0;
    for (i = 0; i < nlen + ndist; ) {
        int s = decode(z, &cl);
        u32 rep, v;
        if (s < 0) return 0;
        if (s < 16) { lens[i++] = (u8)s; continue; }
        if (s == 16) { if (!i) return 0; v = lens[i - 1]; rep = 3 + need(z, 2); }
        else if (s == 17) { v = 0; rep = 3 + need(z, 3); }
        else { v = 0; rep = 11 + need(z, 7); }
        if (z->bad || i + rep > nlen + ndist) return 0;
        while (rep--) lens[i++] = (u8)v;
    }
    if (!lens[256]) return 0;                          /* no end of block */
    if (!build(&lit, lens, nlen) || !build(&dist, lens + nlen, ndist)) return 0;
    return codes(z, &lit, &dist);
}

/* A zlib stream of n bytes into out (cap bytes); the bytes made, or -1. */
int inflate_zlib(const u8 *in, u32 n, u8 *out, u32 cap)
{
    Z z;
    u32 last, a = 1, b = 0, i, sum;
    if (n < 6 || (in[0] & 15) != 8 || ((in[0] << 8) | in[1]) % 31 || (in[1] & 32)) return -1;
    z.in = in + 2; z.in_end = in + n - 4; z.bits = z.nbits = 0; z.bad = 0;
    z.out = z.out_start = out; z.out_end = out + cap;
    do {
        u32 type;
        last = need(&z, 1);
        type = need(&z, 2);
        if (z.bad) return -1;
        if (type == 0) {                                /* stored */
            u32 len;
            z.bits = z.nbits = 0;                       /* to the byte */
            if (z.in_end - z.in < 4) return -1;
            len = z.in[0] | (z.in[1] << 8);
            if ((len ^ 0xFFFF) != (u32)(z.in[2] | (z.in[3] << 8))) return -1;
            z.in += 4;
            if ((u32)(z.in_end - z.in) < len || (u32)(z.out_end - z.out) < len) return -1;
            memcpy8(z.out, z.in, len);
            z.in += len; z.out += len;
        } else if (type == 1) { if (!fixed(&z)) return -1; }
        else if (type == 2) { if (!dynamic(&z)) return -1; }
        else return -1;
    } while (!last);
    for (i = 0; i < (u32)(z.out - out); i++) { a = (a + out[i]) % 65521; b = (b + a) % 65521; }
    sum = ((u32)in[n - 4] << 24) | ((u32)in[n - 3] << 16) | ((u32)in[n - 2] << 8) | in[n - 1];
    return sum == ((b << 16) | a) ? (int)(z.out - out) : -1;
}

/* A program file into dest (room bytes there): packed or plain.  Its size
 * unpacked, or -1 with *why. */
int load_packed(const FsFile *f, u8 *dest, u32 room, const char **why)
{
    u8 head[8];
    u32 size;
    u8 *at;
    if (fs_read(f, head, 8) != 8 || head[0] != 'F' || head[1] != 'L' || head[2] != 'Z' || head[3] != '1') {
        if (f->size > room) { *why = "too big"; return -1; }
        if (fs_read(f, dest, f->size) != (int)f->size) { *why = "the floppy would not give it up"; return -1; }
        return (int)f->size;
    }
    size = head[4] | ((u32)head[5] << 8) | ((u32)head[6] << 16) | ((u32)head[7] << 24);
    if (f->size < 14 || size + 4 > room || f->size > room - size - 4) { *why = "too big"; return -1; }
    at = dest + ((room - f->size) & ~3u);              /* the top: unpacking never reaches it */
    if (fs_read(f, at, f->size) != (int)f->size) { *why = "the floppy would not give it up"; return -1; }
    if (inflate_zlib(at + 8, f->size - 8, dest, size) != (int)size) { *why = "it is damaged"; return -1; }
    return (int)size;
}
