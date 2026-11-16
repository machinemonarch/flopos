/* ===========================================================================
 * x25519.c -- agreeing on a secret in public (RFC 7748).
 *
 * Each side picks a random number, multiplies a fixed point on a curve by
 * it, and sends the result.  Each then multiplies the other's result by
 * its own number, and both arrive at the same point -- which nobody
 * watching the wire can work out, because undoing the multiplication is
 * the hard problem the whole of modern encryption rests on.  TLS 1.3
 * starts every connection this way.
 *
 * The arithmetic is modulo 2^255 - 19, done in sixteen 16-bit pieces held
 * in 64-bit integers, so every product has room and carries can be put
 * off.  2^256 is 38 more than a multiple of the modulus, which is how
 * anything that overflows the top piece is folded back into the bottom.
 * Every step does the same work whatever the secret's bits are -- swaps
 * are done by masking, not by branching -- so its timing gives nothing
 * away.
 *
 * Written from RFC 7748 and checked against an independent implementation
 * on random inputs, and by the `crypto` instrument on the machine itself.
 * ===========================================================================
 */
#include "flopos.h"

typedef s64 Fe[16];

static void fe_copy(Fe o, const Fe a) { int i; for (i = 0; i < 16; i++) o[i] = a[i]; }

static void carry(Fe o)
{
    int i;
    s64 c;
    for (i = 0; i < 16; i++) {
        c = o[i] >> 16;                  /* arithmetic: floors negatives   */
        o[i] -= c * 65536;
        if (i < 15) o[i + 1] += c;
        else        o[0] += 38 * c;
    }
}

static void fe_add(Fe o, const Fe a, const Fe b) { int i; for (i = 0; i < 16; i++) o[i] = a[i] + b[i]; }
static void fe_sub(Fe o, const Fe a, const Fe b) { int i; for (i = 0; i < 16; i++) o[i] = a[i] - b[i]; }

static void fe_mul(Fe o, const Fe a, const Fe b)
{
    s64 t[31];
    int i, j;

    for (i = 0; i < 31; i++) t[i] = 0;
    for (i = 0; i < 16; i++)
        for (j = 0; j < 16; j++) t[i + j] += a[i] * b[j];
    for (i = 0; i < 15; i++) t[i] += 38 * t[i + 16];
    for (i = 0; i < 16; i++) o[i] = t[i];
    carry(o);
    carry(o);
}

static void fe_sq(Fe o, const Fe a) { fe_mul(o, a, a); }

/* a to the power p - 2, which is a's inverse.  p - 2 is all ones in
 * binary apart from bits 2 and 4. */
static void fe_inv(Fe o, const Fe a)
{
    Fe c;
    int i;
    fe_copy(c, a);
    for (i = 253; i >= 0; i--) {
        fe_sq(c, c);
        if (i != 2 && i != 4) fe_mul(c, c, a);
    }
    fe_copy(o, c);
}

/* Swap a and b if bit is 1, without a branch that depends on it. */
static void fe_cswap(Fe a, Fe b, int bit)
{
    s64 mask = -(s64)bit, t;
    int i;
    for (i = 0; i < 16; i++) {
        t = mask & (a[i] ^ b[i]);
        a[i] ^= t;
        b[i] ^= t;
    }
}

static void unpack(Fe o, const u8 in[32])
{
    int i;
    for (i = 0; i < 16; i++) o[i] = in[2 * i] | ((s64)in[2 * i + 1] << 8);
    o[15] &= 0x7fff;                     /* the top bit is ignored         */
}

/* Fully reduced, then bytes.  Subtracting p at most twice is enough once
 * the pieces have been carried. */
static void pack(u8 out[32], const Fe n)
{
    Fe t, m;
    int i, j;
    s64 borrow;

    fe_copy(t, n);
    carry(t); carry(t); carry(t);
    for (j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;
        for (i = 1; i < 15; i++) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        borrow = (m[15] >> 16) & 1;
        m[14] &= 0xffff;
        fe_cswap(t, m, (int)(1 - borrow));
    }
    for (i = 0; i < 16; i++) {
        out[2 * i] = (u8)(t[i] & 0xff);
        out[2 * i + 1] = (u8)((t[i] >> 8) & 0xff);
    }
}

void x25519(u8 out[32], const u8 scalar[32], const u8 point[32])
{
    static const Fe a24 = { 0xDB41, 1 };  /* 121665                        */
    u8 k[32];
    Fe x1, x2, z2, x3, z3, A, AA, B, BB, E, C, D, DA, CB, t;
    int i, swap = 0;

    for (i = 0; i < 32; i++) k[i] = scalar[i];
    k[0] &= 248;
    k[31] &= 127;
    k[31] |= 64;

    unpack(x1, point);
    for (i = 0; i < 16; i++) { x2[i] = 0; z2[i] = 0; z3[i] = 0; }
    x2[0] = 1;
    z3[0] = 1;
    fe_copy(x3, x1);

    for (i = 254; i >= 0; i--) {
        int bit = (k[i >> 3] >> (i & 7)) & 1;
        swap ^= bit;
        fe_cswap(x2, x3, swap);
        fe_cswap(z2, z3, swap);
        swap = bit;

        fe_add(A, x2, z2);  fe_sq(AA, A);
        fe_sub(B, x2, z2);  fe_sq(BB, B);
        fe_sub(E, AA, BB);
        fe_add(C, x3, z3);
        fe_sub(D, x3, z3);
        fe_mul(DA, D, A);
        fe_mul(CB, C, B);
        fe_add(t, DA, CB);  fe_sq(x3, t);
        fe_sub(t, DA, CB);  fe_sq(t, t);  fe_mul(z3, x1, t);
        fe_mul(x2, AA, BB);
        fe_mul(t, a24, E);  fe_add(t, AA, t);  fe_mul(z2, E, t);
    }
    fe_cswap(x2, x3, swap);
    fe_cswap(z2, z3, swap);

    fe_inv(z2, z2);
    fe_mul(x2, x2, z2);
    pack(out, x2);
}

/* Our public half from our secret: the curve's base point, 9, times it. */
void x25519_public(u8 out[32], const u8 secret[32])
{
    static const u8 base[32] = { 9 };
    x25519(out, secret, base);
}
