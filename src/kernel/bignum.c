/* ===========================================================================
 * bignum.c -- arithmetic on numbers too big for the processor, modulo one
 * fixed number, the way signature checking needs it.
 *
 * Numbers are arrays of 32-bit words, least significant first.  The one
 * hard operation is multiplying two of them and reducing modulo m, and
 * that is done Montgomery's way: everything is kept multiplied by
 * R = 2^(32n), and a product can then be reduced with multiplies and
 * shifts instead of a long division.  The inner step is one 32x32 -> 64
 * bit multiply-and-add, which the Pentium does in one instruction.
 *
 * RSA uses it with m the public modulus (up to 4096 bits); ECDSA with m
 * the curve's prime and the curve's order.  So does the P-256 and P-384
 * key exchange, with a secret: so multiplying, adding and subtracting do
 * the same work whatever the numbers are -- every result is worked out
 * with and without the modulus taken off, and the right one kept by a
 * mask, never by a branch.
 * ===========================================================================
 */
#include "flopos.h"

int bn_from_be(u32 *a, int n, const u8 *be, int len)
{
    int i;
    for (i = 0; i < n; i++) a[i] = 0;
    while (len > 0 && *be == 0) { be++; len--; }       /* leading zeros     */
    if (len > n * 4) return 0;
    for (i = 0; i < len; i++) {
        int byte = len - 1 - i;                          /* from the bottom   */
        a[byte / 4] |= (u32)be[i] << (8 * (byte % 4));
    }
    return 1;
}

void bn_to_be(u8 *be, int len, const u32 *a, int n)
{
    int i;
    for (i = 0; i < len; i++) {
        int byte = len - 1 - i;
        be[i] = byte / 4 < n ? (u8)(a[byte / 4] >> (8 * (byte % 4))) : 0;
    }
}

int bn_cmp(const u32 *a, const u32 *b, int n)
{
    int i;
    for (i = n - 1; i >= 0; i--) {
        if (a[i] > b[i]) return 1;
        if (a[i] < b[i]) return -1;
    }
    return 0;
}

int bn_is_zero(const u32 *a, int n)
{
    int i;
    for (i = 0; i < n; i++) if (a[i]) return 0;
    return 1;
}

u32 bn_add(u32 *r, const u32 *a, const u32 *b, int n)
{
    u64 c = 0;
    int i;
    for (i = 0; i < n; i++) { c += (u64)a[i] + b[i]; r[i] = (u32)c; c >>= 32; }
    return (u32)c;
}

u32 bn_sub(u32 *r, const u32 *a, const u32 *b, int n)
{
    u32 borrow = 0;
    int i;
    for (i = 0; i < n; i++) {
        u64 d = (u64)a[i] - b[i] - borrow;
        r[i] = (u32)d;
        borrow = (u32)(d >> 63);         /* went below zero: the top bit   */
    }
    return borrow;
}

/* r = a if c is 1, left alone if 0: the same work either way. */
static void bn_keep(u32 *r, const u32 *a, int n, u32 c)
{
    u32 m = 0u - (c & 1);
    int i;
    for (i = 0; i < n; i++) r[i] ^= m & (r[i] ^ a[i]);
}

int bn_bits(const u32 *a, int n)
{
    int i, b;
    for (i = n - 1; i >= 0; i--)
        if (a[i]) { for (b = 31; b >= 0; b--) if (a[i] >> b & 1) return i * 32 + b + 1; }
    return 0;
}

/* ---- Montgomery ------------------------------------------------------------ */
int mont_init(Mont *M, const u8 *mod_be, int len)
{
    u32 x;
    int i, k;

    while (len > 0 && *mod_be == 0) { mod_be++; len--; }
    if (len <= 0 || len > BN_LIMBS * 4 - 4) return 0;
    M->n = (len + 3) / 4;
    bn_from_be(M->m, M->n, mod_be, len);
    if (!(M->m[0] & 1)) return 0;                        /* must be odd       */
    M->bits = bn_bits(M->m, M->n);

    /* -m^-1 mod 2^32, by Newton's method: each round doubles the bits. */
    x = M->m[0];
    for (i = 0; i < 5; i++) x *= 2 - M->m[0] * x;
    M->minv = (u32)(0 - x);

    /* R mod m, then R^2 mod m: start from 1 and double, 32n times each. */
    for (i = 0; i < M->n; i++) M->one[i] = 0;
    M->one[0] = 1;
    for (k = 0; k < 64 * M->n; k++) {
        u32 *t = k < 32 * M->n ? M->one : M->rr;
        u32 top;
        if (k == 32 * M->n) for (i = 0; i < M->n; i++) M->rr[i] = M->one[i];
        top = t[M->n - 1] >> 31;
        for (i = M->n - 1; i > 0; i--) t[i] = (t[i] << 1) | (t[i - 1] >> 31);
        t[0] <<= 1;
        if (top || bn_cmp(t, M->m, M->n) >= 0) bn_sub(t, t, M->m, M->n);
    }
    return 1;
}

/* r = a * b / R mod m.  r may be a or b. */
void mont_mul(const Mont *M, u32 *r, const u32 *a, const u32 *b)
{
    static u32 t[BN_LIMBS + 2];
    int n = M->n, i, j;

    for (i = 0; i < n + 2; i++) t[i] = 0;
    for (i = 0; i < n; i++) {
        u64 c = 0;
        u32 q;
        for (j = 0; j < n; j++) {
            c += (u64)a[j] * b[i] + t[j];
            t[j] = (u32)c; c >>= 32;
        }
        c += t[n]; t[n] = (u32)c; t[n + 1] = (u32)(c >> 32);

        q = t[0] * M->minv;
        c = (u64)q * M->m[0] + t[0];
        c >>= 32;
        for (j = 1; j < n; j++) {
            c += (u64)q * M->m[j] + t[j];
            t[j - 1] = (u32)c; c >>= 32;
        }
        c += t[n]; t[n - 1] = (u32)c;
        t[n] = t[n + 1] + (u32)(c >> 32);
    }
    {
        static u32 d[BN_LIMBS + 2];
        u32 borrow = bn_sub(d, t, M->m, n);
        bn_keep(t, d, n, t[n] | (borrow ^ 1));    /* t - m, unless below zero */
    }
    for (i = 0; i < n; i++) r[i] = t[i];
}

void mont_to(const Mont *M, u32 *r, const u32 *a)   { mont_mul(M, r, a, M->rr); }

void mont_from(const Mont *M, u32 *r, const u32 *a)
{
    static u32 one[BN_LIMBS];
    int i;
    for (i = 0; i < M->n; i++) one[i] = 0;
    one[0] = 1;
    mont_mul(M, r, a, one);
}

/* r = base^exp mod m, all in the ordinary (not Montgomery) form; the
 * exponent is big-endian bytes. */
void mont_pow(const Mont *M, u32 *r, const u32 *base, const u8 *exp, int elen)
{
    static u32 x[BN_LIMBS], acc[BN_LIMBS];
    int i, b, started = 0;

    mont_to(M, x, base);
    for (i = 0; i < M->n; i++) acc[i] = M->one[i];
    for (i = 0; i < elen; i++)
        for (b = 7; b >= 0; b--) {
            if (started) mont_mul(M, acc, acc, acc);
            if (exp[i] >> b & 1) { mont_mul(M, acc, acc, x); started = 1; }
        }
    mont_from(M, r, acc);
}

void mod_add(const Mont *M, u32 *r, const u32 *a, const u32 *b)
{
    static u32 d[BN_LIMBS];
    u32 c = bn_add(r, a, b, M->n), borrow = bn_sub(d, r, M->m, M->n);
    bn_keep(r, d, M->n, c | (borrow ^ 1));
}

void mod_sub(const Mont *M, u32 *r, const u32 *a, const u32 *b)
{
    static u32 d[BN_LIMBS];
    u32 borrow = bn_sub(r, a, b, M->n);
    bn_add(d, r, M->m, M->n);
    bn_keep(r, d, M->n, borrow);
}
