/* ===========================================================================
 * sha256.c -- SHA-256, HMAC and HKDF.
 *
 * The hash everything in TLS 1.3 leans on: every key in a connection is
 * derived from the handshake by HMAC over SHA-256 (HKDF, RFC 5869), and
 * the handshake itself is checked by a running SHA-256 of every message
 * in it.  Written from FIPS 180-4 and checked against its test vectors by
 * the `crypto` instrument -- nothing that depends on this is trusted until
 * that says so.
 * ===========================================================================
 */
#include "flopos.h"

static const u32 K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void block(Sha256 *c, const u8 *p)
{
    u32 w[64], a, b, d, e, f, g, h, cc, t1, t2;
    int i;

    for (i = 0; i < 16; i++)
        w[i] = ((u32)p[i * 4] << 24) | ((u32)p[i * 4 + 1] << 16) |
               ((u32)p[i * 4 + 2] << 8) | p[i * 4 + 3];
    for (i = 16; i < 64; i++) {
        u32 s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        u32 s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3];
    e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];
    for (i = 0; i < 64; i++) {
        t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & cc) ^ (b & cc));
        h = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
    c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

void sha256_init(Sha256 *c)
{
    c->h[0] = 0x6a09e667; c->h[1] = 0xbb67ae85; c->h[2] = 0x3c6ef372;
    c->h[3] = 0xa54ff53a; c->h[4] = 0x510e527f; c->h[5] = 0x9b05688c;
    c->h[6] = 0x1f83d9ab; c->h[7] = 0x5be0cd19;
    c->len = 0;
    c->fill = 0;
}

void sha256_update(Sha256 *c, const u8 *p, u32 n)
{
    while (n--) {
        c->buf[c->fill++] = *p++;
        c->len++;
        if (c->fill == 64) { block(c, c->buf); c->fill = 0; }
    }
}

void sha256_final(Sha256 *c, u8 out[32])
{
    u32 bits_hi = c->len >> 29, bits_lo = c->len << 3;
    int i;

    c->buf[c->fill++] = 0x80;
    if (c->fill > 56) {
        while (c->fill < 64) c->buf[c->fill++] = 0;
        block(c, c->buf);
        c->fill = 0;
    }
    while (c->fill < 56) c->buf[c->fill++] = 0;
    for (i = 0; i < 4; i++) c->buf[56 + i] = (u8)(bits_hi >> (24 - 8 * i));
    for (i = 0; i < 4; i++) c->buf[60 + i] = (u8)(bits_lo >> (24 - 8 * i));
    block(c, c->buf);
    for (i = 0; i < 32; i++) out[i] = (u8)(c->h[i / 4] >> (24 - 8 * (i % 4)));
}

void sha256(const u8 *p, u32 n, u8 out[32])
{
    Sha256 c;
    sha256_init(&c);
    sha256_update(&c, p, n);
    sha256_final(&c, out);
}

/* ---- HMAC (RFC 2104) --------------------------------------------------- */
void hmac_sha256(const u8 *key, u32 klen, const u8 *msg, u32 mlen, u8 out[32])
{
    u8 k[64], pad[64], inner[32];
    Sha256 c;
    int i;

    for (i = 0; i < 64; i++) k[i] = 0;
    if (klen > 64) sha256(key, klen, k);
    else for (i = 0; i < (int)klen; i++) k[i] = key[i];

    for (i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    sha256_init(&c);
    sha256_update(&c, pad, 64);
    sha256_update(&c, msg, mlen);
    sha256_final(&c, inner);

    for (i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
    sha256_init(&c);
    sha256_update(&c, pad, 64);
    sha256_update(&c, inner, 32);
    sha256_final(&c, out);
}

/* ---- PBKDF2 (RFC 8018), one block of it: a passphrase made into a key ----
 * The passphrase is run through HMAC `iters` times so that each guess
 * costs a guesser that much work.  The two halves of the HMAC key are
 * hashed once and kept, so a round is two hashes of 32 bytes. */
void pbkdf2_sha256(const u8 *pw, u32 plen, const u8 *salt, u32 slen, u32 iters, u8 out[32])
{
    u8 k[64], pad[64], u[32], t[32];
    Sha256 in, outer, c;
    u32 i, n;

    for (i = 0; i < 64; i++) k[i] = 0;
    if (plen > 64) sha256(pw, plen, k);
    else for (i = 0; i < plen; i++) k[i] = pw[i];
    for (i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    sha256_init(&in); sha256_update(&in, pad, 64);
    for (i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
    sha256_init(&outer); sha256_update(&outer, pad, 64);

    c = in;
    sha256_update(&c, salt, slen);
    t[0] = 0; t[1] = 0; t[2] = 0; t[3] = 1;          /* block number one */
    sha256_update(&c, t, 4);
    sha256_final(&c, t);
    c = outer; sha256_update(&c, t, 32); sha256_final(&c, u);
    for (i = 0; i < 32; i++) out[i] = u[i];
    for (n = 1; n < iters; n++) {
        c = in;    sha256_update(&c, u, 32); sha256_final(&c, t);
        c = outer; sha256_update(&c, t, 32); sha256_final(&c, u);
        for (i = 0; i < 32; i++) out[i] ^= u[i];
    }
    for (i = 0; i < 64; i++) k[i] = pad[i] = 0;
}

/* ---- HKDF (RFC 5869) ----------------------------------------------------
 * Extract squeezes whatever randomness there is into one key; expand
 * stretches a key into as many bytes as are wanted, labelled with what
 * they are for, so the same secret gives unrelated keys for unrelated
 * jobs.  TLS 1.3 derives every one of its keys with these two. */
void hkdf_extract(const u8 *salt, u32 slen, const u8 *ikm, u32 ilen, u8 prk[32])
{
    static const u8 zero[32];
    if (!salt || !slen) { salt = zero; slen = 32; }
    hmac_sha256(salt, slen, ikm, ilen, prk);
}

void hkdf_expand(const u8 prk[32], const u8 *info, u32 ilen, u8 *out, u32 olen)
{
    u8 t[32], buf[32 + 255 + 1];
    u32 done = 0, tlen = 0, i;
    u8 ctr = 1;

    while (done < olen) {
        u32 n = 0;
        for (i = 0; i < tlen; i++) buf[n++] = t[i];
        for (i = 0; i < ilen && i < 255; i++) buf[n++] = info[i];
        buf[n++] = ctr++;
        hmac_sha256(prk, 32, buf, n, t);
        tlen = 32;
        for (i = 0; i < 32 && done < olen; i++) out[done++] = t[i];
    }
}

/* ---- SHA-1 (FIPS 180-4), for the message checks of the two TLS 1.2 CBC
 * ciphers browzer still speaks, as Chrome does.  Never for signatures or
 * certificates: those refuse SHA-1. */
static u32 rol(u32 x, int n) { return (x << n) | (x >> (32 - n)); }

static void sha1_block(u32 h[5], const u8 *p)
{
    u32 w[80], a, b, c, d, e, f, k, t;
    int i;
    for (i = 0; i < 16; i++) w[i] = ((u32)p[4 * i] << 24) | ((u32)p[4 * i + 1] << 16) | ((u32)p[4 * i + 2] << 8) | p[4 * i + 3];
    for (i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4];
    for (i = 0; i < 80; i++) {
        if (i < 20)      { f = (b & c) | (~b & d);          k = 0x5A827999u; }
        else if (i < 40) { f = b ^ c ^ d;                    k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d);  k = 0x8F1BBCDCu; }
        else             { f = b ^ c ^ d;                    k = 0xCA62C1D6u; }
        t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

void sha1_init(Sha1 *c)
{
    c->h[0] = 0x67452301u; c->h[1] = 0xEFCDAB89u; c->h[2] = 0x98BADCFEu;
    c->h[3] = 0x10325476u; c->h[4] = 0xC3D2E1F0u;
    c->len = 0; c->fill = 0;
}

void sha1_update(Sha1 *c, const u8 *p, u32 n)
{
    c->len += n;
    while (n--) {
        c->buf[c->fill++] = *p++;
        if (c->fill == 64) { sha1_block(c->h, c->buf); c->fill = 0; }
    }
}

void sha1_final(Sha1 *c, u8 out[20])
{
    u32 bits = c->len * 8;
    int i;
    c->buf[c->fill++] = 0x80;
    if (c->fill > 56) { while (c->fill < 64) c->buf[c->fill++] = 0; sha1_block(c->h, c->buf); c->fill = 0; }
    while (c->fill < 56) c->buf[c->fill++] = 0;
    c->buf[56] = c->buf[57] = c->buf[58] = 0;
    c->buf[59] = (u8)(c->len >> 29);
    c->buf[60] = (u8)(bits >> 24); c->buf[61] = (u8)(bits >> 16); c->buf[62] = (u8)(bits >> 8); c->buf[63] = (u8)bits;
    sha1_block(c->h, c->buf);
    for (i = 0; i < 5; i++) {
        out[4 * i] = (u8)(c->h[i] >> 24); out[4 * i + 1] = (u8)(c->h[i] >> 16);
        out[4 * i + 2] = (u8)(c->h[i] >> 8); out[4 * i + 3] = (u8)c->h[i];
    }
}

/* Work for nothing: n more compressions, so a short message takes as long
 * as a long one (see tls.c, on checking a CBC record). */
void sha1_waste(int n)
{
    static u8 junk[64];
    u32 h[5] = { 0, 0, 0, 0, 0 };
    while (n-- > 0) sha1_block(h, junk);
}
