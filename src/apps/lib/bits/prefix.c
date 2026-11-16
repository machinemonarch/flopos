/* ===========================================================================
 * prefix.c -- building the tables prefix.h decodes with.
 *
 * Three passes over the codes: how wide a second table each long code's
 * first `root` bits need (the longest code under them decides); where
 * those second tables go; and every code written into every entry it
 * owns -- a code of length L shorter than root owns 2^(root - L) entries,
 * all those whose first L bits are it.  An entry claimed twice means the
 * codes were not a prefix code, and the build says so.
 * ===========================================================================
 */
#include "prefix.h"

static u32 reverse(u32 v, int n)
{
    u32 r = 0;
    while (n--) { r = (r << 1) | (v & 1); v >>= 1; }
    return r;
}

/* A code's bits as the stream gives them, first bit in bit 0 for the low
 * order and in the top of n bits for the high. */
static u32 as_read(u32 code, int len, int lsb) { return lsb ? reverse(code, len) : code; }

int prefix_build(Prefix *p, u32 *store, u32 cap, int root,
                 const u32 *codes, const u8 *lens, int n, int lsb)
{
    u32 size = 1u << root, i, j;
    int s;

    if (root < 1 || root > 16 || size > cap) return 0;
    for (i = 0; i < size; i++) store[i] = 0;
    p->tab = store;
    p->root = root;
    p->used = size;

    /* The second tables' widths. */
    for (s = 0; s < n; s++) {
        int L = lens[s], w;
        u32 r, top;
        if (L <= root) continue;
        if (L > root + 12 || L > 32) return 0;
        r = as_read(codes[s], L, lsb);
        top = lsb ? r & (size - 1) : r >> (L - root);
        w = L - root;
        if (!(store[top] & PREFIX_SUB) || (int)(store[top] & 0x3F) < w) store[top] = PREFIX_SUB | (u32)w;
    }
    /* Where they go. */
    for (i = 0; i < size; i++)
        if (store[i] & PREFIX_SUB) {
            u32 w = store[i] & 0x3F, m = 1u << w;
            if (p->used + m > cap) return 0;
            for (j = 0; j < m; j++) store[p->used + j] = 0;
            store[i] = (p->used << 8) | PREFIX_SUB | w;
            p->used += m;
        }
    /* Every code, into every entry it owns. */
    for (s = 0; s < n; s++) {
        int L = lens[s];
        u32 r;
        if (!L) continue;
        r = as_read(codes[s], L, lsb);
        if (L <= root) {
            u32 m = 1u << (root - L);
            for (j = 0; j < m; j++) {
                u32 at = lsb ? (r | (j << L)) : ((r << (root - L)) | j);
                if (store[at]) return 0;
                store[at] = ((u32)s << 8) | (u32)L;
            }
        } else {
            u32 top = lsb ? r & (size - 1) : r >> (L - root);
            u32 e = store[top], base = e >> 8, w = e & 0x3F, l2 = (u32)(L - root), m = 1u << (w - l2);
            u32 rest = lsb ? r >> root : r & ((1u << l2) - 1);
            for (j = 0; j < m; j++) {
                u32 at = base + (lsb ? (rest | (j << l2)) : ((rest << (w - l2)) | j));
                if (store[at]) return 0;
                store[at] = ((u32)s << 8) | l2;
            }
        }
    }
    return 1;
}
