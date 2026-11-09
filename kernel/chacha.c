/* ===========================================================================
 * chacha.c -- ChaCha20, Poly1305, and the two together (RFC 8439).
 *
 * The cipher this machine encrypts TLS with.  ChaCha20 is additions,
 * exclusive-ors and rotations of 32-bit words and nothing else -- no
 * tables, no multiplication -- which is exactly the arithmetic a 1996
 * Pentium is good at, and why it is used here rather than AES, whose
 * fast versions want instructions this processor does not have.
 *
 * Poly1305 is the seal: a number computed over everything sent, modulo
 * 2^130 - 5, that the other end recomputes.  If one bit of a record was
 * changed on the way, the numbers do not match and the record is thrown
 * away rather than believed.  The arithmetic is done in five 26-bit
 * pieces so every product fits in 64 bits.
 *
 * Written from RFC 8439 and checked against its test vectors by the
 * `crypto` instrument.
 * ===========================================================================
 */
#include "flopos.h"

#define ROTL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
#define QR(a, b, c, d) \
    a += b; d ^= a; d = ROTL(d, 16); c += d; b ^= c; b = ROTL(b, 12); \
    a += b; d ^= a; d = ROTL(d, 8);  c += d; b ^= c; b = ROTL(b, 7)

static u32 le32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static void put_le32(u8 *p, u32 v)
{
    p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24);
}

void chacha20_block(const u8 key[32], u32 counter, const u8 nonce[12], u8 out[64])
{
    u32 s[16], x[16];
    int i;

    s[0] = 0x61707865; s[1] = 0x3320646e; s[2] = 0x79622d32; s[3] = 0x6b206574;
    for (i = 0; i < 8; i++) s[4 + i] = le32(key + 4 * i);
    s[12] = counter;
    for (i = 0; i < 3; i++) s[13 + i] = le32(nonce + 4 * i);

    for (i = 0; i < 16; i++) x[i] = s[i];
    for (i = 0; i < 10; i++) {
        QR(x[0], x[4], x[8],  x[12]); QR(x[1], x[5], x[9],  x[13]);
        QR(x[2], x[6], x[10], x[14]); QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]); QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[8],  x[13]); QR(x[3], x[4], x[9],  x[14]);
    }
    for (i = 0; i < 16; i++) put_le32(out + 4 * i, x[i] + s[i]);
}

void chacha20_xor(const u8 key[32], u32 counter, const u8 nonce[12],
                  const u8 *in, u8 *out, u32 len)
{
    u8 ks[64];
    u32 i, j;

    for (i = 0; i < len; i += 64) {
        chacha20_block(key, counter++, nonce, ks);
        for (j = 0; j < 64 && i + j < len; j++) out[i + j] = in[i + j] ^ ks[j];
    }
}

/* ---- Poly1305 ----------------------------------------------------------- */
void poly1305_init(Poly1305 *p, const u8 key[32])
{
    /* r, with the bits the definition says must be zero cleared. */
    p->r[0] = (le32(key + 0))      & 0x3ffffff;
    p->r[1] = (le32(key + 3) >> 2) & 0x3ffff03;
    p->r[2] = (le32(key + 6) >> 4) & 0x3ffc0ff;
    p->r[3] = (le32(key + 9) >> 6) & 0x3f03fff;
    p->r[4] = (le32(key + 12) >> 8) & 0x00fffff;
    p->h[0] = p->h[1] = p->h[2] = p->h[3] = p->h[4] = 0;
    p->pad[0] = le32(key + 16); p->pad[1] = le32(key + 20);
    p->pad[2] = le32(key + 24); p->pad[3] = le32(key + 28);
    p->fill = 0;
}

static void poly_block(Poly1305 *p, const u8 *m, u32 hibit)
{
    u32 r0 = p->r[0], r1 = p->r[1], r2 = p->r[2], r3 = p->r[3], r4 = p->r[4];
    u32 s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;
    u32 h0 = p->h[0], h1 = p->h[1], h2 = p->h[2], h3 = p->h[3], h4 = p->h[4];
    u64 d0, d1, d2, d3, d4;
    u32 c;

    h0 += (le32(m + 0))      & 0x3ffffff;
    h1 += (le32(m + 3) >> 2) & 0x3ffffff;
    h2 += (le32(m + 6) >> 4) & 0x3ffffff;
    h3 += (le32(m + 9) >> 6) & 0x3ffffff;
    h4 += (le32(m + 12) >> 8) | hibit;

    d0 = (u64)h0 * r0 + (u64)h1 * s4 + (u64)h2 * s3 + (u64)h3 * s2 + (u64)h4 * s1;
    d1 = (u64)h0 * r1 + (u64)h1 * r0 + (u64)h2 * s4 + (u64)h3 * s3 + (u64)h4 * s2;
    d2 = (u64)h0 * r2 + (u64)h1 * r1 + (u64)h2 * r0 + (u64)h3 * s4 + (u64)h4 * s3;
    d3 = (u64)h0 * r3 + (u64)h1 * r2 + (u64)h2 * r1 + (u64)h3 * r0 + (u64)h4 * s4;
    d4 = (u64)h0 * r4 + (u64)h1 * r3 + (u64)h2 * r2 + (u64)h3 * r1 + (u64)h4 * r0;

    c = (u32)(d0 >> 26); h0 = (u32)d0 & 0x3ffffff; d1 += c;
    c = (u32)(d1 >> 26); h1 = (u32)d1 & 0x3ffffff; d2 += c;
    c = (u32)(d2 >> 26); h2 = (u32)d2 & 0x3ffffff; d3 += c;
    c = (u32)(d3 >> 26); h3 = (u32)d3 & 0x3ffffff; d4 += c;
    c = (u32)(d4 >> 26); h4 = (u32)d4 & 0x3ffffff;
    h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffff; h1 += c;

    p->h[0] = h0; p->h[1] = h1; p->h[2] = h2; p->h[3] = h3; p->h[4] = h4;
}

void poly1305_update(Poly1305 *p, const u8 *m, u32 n)
{
    while (n--) {
        p->buf[p->fill++] = *m++;
        if (p->fill == 16) { poly_block(p, p->buf, 1u << 24); p->fill = 0; }
    }
}

void poly1305_final(Poly1305 *p, u8 tag[16])
{
    u32 h0, h1, h2, h3, h4, c, g0, g1, g2, g3, g4, mask;
    u64 f;

    if (p->fill) {                       /* the last short block: a 1 after it */
        int i = p->fill;
        p->buf[i++] = 1;
        while (i < 16) p->buf[i++] = 0;
        poly_block(p, p->buf, 0);
    }

    h0 = p->h[0]; h1 = p->h[1]; h2 = p->h[2]; h3 = p->h[3]; h4 = p->h[4];
    c = h1 >> 26; h1 &= 0x3ffffff; h2 += c;
    c = h2 >> 26; h2 &= 0x3ffffff; h3 += c;
    c = h3 >> 26; h3 &= 0x3ffffff; h4 += c;
    c = h4 >> 26; h4 &= 0x3ffffff; h0 += c * 5;
    c = h0 >> 26; h0 &= 0x3ffffff; h1 += c;

    /* h - (2^130 - 5): if that does not go negative, it is the answer. */
    g0 = h0 + 5; c = g0 >> 26; g0 &= 0x3ffffff;
    g1 = h1 + c; c = g1 >> 26; g1 &= 0x3ffffff;
    g2 = h2 + c; c = g2 >> 26; g2 &= 0x3ffffff;
    g3 = h3 + c; c = g3 >> 26; g3 &= 0x3ffffff;
    g4 = h4 + c - (1u << 26);

    mask = (g4 >> 31) - 1;               /* all ones if g did not go negative */
    h0 = (h0 & ~mask) | (g0 & mask);
    h1 = (h1 & ~mask) | (g1 & mask);
    h2 = (h2 & ~mask) | (g2 & mask);
    h3 = (h3 & ~mask) | (g3 & mask);
    h4 = (h4 & ~mask) | (g4 & mask);

    /* Back into four 32-bit words, and the second half of the key added. */
    h0 = h0 | (h1 << 26);
    h1 = (h1 >> 6) | (h2 << 20);
    h2 = (h2 >> 12) | (h3 << 14);
    h3 = (h3 >> 18) | (h4 << 8);

    f = (u64)h0 + p->pad[0];             put_le32(tag + 0, (u32)f);
    f = (u64)h1 + p->pad[1] + (f >> 32); put_le32(tag + 4, (u32)f);
    f = (u64)h2 + p->pad[2] + (f >> 32); put_le32(tag + 8, (u32)f);
    f = (u64)h3 + p->pad[3] + (f >> 32); put_le32(tag + 12, (u32)f);
}

/* ---- the two together (RFC 8439 section 2.8) ---------------------------- */
static void aead_tag(const u8 key[32], const u8 nonce[12],
                     const u8 *aad, u32 alen, const u8 *ct, u32 clen, u8 tag[16])
{
    static const u8 zeros[16];
    u8 block0[64], lens[16];
    Poly1305 p;

    chacha20_block(key, 0, nonce, block0);          /* the one-time seal key */
    poly1305_init(&p, block0);
    poly1305_update(&p, aad, alen);
    if (alen % 16) poly1305_update(&p, zeros, 16 - alen % 16);
    poly1305_update(&p, ct, clen);
    if (clen % 16) poly1305_update(&p, zeros, 16 - clen % 16);
    put_le32(lens + 0, alen); put_le32(lens + 4, 0);
    put_le32(lens + 8, clen); put_le32(lens + 12, 0);
    poly1305_update(&p, lens, 16);
    poly1305_final(&p, tag);
}

void aead_seal(const u8 key[32], const u8 nonce[12], const u8 *aad, u32 alen,
               const u8 *pt, u32 len, u8 *ct, u8 tag[16])
{
    chacha20_xor(key, 1, nonce, pt, ct, len);
    aead_tag(key, nonce, aad, alen, ct, len, tag);
}

/* Returns 1 and the plaintext if the seal holds; 0 and nothing if not. */
int aead_open(const u8 key[32], const u8 nonce[12], const u8 *aad, u32 alen,
              const u8 *ct, u32 len, const u8 tag[16], u8 *pt)
{
    u8 want[16];
    u8 diff = 0;
    int i;

    aead_tag(key, nonce, aad, alen, ct, len, want);
    for (i = 0; i < 16; i++) diff |= (u8)(want[i] ^ tag[i]);   /* no early out */
    if (diff) return 0;
    chacha20_xor(key, 1, nonce, ct, pt, len);
    return 1;
}
