/* ===========================================================================
 * prefix.h -- decoding prefix codes (Huffman codes) by table.
 *
 * A codebook is built once into a table indexed by the next `root` bits
 * of the stream.  Each entry says which symbol those bits begin with and
 * how many of them the code really used.  For the few codes longer than
 * root, the entry instead says where a second, smaller table for the bits
 * after root begins.  So a symbol costs one lookup, or two -- not, as a
 * tree does, one branch for every bit.
 *
 * One kind of table serves both bit orders: codes read first-bit-highest
 * (MPEG audio, AAC, JPEG) and first-bit-lowest (Vorbis, deflate), whose
 * codes are reversed as the table is built.  Codebooks that do not use
 * every code (some Vorbis books) work too: the unused bits decode as -1.
 * ===========================================================================
 */
#ifndef PREFIX_H
#define PREFIX_H
#include "bits.h"

#define PREFIX_SUB 0x40u                 /* an entry pointing at a second table */

typedef struct {
    u32 *tab;                            /* 2^root entries, then the second tables */
    int  root;
    u32  used;                           /* entries of the store in use    */
} Prefix;

/* Build a table into store (cap entries) for n symbols: symbol s has the
 * code codes[s], lens[s] bits long (0: not used), written first-bit-
 * highest; lsb says the stream reads first-bit-lowest.  1 if built; 0 if
 * the store is too small, a code is longer than root + 12, or the codes
 * are not a prefix code. */
int prefix_build(Prefix *p, u32 *store, u32 cap, int root,
                 const u32 *codes, const u8 *lens, int n, int lsb);

/* The next symbol, read first-bit-highest; -1 if the bits are not a code. */
static inline int prefix_get(const Prefix *p, Bits *b)
{
    u32 e = p->tab[bits_peek(b, p->root)];
    if (e & PREFIX_SUB) {
        bits_skip(b, (u32)p->root);
        e = p->tab[(e >> 8) + bits_peek(b, (int)(e & 0x3F))];
    }
    if (!(e & 0x3F)) return -1;
    bits_skip(b, e & 0x3F);
    return (int)(e >> 8);
}

/* The same, first-bit-lowest. */
static inline int prefix_lget(const Prefix *p, Bits *b)
{
    u32 e = p->tab[lbits_peek(b, p->root)];
    if (e & PREFIX_SUB) {
        bits_skip(b, (u32)p->root);
        e = p->tab[(e >> 8) + lbits_peek(b, (int)(e & 0x3F))];
    }
    if (!(e & 0x3F)) return -1;
    bits_skip(b, e & 0x3F);
    return (int)(e >> 8);
}

#endif
