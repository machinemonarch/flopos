/* ===========================================================================
 * bits.h -- reading a stream of bits, quickly.
 *
 * Every compressed format reads bits.  Some put the first bit in the
 * highest place of each byte (MPEG audio, AAC, FLAC, JPEG), some in the
 * lowest (Vorbis, deflate and so gzip and PNG).  Reading them one at a
 * time costs tens of instructions a bit on a Pentium; these read up to 25
 * at once with a single unaligned 32-bit load, and a byte swap for the
 * high-first kind.
 *
 * The buffer must have four readable bytes past its end: callers decode
 * from a padded copy.  Reading past the end gives nothing useful and sets
 * `over`, which is checked once, at the end of whatever was being read,
 * instead of at every bit; the position stops at the end, so a damaged
 * stream can never send a reader wandering off through memory.
 * ===========================================================================
 */
#ifndef BITS_H
#define BITS_H
#include "app.h"

typedef struct {
    const u8 *buf;
    u32 pos, end;                        /* in bits                        */
    int over;                            /* tried to read past the end     */
} Bits;

typedef u32 __attribute__((may_alias, aligned(1))) bits_u32;

/* Four bytes as a number, first byte highest; and first byte lowest. */
static inline u32 bits_be32(const u8 *p) { return __builtin_bswap32(*(const bits_u32 *)p); }
static inline u32 bits_le32(const u8 *p) { return *(const bits_u32 *)p; }

static inline void bits_init(Bits *b, const u8 *buf, u32 bytes)
{
    b->buf = buf; b->pos = 0; b->end = bytes * 8; b->over = 0;
}

static inline void bits_skip(Bits *b, u32 n)
{
    b->pos += n;
    if (b->pos > b->end) { b->over = 1; b->pos = b->end; }
}

static inline void bits_align(Bits *b) { bits_skip(b, (8 - (b->pos & 7)) & 7); }
static inline u32  bits_left(const Bits *b) { return b->end - b->pos; }

/* ---- first bit highest ---------------------------------------------------- */
/* The next n bits (0 to 25), not taken. */
static inline u32 bits_peek(const Bits *b, int n)
{
    u32 w = bits_be32(b->buf + (b->pos >> 3)) << (b->pos & 7);
    return n ? w >> (32 - n) : 0;
}

static inline u32 bits_get(Bits *b, int n)
{
    u32 v = bits_peek(b, n);
    bits_skip(b, (u32)n);
    return v;
}

static inline u32 bits_get32(Bits *b, int n)    /* up to 32 */
{
    u32 hi;
    if (n <= 24) return bits_get(b, n);
    hi = bits_get(b, n - 16);
    return (hi << 16) | bits_get(b, 16);
}

/* ---- first bit lowest ------------------------------------------------------- */
static inline u32 lbits_peek(const Bits *b, int n)
{
    u32 w = bits_le32(b->buf + (b->pos >> 3)) >> (b->pos & 7);
    return n ? w & (0xFFFFFFFFu >> (32 - n)) : 0;
}

static inline u32 lbits_get(Bits *b, int n)
{
    u32 v = lbits_peek(b, n);
    bits_skip(b, (u32)n);
    return v;
}

static inline u32 lbits_get32(Bits *b, int n)   /* up to 32 */
{
    u32 lo;
    if (n <= 24) return lbits_get(b, n);
    lo = lbits_get(b, 16);
    return lo | (lbits_get(b, n - 16) << 16);
}

#endif
