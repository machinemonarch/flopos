/* ===========================================================================
 * ecdsa.c -- checking an ECDSA signature, on P-256 and P-384 (FIPS 186-4).
 *
 * The other kind of signature certificates carry, and the one most of the
 * web's newer certificates use: smaller keys than RSA for the same
 * strength.  Checking one means computing u1*G + u2*Q on the curve, where
 * G is the curve's fixed point, Q the signer's public key, and u1, u2 come
 * from the hash and the signature; the signature is good if that point's
 * x coordinate, reduced modulo the curve's order, is r.
 *
 * Points are kept in Jacobian coordinates (X, Y, Z standing for X/Z^2,
 * Y/Z^3) so that no division happens until the very end, and every
 * coordinate is kept in Montgomery form (bignum.c).  Both curves have
 * a = -3, which the doubling formula takes advantage of.  The two
 * multiplications are done together, one bit of u1 and u2 at a time
 * (Shamir's trick), so it costs about one multiplication, not two.
 *
 * The constants are the curves' own, as OpenSSL prints them.
 * ===========================================================================
 */
#include "flopos.h"

static const u8 p256_p[32] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};
static const u8 p256_b[32] = {
    0x5A, 0xC6, 0x35, 0xD8, 0xAA, 0x3A, 0x93, 0xE7, 0xB3, 0xEB, 0xBD, 0x55,
    0x76, 0x98, 0x86, 0xBC, 0x65, 0x1D, 0x06, 0xB0, 0xCC, 0x53, 0xB0, 0xF6,
    0x3B, 0xCE, 0x3C, 0x3E, 0x27, 0xD2, 0x60, 0x4B,
};
static const u8 p256_g[64] = {
    0x6B, 0x17, 0xD1, 0xF2, 0xE1, 0x2C, 0x42, 0x47, 0xF8, 0xBC, 0xE6, 0xE5,
    0x63, 0xA4, 0x40, 0xF2, 0x77, 0x03, 0x7D, 0x81, 0x2D, 0xEB, 0x33, 0xA0,
    0xF4, 0xA1, 0x39, 0x45, 0xD8, 0x98, 0xC2, 0x96, 0x4F, 0xE3, 0x42, 0xE2,
    0xFE, 0x1A, 0x7F, 0x9B, 0x8E, 0xE7, 0xEB, 0x4A, 0x7C, 0x0F, 0x9E, 0x16,
    0x2B, 0xCE, 0x33, 0x57, 0x6B, 0x31, 0x5E, 0xCE, 0xCB, 0xB6, 0x40, 0x68,
    0x37, 0xBF, 0x51, 0xF5,
};
static const u8 p256_n[32] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xBC, 0xE6, 0xFA, 0xAD, 0xA7, 0x17, 0x9E, 0x84,
    0xF3, 0xB9, 0xCA, 0xC2, 0xFC, 0x63, 0x25, 0x51,
};
static const u8 p384_p[48] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFE, 0xFF, 0xFF, 0xFF, 0xFF,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF,
};
static const u8 p384_b[48] = {
    0xB3, 0x31, 0x2F, 0xA7, 0xE2, 0x3E, 0xE7, 0xE4, 0x98, 0x8E, 0x05, 0x6B,
    0xE3, 0xF8, 0x2D, 0x19, 0x18, 0x1D, 0x9C, 0x6E, 0xFE, 0x81, 0x41, 0x12,
    0x03, 0x14, 0x08, 0x8F, 0x50, 0x13, 0x87, 0x5A, 0xC6, 0x56, 0x39, 0x8D,
    0x8A, 0x2E, 0xD1, 0x9D, 0x2A, 0x85, 0xC8, 0xED, 0xD3, 0xEC, 0x2A, 0xEF,
};
static const u8 p384_g[96] = {
    0xAA, 0x87, 0xCA, 0x22, 0xBE, 0x8B, 0x05, 0x37, 0x8E, 0xB1, 0xC7, 0x1E,
    0xF3, 0x20, 0xAD, 0x74, 0x6E, 0x1D, 0x3B, 0x62, 0x8B, 0xA7, 0x9B, 0x98,
    0x59, 0xF7, 0x41, 0xE0, 0x82, 0x54, 0x2A, 0x38, 0x55, 0x02, 0xF2, 0x5D,
    0xBF, 0x55, 0x29, 0x6C, 0x3A, 0x54, 0x5E, 0x38, 0x72, 0x76, 0x0A, 0xB7,
    0x36, 0x17, 0xDE, 0x4A, 0x96, 0x26, 0x2C, 0x6F, 0x5D, 0x9E, 0x98, 0xBF,
    0x92, 0x92, 0xDC, 0x29, 0xF8, 0xF4, 0x1D, 0xBD, 0x28, 0x9A, 0x14, 0x7C,
    0xE9, 0xDA, 0x31, 0x13, 0xB5, 0xF0, 0xB8, 0xC0, 0x0A, 0x60, 0xB1, 0xCE,
    0x1D, 0x7E, 0x81, 0x9D, 0x7A, 0x43, 0x1D, 0x7C, 0x90, 0xEA, 0x0E, 0x5F,
};
static const u8 p384_n[48] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xC7, 0x63, 0x4D, 0x81, 0xF4, 0x37, 0x2D, 0xDF, 0x58, 0x1A, 0x0D, 0xB2,
    0x48, 0xB0, 0xA7, 0x7A, 0xEC, 0xEC, 0x19, 0x6A, 0xCC, 0xC5, 0x29, 0x73,
};

typedef struct { const u8 *p, *b, *g, *n; int bytes; } Curve;

static const Curve s_curves[2] = {
    { p256_p, p256_b, p256_g, p256_n, 32 },
    { p384_p, p384_b, p384_g, p384_n, 48 },
};

#define L 12                             /* words in the bigger curve     */
typedef struct { u32 x[L], y[L], z[L]; } Pt;

static Mont s_fp, s_fn;                  /* the field, and the order       */
static int  s_w;                         /* words in use                   */
static u32  s_b[L];                      /* b, Montgomery form              */

static void fcopy(u32 *r, const u32 *a) { int i; for (i = 0; i < s_w; i++) r[i] = a[i]; }
static void fmul(u32 *r, const u32 *a, const u32 *b) { mont_mul(&s_fp, r, a, b); }
static void fadd(u32 *r, const u32 *a, const u32 *b) { mod_add(&s_fp, r, a, b); }
static void fsub(u32 *r, const u32 *a, const u32 *b) { mod_sub(&s_fp, r, a, b); }
static int  fzero(const u32 *a) { return bn_is_zero(a, s_w); }

static void pt_double(Pt *r, const Pt *p)
{
    u32 delta[L], gamma[L], beta[L], alpha[L], t1[L], t2[L], x3[L], y3[L], z3[L];

    if (fzero(p->z) || fzero(p->y)) {             /* nowhere, doubled: nowhere */
        int i;
        for (i = 0; i < s_w; i++) r->z[i] = 0;
        return;
    }
    fmul(delta, p->z, p->z);
    fmul(gamma, p->y, p->y);
    fmul(beta, p->x, gamma);
    fsub(t1, p->x, delta);
    fadd(t2, p->x, delta);
    fmul(alpha, t1, t2);
    fadd(t1, alpha, alpha); fadd(alpha, t1, alpha);          /* 3(x-d)(x+d)  */

    fmul(x3, alpha, alpha);
    fadd(t1, beta, beta); fadd(t1, t1, t1); fadd(t2, t1, t1); /* t1=4b t2=8b */
    fsub(x3, x3, t2);

    fadd(z3, p->y, p->z);
    fmul(z3, z3, z3);
    fsub(z3, z3, gamma);
    fsub(z3, z3, delta);

    fsub(y3, t1, x3);
    fmul(y3, alpha, y3);
    fmul(t2, gamma, gamma);
    fadd(t2, t2, t2); fadd(t2, t2, t2); fadd(t2, t2, t2);   /* 8 gamma^2    */
    fsub(y3, y3, t2);

    fcopy(r->x, x3); fcopy(r->y, y3); fcopy(r->z, z3);
}

static void pt_add(Pt *r, const Pt *p, const Pt *q)
{
    u32 z1z1[L], z2z2[L], u1[L], u2[L], s1[L], s2[L], h[L], rr[L];
    u32 hh[L], hhh[L], v[L], x3[L], y3[L], z3[L], t[L];

    if (fzero(p->z)) { if (r != q) *r = *q; return; }
    if (fzero(q->z)) { if (r != p) *r = *p; return; }

    fmul(z1z1, p->z, p->z);
    fmul(z2z2, q->z, q->z);
    fmul(u1, p->x, z2z2);
    fmul(u2, q->x, z1z1);
    fmul(t, q->z, z2z2); fmul(s1, p->y, t);
    fmul(t, p->z, z1z1); fmul(s2, q->y, t);
    fsub(h, u2, u1);
    fsub(rr, s2, s1);
    if (fzero(h)) {
        if (fzero(rr)) { pt_double(r, p); return; }
        { int i; for (i = 0; i < s_w; i++) r->z[i] = 0; }  /* P + -P        */
        return;
    }
    fmul(hh, h, h);
    fmul(hhh, h, hh);
    fmul(v, u1, hh);

    fmul(x3, rr, rr);
    fsub(x3, x3, hhh);
    fsub(x3, x3, v);
    fsub(x3, x3, v);

    fsub(t, v, x3);
    fmul(y3, rr, t);
    fmul(t, s1, hhh);
    fsub(y3, y3, t);

    fmul(z3, p->z, q->z);
    fmul(z3, z3, h);

    fcopy(r->x, x3); fcopy(r->y, y3); fcopy(r->z, z3);
}

/* An affine point from big-endian x and y, checked to be on the curve. */
static int pt_load(Pt *r, const u8 *xy, int bytes)
{
    u32 x[L], y[L], t[L], u[L];
    int i;

    if (!bn_from_be(x, s_w, xy, bytes) || !bn_from_be(y, s_w, xy + bytes, bytes)) return 0;
    if (bn_cmp(x, s_fp.m, s_w) >= 0 || bn_cmp(y, s_fp.m, s_w) >= 0) return 0;
    mont_to(&s_fp, r->x, x);
    mont_to(&s_fp, r->y, y);
    for (i = 0; i < s_w; i++) r->z[i] = s_fp.one[i];

    /* y^2 = x^3 - 3x + b */
    fmul(t, r->x, r->x);
    fmul(t, t, r->x);
    fsub(t, t, r->x); fsub(t, t, r->x); fsub(t, t, r->x);
    fadd(t, t, s_b);
    fmul(u, r->y, r->y);
    return bn_cmp(t, u, s_w) == 0;
}

static int bit(const u32 *a, int i) { return (int)(a[i / 32] >> (i % 32)) & 1; }

/* Reads a DER INTEGER at *p, as a number below the order.  0 if it is not
 * one, or is zero, or is not below the order. */
static int der_int(const u8 **p, const u8 *end, u32 *out)
{
    const u8 *q = *p;
    int len;
    if (end - q < 2 || q[0] != 0x02) return 0;
    len = q[1];
    q += 2;
    if (len & 0x80 || len == 0 || end - q < len) return 0;
    if (!bn_from_be(out, s_w, q, len)) return 0;
    *p = q + len;
    return !bn_is_zero(out, s_w) && bn_cmp(out, s_fn.m, s_w) < 0;
}

/* curve: 0 for P-256, 1 for P-384.  pub: 04 || X || Y.  sig: DER
 * SEQUENCE { r INTEGER, s INTEGER }.  hash: any length; the leftmost bits
 * that fit the order are what count. */
int ecdsa_verify(int curve, const u8 *pub, int publen,
                 const u8 *sig, int siglen, const u8 *hash, int hlen)
{
    static Pt G, Q, GQ, R;
    const Curve *C;
    u32 r[L], s[L], e[L], w[L], u1[L], u2[L], t[L], b[L];
    u8 exp[48];
    const u8 *p = sig, *end = sig + siglen;
    int bytes, i, len, top;

    if (curve < 0 || curve > 1) return 0;
    C = &s_curves[curve];
    bytes = C->bytes;
    if (!mont_init(&s_fp, C->p, bytes) || !mont_init(&s_fn, C->n, bytes)) return 0;
    s_w = s_fp.n;
    bn_from_be(b, s_w, C->b, bytes);
    mont_to(&s_fp, s_b, b);

    /* The signature. */
    if (siglen < 8 || p[0] != 0x30) return 0;
    len = p[1]; p += 2;
    if (len & 0x80) { if (len != 0x81 || end - p < 1) return 0; len = *p++; }
    if (end - p != len) return 0;
    if (!der_int(&p, end, r) || !der_int(&p, end, s) || p != end) return 0;

    /* The keys. */
    if (publen != 1 + 2 * bytes || pub[0] != 0x04) return 0;
    if (!pt_load(&Q, pub + 1, bytes) || !pt_load(&G, C->g, bytes)) return 0;

    /* e: the hash, as many leftmost bytes as the order has, below n. */
    if (!bn_from_be(e, s_w, hash, hlen < bytes ? hlen : bytes)) return 0;
    if (bn_cmp(e, s_fn.m, s_w) >= 0) bn_sub(e, e, s_fn.m, s_w);

    /* w = 1/s, as s^(n-2); u1 = e w, u2 = r w, all modulo n. */
    for (i = 0; i < bytes; i++) exp[i] = C->n[i];
    for (i = bytes - 1; i >= 0; i--) {                      /* n - 2          */
        int sub = i == bytes - 1 ? 2 : 1;
        if (exp[i] >= sub) { exp[i] = (u8)(exp[i] - sub); break; }
        exp[i] = (u8)(exp[i] + 256 - sub);
    }
    mont_pow(&s_fn, w, s, exp, bytes);
    mont_to(&s_fn, t, e);  mont_mul(&s_fn, u1, t, w);
    mont_to(&s_fn, t, r);  mont_mul(&s_fn, u2, t, w);

    /* u1 G + u2 Q, both at once. */
    pt_add(&GQ, &G, &Q);
    for (i = 0; i < s_w; i++) R.z[i] = 0;
    top = bn_bits(u1, s_w) > bn_bits(u2, s_w) ? bn_bits(u1, s_w) : bn_bits(u2, s_w);
    for (i = top - 1; i >= 0; i--) {
        int a = bit(u1, i), c = bit(u2, i);
        pt_double(&R, &R);
        if (a && c) pt_add(&R, &R, &GQ);
        else if (a) pt_add(&R, &R, &G);
        else if (c) pt_add(&R, &R, &Q);
    }
    if (fzero(R.z)) return 0;

    /* x = X / Z^2, then reduced modulo n, must be r. */
    for (i = 0; i < bytes; i++) exp[i] = C->p[i];
    for (i = bytes - 1; i >= 0; i--) {                      /* p - 2          */
        int sub = i == bytes - 1 ? 2 : 1;
        if (exp[i] >= sub) { exp[i] = (u8)(exp[i] - sub); break; }
        exp[i] = (u8)(exp[i] + 256 - sub);
    }
    mont_from(&s_fp, t, R.z);
    mont_pow(&s_fp, w, t, exp, bytes);          /* 1/Z                        */
    mont_to(&s_fp, t, w);
    fmul(t, t, t);                              /* 1/Z^2, Montgomery form     */
    fmul(t, R.x, t);
    mont_from(&s_fp, e, t);                     /* x, ordinary form           */
    if (bn_cmp(e, s_fn.m, s_w) >= 0) bn_sub(e, e, s_fn.m, s_w);
    return bn_cmp(e, r, s_w) == 0;
}

/* ---- ECDH on P-256 --------------------------------------------------------
 * For TLS 1.2 servers that have no X25519.  k P by a doubling and an
 * addition at every bit, the sum kept or not by a mask, so the work is the
 * same whatever the secret's bits are. */
static void pt_mul(Pt *r, const Pt *p, const u32 *k)
{
    static Pt acc, sum;
    int i, j;
    for (j = 0; j < L; j++) acc.x[j] = acc.y[j] = acc.z[j] = 0;
    for (i = 255; i >= 0; i--) {
        u32 m = (u32)0 - (u32)bit(k, i);
        pt_double(&acc, &acc);
        pt_add(&sum, &acc, p);
        for (j = 0; j < s_w; j++) {
            acc.x[j] ^= m & (acc.x[j] ^ sum.x[j]);
            acc.y[j] ^= m & (acc.y[j] ^ sum.y[j]);
            acc.z[j] ^= m & (acc.z[j] ^ sum.z[j]);
        }
    }
    *r = acc;
}

/* A point's x and y, big-endian: X / Z^2 and Y / Z^3. */
static void affine(const Pt *pt, const Curve *C, u8 *xy)
{
    u32 t[L], w[L], zi[L], zz[L], v[L];
    u8 exp[48];
    int i, bytes = C->bytes;
    for (i = 0; i < bytes; i++) exp[i] = C->p[i];
    for (i = bytes - 1; i >= 0; i--) {                      /* p - 2          */
        int sub = i == bytes - 1 ? 2 : 1;
        if (exp[i] >= sub) { exp[i] = (u8)(exp[i] - sub); break; }
        exp[i] = (u8)(exp[i] + 256 - sub);
    }
    mont_from(&s_fp, t, pt->z);
    mont_pow(&s_fp, w, t, exp, bytes);                      /* 1/Z            */
    mont_to(&s_fp, zi, w);
    fmul(zz, zi, zi);
    fmul(t, pt->x, zz); mont_from(&s_fp, v, t); bn_to_be(xy, bytes, v, s_w);
    fmul(zz, zz, zi);
    fmul(t, pt->y, zz); mont_from(&s_fp, v, t); bn_to_be(xy + bytes, bytes, v, s_w);
}

/* Our public point for a secret (pub, 65 bytes, if asked), and the shared
 * x with the server's point (peer, 65 bytes, if given).  0 if the server's
 * point is not on the curve, or the result is nowhere. */
int ecdh_p256(const u8 secret[32], const u8 *peer, u8 shared[32], u8 pub[65])
{
    static Pt G, Q, R;
    const Curve *C = &s_curves[0];
    u32 k[L], b[L];
    u8 xy[64];
    int i;
    if (!mont_init(&s_fp, C->p, 32) || !mont_init(&s_fn, C->n, 32)) return 0;
    s_w = s_fp.n;
    bn_from_be(b, s_w, C->b, 32);
    mont_to(&s_fp, s_b, b);
    for (i = 0; i < L; i++) k[i] = 0;
    bn_from_be(k, s_w, secret, 32);
    if (bn_cmp(k, s_fn.m, s_w) >= 0) bn_sub(k, k, s_fn.m, s_w);
    if (bn_is_zero(k, s_w)) return 0;
    if (pub) {
        if (!pt_load(&G, C->g, 32)) return 0;
        pt_mul(&R, &G, k);
        affine(&R, C, xy);
        pub[0] = 4;
        for (i = 0; i < 64; i++) pub[1 + i] = xy[i];
    }
    if (peer) {
        if (peer[0] != 4 || !pt_load(&Q, peer + 1, 32)) return 0;
        pt_mul(&R, &Q, k);
        if (fzero(R.z)) return 0;
        affine(&R, C, xy);
        for (i = 0; i < 32; i++) shared[i] = xy[i];
    }
    return 1;
}
