/* ===========================================================================
 * mlkem.c -- ML-KEM-768 (FIPS 203), the post-quantum half of the key.
 *
 * X25519 rests on a problem a large enough quantum computer would solve,
 * which means a recording of today's traffic could be decrypted later.
 * ML-KEM rests on a different one -- recovering a secret from equations
 * that have had a little noise deliberately added to them -- for which no
 * quantum shortcut is known.  TLS uses the two together, X25519MLKEM768:
 * the connection is safe if either of them holds.
 *
 * The arithmetic is on polynomials of 256 coefficients modulo 3329, kept
 * in 16 bits each.  Multiplying two of them is done in the "NTT" form,
 * where it is just multiplying pairs of numbers, which is the whole
 * reason the scheme is fast enough for a 1996 Pentium.  The three
 * operations are key generation (ours), encapsulation (the server's, but
 * decapsulation has to redo it to check the server's work), and
 * decapsulation (ours).
 *
 * Written from FIPS 203.  Checked against NIST's own ACVP test vectors --
 * key generation, encapsulation, and decapsulation including the cases
 * where a tampered ciphertext has to be quietly turned away.
 * ===========================================================================
 */
#include "flopos.h"

#define Q    3329
#define K    3
#define ETA1 2
#define ETA2 2
#define DU   10
#define DV   4

typedef u16 Poly[256];

/* 17 to the power BitRev7(i), and the twiddles for multiplying in NTT
 * form, 17^(2 BitRev7(i) + 1).  Worked out once at first use. */
static u16 s_zeta[128], s_gamma[128];
static int s_tables;

static u32 bitrev7(u32 x)
{
    u32 r = 0;
    int i;
    for (i = 0; i < 7; i++) { r = (r << 1) | (x & 1); x >>= 1; }
    return r;
}

static u32 powmod(u32 b, u32 e)
{
    u32 r = 1;
    while (e) { if (e & 1) r = r * b % Q; b = b * b % Q; e >>= 1; }
    return r;
}

static void tables(void)
{
    int i;
    if (s_tables) return;
    for (i = 0; i < 128; i++) {
        s_zeta[i]  = (u16)powmod(17, bitrev7((u32)i));
        s_gamma[i] = (u16)powmod(17, 2 * bitrev7((u32)i) + 1);
    }
    s_tables = 1;
}

static u16 addq(u32 a, u32 b) { u32 r = a + b; return (u16)(r >= Q ? r - Q : r); }
static u16 subq(u32 a, u32 b) { return (u16)(a >= b ? a - b : a + Q - b); }
static u16 mulq(u32 a, u32 b) { return (u16)(a * b % Q); }

static void ntt(Poly f)
{
    int len, start, j, i = 1;
    for (len = 128; len >= 2; len >>= 1)
        for (start = 0; start < 256; start += 2 * len) {
            u32 z = s_zeta[i++];
            for (j = start; j < start + len; j++) {
                u16 t = mulq(z, f[j + len]);
                f[j + len] = subq(f[j], t);
                f[j] = addq(f[j], t);
            }
        }
}

static void intt(Poly f)
{
    int len, start, j, i = 127;
    for (len = 2; len <= 128; len <<= 1)
        for (start = 0; start < 256; start += 2 * len) {
            u32 z = s_zeta[i--];
            for (j = start; j < start + len; j++) {
                u16 t = f[j];
                f[j] = addq(t, f[j + len]);
                f[j + len] = mulq(z, subq(f[j + len], t));
            }
        }
    for (j = 0; j < 256; j++) f[j] = mulq(f[j], 3303);   /* 128^-1 mod q */
}

/* h += f * g, all in NTT form: 128 small products of degree-one pieces. */
static void mul_acc(Poly h, const Poly f, const Poly g)
{
    int i;
    for (i = 0; i < 128; i++) {
        u32 a0 = f[2 * i], a1 = f[2 * i + 1], b0 = g[2 * i], b1 = g[2 * i + 1];
        u16 c0 = addq(mulq(a0, b0), mulq(mulq(a1, b1), s_gamma[i]));
        u16 c1 = addq(mulq(a0, b1), mulq(a1, b0));
        h[2 * i] = addq(h[2 * i], c0);
        h[2 * i + 1] = addq(h[2 * i + 1], c1);
    }
}

/* ---- sampling ------------------------------------------------------------ */
/* A matrix entry, straight into NTT form, from SHAKE128(rho || j || i):
 * three bytes give two candidates of twelve bits; those below q are kept. */
static void sample_ntt(Poly a, const u8 rho[32], u8 j, u8 i)
{
    Keccak k;
    u8 seed[34], b[3];
    int n = 0;
    int x;

    for (x = 0; x < 32; x++) seed[x] = rho[x];
    seed[32] = j; seed[33] = i;
    keccak_init(&k, 168, 0x1F);
    keccak_absorb(&k, seed, 34);
    while (n < 256) {
        u32 d1, d2;
        keccak_squeeze(&k, b, 3);
        d1 = b[0] | ((u32)(b[1] & 15) << 8);
        d2 = (b[1] >> 4) | ((u32)b[2] << 4);
        if (d1 < Q) a[n++] = (u16)d1;
        if (d2 < Q && n < 256) a[n++] = (u16)d2;
    }
}

/* Small noise, from SHAKE256(seed || n): each coefficient the difference
 * of two pairs of bits, so it is -2..2. */
static void sample_cbd2(Poly f, const u8 seed[32], u8 n)
{
    u8 in[33], buf[128];
    int i;

    for (i = 0; i < 32; i++) in[i] = seed[i];
    in[32] = n;
    shake256(in, 33, buf, 128);
    for (i = 0; i < 256; i++) {
        u32 bits = (u32)buf[i / 2] >> (4 * (i & 1));
        u32 x = (bits & 1) + ((bits >> 1) & 1);
        u32 y = ((bits >> 2) & 1) + ((bits >> 3) & 1);
        f[i] = subq(x, y);
    }
}

/* ---- packing ------------------------------------------------------------- */
static void encode(u8 *out, const Poly f, int d)
{
    u32 acc = 0;
    int bits = 0, i, o = 0;
    for (i = 0; i < 256; i++) {
        acc |= (u32)f[i] << bits;
        bits += d;
        while (bits >= 8) { out[o++] = (u8)acc; acc >>= 8; bits -= 8; }
    }
}

static void decode(Poly f, const u8 *in, int d)
{
    u32 acc = 0, mask = (1u << d) - 1;
    int bits = 0, i, o = 0;
    for (i = 0; i < 256; i++) {
        while (bits < d) { acc |= (u32)in[o++] << bits; bits += 8; }
        f[i] = (u16)(acc & mask);
        acc >>= d;
        bits -= d;
        if (d == 12) f[i] = (u16)(f[i] % Q);
    }
}

static u16 compress(u32 x, int d)
{
    return (u16)((((x << d) + Q / 2) / Q) & ((1u << d) - 1));
}

static u16 decompress(u32 y, int d)
{
    return (u16)((y * Q + (1u << (d - 1))) >> d);
}

/* ---- the public-key scheme underneath ------------------------------------ */
static Poly s_a[K][K];                   /* the matrix, NTT form           */

static void make_matrix(const u8 rho[32])
{
    int i, j;
    for (i = 0; i < K; i++)
        for (j = 0; j < K; j++)
            sample_ntt(s_a[i][j], rho, (u8)j, (u8)i);
}

static void pke_keygen(const u8 d[32], u8 *ek, u8 *dk_pke)
{
    static Poly s[K], e[K], t;
    u8 in[33], g[64];
    int i, j;

    for (i = 0; i < 32; i++) in[i] = d[i];
    in[32] = K;
    sha3_512(in, 33, g);                 /* rho = g[0..31], sigma = g[32..] */
    make_matrix(g);

    for (i = 0; i < K; i++) { sample_cbd2(s[i], g + 32, (u8)i); ntt(s[i]); }
    for (i = 0; i < K; i++) { sample_cbd2(e[i], g + 32, (u8)(K + i)); ntt(e[i]); }

    for (i = 0; i < K; i++) {
        for (j = 0; j < 256; j++) t[j] = e[i][j];
        for (j = 0; j < K; j++) mul_acc(t, s_a[i][j], s[j]);
        encode(ek + 384 * i, t, 12);
        encode(dk_pke + 384 * i, s[i], 12);
    }
    for (i = 0; i < 32; i++) ek[384 * K + i] = g[i];
}

static void pke_encrypt(const u8 *ek, const u8 m[32], const u8 r[32], u8 *c)
{
    static Poly t[K], y[K], u, v, e;
    int i, j, x;

    for (i = 0; i < K; i++) decode(t[i], ek + 384 * i, 12);
    make_matrix(ek + 384 * K);

    for (i = 0; i < K; i++) { sample_cbd2(y[i], r, (u8)i); ntt(y[i]); }

    for (i = 0; i < K; i++) {
        for (x = 0; x < 256; x++) u[x] = 0;
        for (j = 0; j < K; j++) mul_acc(u, s_a[j][i], y[j]);   /* A transposed */
        intt(u);
        sample_cbd2(e, r, (u8)(K + i));
        for (x = 0; x < 256; x++) u[x] = compress(addq(u[x], e[x]), DU);
        encode(c + 320 * i, u, DU);
    }

    for (x = 0; x < 256; x++) v[x] = 0;
    for (j = 0; j < K; j++) mul_acc(v, t[j], y[j]);
    intt(v);
    sample_cbd2(e, r, (u8)(2 * K));
    for (x = 0; x < 256; x++) {
        u32 bit = (m[x / 8] >> (x % 8)) & 1;
        u16 mu = bit ? (Q + 1) / 2 : 0;
        v[x] = compress(addq(addq(v[x], e[x]), mu), DV);
    }
    encode(c + 320 * K, v, DV);
}

static void pke_decrypt(const u8 *dk_pke, const u8 *c, u8 m[32])
{
    static Poly s, u, w, v;
    int i, x;

    for (x = 0; x < 256; x++) w[x] = 0;
    for (i = 0; i < K; i++) {
        decode(u, c + 320 * i, DU);
        for (x = 0; x < 256; x++) u[x] = decompress(u[x], DU);
        ntt(u);
        decode(s, dk_pke + 384 * i, 12);
        mul_acc(w, s, u);
    }
    intt(w);
    decode(v, c + 320 * K, DV);
    for (i = 0; i < 32; i++) m[i] = 0;
    for (x = 0; x < 256; x++) {
        u16 d = subq(decompress(v[x], DV), w[x]);
        m[x / 8] |= (u8)(compress(d, 1) << (x % 8));
    }
}

/* ---- ML-KEM itself --------------------------------------------------------- */
void mlkem_keygen(const u8 d[32], const u8 z[32], u8 ek[MLKEM_EK], u8 dk[MLKEM_DK])
{
    int i;
    tables();
    pke_keygen(d, ek, dk);
    for (i = 0; i < MLKEM_EK; i++) dk[384 * K + i] = ek[i];
    sha3_256(ek, MLKEM_EK, dk + 384 * K + MLKEM_EK);
    for (i = 0; i < 32; i++) dk[384 * K + MLKEM_EK + 32 + i] = z[i];
}

void mlkem_encaps(const u8 ek[MLKEM_EK], const u8 m[32], u8 c[MLKEM_CT], u8 key[32])
{
    u8 in[64], g[64];
    int i;
    tables();
    for (i = 0; i < 32; i++) in[i] = m[i];
    sha3_256(ek, MLKEM_EK, in + 32);
    sha3_512(in, 64, g);
    pke_encrypt(ek, m, g + 32, c);
    for (i = 0; i < 32; i++) key[i] = g[i];
}

/* If the ciphertext is not one this key's owner could have been sent, the
 * answer is not an error -- it is a key derived from a secret and the
 * ciphertext, which matches nothing, so a tampered handshake simply fails
 * later without telling anyone why. */
void mlkem_decaps(const u8 dk[MLKEM_DK], const u8 c[MLKEM_CT], u8 key[32])
{
    static u8 c2[MLKEM_CT], jin[32 + MLKEM_CT];
    const u8 *ek = dk + 384 * K, *h = ek + MLKEM_EK, *z = h + 32;
    u8 m[32], in[64], g[64], kbar[32], diff = 0;
    int i;

    tables();
    pke_decrypt(dk, c, m);
    for (i = 0; i < 32; i++) { in[i] = m[i]; in[32 + i] = h[i]; }
    sha3_512(in, 64, g);
    for (i = 0; i < 32; i++) jin[i] = z[i];
    for (i = 0; i < MLKEM_CT; i++) jin[32 + i] = c[i];
    shake256(jin, 32 + MLKEM_CT, kbar, 32);
    pke_encrypt(ek, m, g + 32, c2);
    for (i = 0; i < MLKEM_CT; i++) diff |= (u8)(c[i] ^ c2[i]);
    {
        u8 mask = (u8)(-(s32)(diff != 0));          /* all ones if different */
        for (i = 0; i < 32; i++) key[i] = (u8)((g[i] & ~mask) | (kbar[i] & mask));
    }
}
