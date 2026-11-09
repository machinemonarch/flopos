/* ===========================================================================
 * sha3.c -- Keccak: SHA3-256, SHA3-512, SHAKE128 and SHAKE256 (FIPS 202).
 *
 * The other family of hash, built a different way from SHA-256: a 1600-
 * bit state that input is stirred into and output squeezed out of, as
 * much as is wanted.  ML-KEM uses all four -- SHA3-256 and SHA3-512 to
 * derive things, SHAKE128 as an endless stream of numbers to build its
 * matrix from, SHAKE256 for its noise -- so this is the ground the
 * post-quantum key exchange stands on.
 *
 * The state is twenty-five 64-bit lanes.  On a 32-bit Pentium every lane
 * operation is two, which is slow by modern standards and fine here.
 * Written from FIPS 202; checked against Python's hashlib on random
 * inputs of every awkward length.
 * ===========================================================================
 */
#include "flopos.h"

static const u64 RC[24] = {
    0x0000000000000001ULL, 0x0000000000008082ULL, 0x800000000000808AULL,
    0x8000000080008000ULL, 0x000000000000808BULL, 0x0000000080000001ULL,
    0x8000000080008081ULL, 0x8000000000008009ULL, 0x000000000000008AULL,
    0x0000000000000088ULL, 0x0000000080008009ULL, 0x000000008000000AULL,
    0x000000008000808BULL, 0x800000000000008BULL, 0x8000000000008089ULL,
    0x8000000000008003ULL, 0x8000000000008002ULL, 0x8000000000000080ULL,
    0x000000000000800AULL, 0x800000008000000AULL, 0x8000000080008081ULL,
    0x8000000000008080ULL, 0x0000000080000001ULL, 0x8000000080008008ULL
};

/* Where each lane's bits are rotated to, and where the lane moves:
 * worked out once from the definitions in FIPS 202 section 3.2. */
static const u8 ROT[25] = {
     0,  1, 62, 28, 27,
    36, 44,  6, 55, 20,
     3, 10, 43, 25, 39,
    41, 45, 15, 21,  8,
    18,  2, 61, 56, 14
};

static u64 rol(u64 x, int n) { return n ? (x << n) | (x >> (64 - n)) : x; }

static void keccakf(u64 a[25])
{
    u64 c[5], d, b[25];
    int r, x, y;

    for (r = 0; r < 24; r++) {
        /* theta: every column's parity, folded into its neighbours */
        for (x = 0; x < 5; x++)
            c[x] = a[x] ^ a[x + 5] ^ a[x + 10] ^ a[x + 15] ^ a[x + 20];
        for (x = 0; x < 5; x++) {
            d = c[(x + 4) % 5] ^ rol(c[(x + 1) % 5], 1);
            for (y = 0; y < 25; y += 5) a[y + x] ^= d;
        }
        /* rho and pi: rotate each lane, and move it to (y, 2x + 3y) */
        for (x = 0; x < 5; x++)
            for (y = 0; y < 5; y++)
                b[((2 * x + 3 * y) % 5) * 5 + y] = rol(a[y * 5 + x], ROT[y * 5 + x]);
        /* chi: the only part that is not linear */
        for (y = 0; y < 25; y += 5)
            for (x = 0; x < 5; x++)
                a[y + x] = b[y + x] ^ (~b[y + (x + 1) % 5] & b[y + (x + 2) % 5]);
        /* iota */
        a[0] ^= RC[r];
    }
}

void keccak_init(Keccak *k, int rate, u8 pad)
{
    int i;
    for (i = 0; i < 25; i++) k->s[i] = 0;
    k->rate = rate;
    k->pos = 0;
    k->pad = pad;
    k->squeezing = 0;
}

static void xor_byte(Keccak *k, int i, u8 v)
{
    k->s[i / 8] ^= (u64)v << (8 * (i % 8));
}

static u8 get_byte(const Keccak *k, int i)
{
    return (u8)(k->s[i / 8] >> (8 * (i % 8)));
}

void keccak_absorb(Keccak *k, const u8 *in, u32 len)
{
    while (len--) {
        xor_byte(k, k->pos++, *in++);
        if (k->pos == k->rate) { keccakf(k->s); k->pos = 0; }
    }
}

void keccak_squeeze(Keccak *k, u8 *out, u32 len)
{
    if (!k->squeezing) {
        xor_byte(k, k->pos, k->pad);
        xor_byte(k, k->rate - 1, 0x80);
        keccakf(k->s);
        k->pos = 0;
        k->squeezing = 1;
    }
    while (len--) {
        if (k->pos == k->rate) { keccakf(k->s); k->pos = 0; }
        *out++ = get_byte(k, k->pos++);
    }
}

void sha3_256(const u8 *in, u32 len, u8 out[32])
{
    Keccak k;
    keccak_init(&k, 136, 0x06); keccak_absorb(&k, in, len); keccak_squeeze(&k, out, 32);
}

void sha3_512(const u8 *in, u32 len, u8 out[64])
{
    Keccak k;
    keccak_init(&k, 72, 0x06); keccak_absorb(&k, in, len); keccak_squeeze(&k, out, 64);
}

void shake128(const u8 *in, u32 len, u8 *out, u32 olen)
{
    Keccak k;
    keccak_init(&k, 168, 0x1F); keccak_absorb(&k, in, len); keccak_squeeze(&k, out, olen);
}

void shake256(const u8 *in, u32 len, u8 *out, u32 olen)
{
    Keccak k;
    keccak_init(&k, 136, 0x1F); keccak_absorb(&k, in, len); keccak_squeeze(&k, out, olen);
}
