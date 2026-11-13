/* ===========================================================================
 * aes.c -- AES in GCM mode, with 128-bit and 256-bit keys (FIPS 197,
 * SP 800-38D).
 *
 * Every TLS 1.3 server has to speak this one; ChaCha20 is only
 * recommended.  Some large sites do not offer ChaCha20 at all, and offered
 * nothing else, they do not always say so -- they just stop answering.
 * So this is here for compatibility, and ChaCha20 is still offered first,
 * because on a processor with no AES instructions it is the faster of the
 * two by a distance.
 *
 * AES is done a byte at a time, with no lookup tables beyond the S-box,
 * which is built at first use from its definition -- the inverse in
 * GF(2^8) followed by a fixed affine step -- rather than typed in.  GCM's
 * authenticator, GHASH, is multiplication in GF(2^128), done a bit at a
 * time.  Neither is fast; both are short and easy to check, and were
 * checked against OpenSSL on random keys, nonces and lengths.
 * ===========================================================================
 */
#include "flopos.h"

static u8 s_sbox[256];
static int s_ready;

static u8 xtime(u8 a) { return (u8)((a << 1) ^ ((a & 0x80) ? 0x1b : 0)); }

static void build_sbox(void)
{
    u8 exp[256], log[256], x = 1;
    int i;

    /* 3 generates the multiplicative group: powers and logs of it give
     * every inverse. */
    for (i = 0; i < 255; i++) {
        exp[i] = x;
        log[x] = (u8)i;
        x = (u8)(x ^ xtime(x));          /* x * 3                          */
    }
    for (i = 0; i < 256; i++) {
        u8 inv = i ? exp[(255 - log[i]) % 255] : 0, s = inv;
        int r;
        for (r = 1; r <= 4; r++) s ^= (u8)((inv << r) | (inv >> (8 - r)));
        s_sbox[i] = (u8)(s ^ 0x63);
    }
    s_ready = 1;
}

/* A key, made ready: the round keys, and how many rounds (10 for a
 * 128-bit key, 14 for a 256-bit one). */
typedef struct { u8 rk[240]; int nr; } AesKey;

static void expand(const u8 *key, int klen, AesKey *K)
{
    u8 rcon = 1, *rk = K->rk;
    int i, total;

    if (!s_ready) build_sbox();
    K->nr = klen == 32 ? 14 : 10;
    total = (K->nr + 1) * 16;
    for (i = 0; i < klen; i++) rk[i] = key[i];
    for (i = klen; i < total; i += 4) {
        u8 t0 = rk[i - 4], t1 = rk[i - 3], t2 = rk[i - 2], t3 = rk[i - 1];
        if (i % klen == 0) {
            u8 tmp = t0;
            t0 = (u8)(s_sbox[t1] ^ rcon); t1 = s_sbox[t2]; t2 = s_sbox[t3]; t3 = s_sbox[tmp];
            rcon = xtime(rcon);
        } else if (klen == 32 && i % 32 == 16) {     /* the longer key's extra step */
            t0 = s_sbox[t0]; t1 = s_sbox[t1]; t2 = s_sbox[t2]; t3 = s_sbox[t3];
        }
        rk[i] = rk[i - klen] ^ t0; rk[i + 1] = rk[i - klen + 1] ^ t1;
        rk[i + 2] = rk[i - klen + 2] ^ t2; rk[i + 3] = rk[i - klen + 3] ^ t3;
    }
}

static void encrypt_block(const AesKey *K, const u8 in[16], u8 out[16])
{
    const u8 *rk = K->rk;
    u8 s[16], t[16];
    int r, i, c, nr = K->nr;

    for (i = 0; i < 16; i++) s[i] = in[i] ^ rk[i];
    for (r = 1; r <= nr; r++) {
        /* SubBytes and ShiftRows together: row i moves left by i. */
        for (c = 0; c < 4; c++)
            for (i = 0; i < 4; i++)
                t[c * 4 + i] = s_sbox[s[((c + i) % 4) * 4 + i]];
        if (r < nr) {
            for (c = 0; c < 4; c++) {        /* MixColumns                  */
                u8 *col = t + c * 4, a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
                u8 all = (u8)(a0 ^ a1 ^ a2 ^ a3);
                col[0] ^= (u8)(all ^ xtime((u8)(a0 ^ a1)));
                col[1] ^= (u8)(all ^ xtime((u8)(a1 ^ a2)));
                col[2] ^= (u8)(all ^ xtime((u8)(a2 ^ a3)));
                col[3] ^= (u8)(all ^ xtime((u8)(a3 ^ a0)));
            }
        }
        for (i = 0; i < 16; i++) s[i] = t[i] ^ rk[r * 16 + i];
    }
    for (i = 0; i < 16; i++) out[i] = s[i];
}

/* ---- GCM ------------------------------------------------------------------- */
static void gf_mul(u32 x[4], const u32 h[4])
{
    u32 z[4] = { 0, 0, 0, 0 }, v[4];
    int i, b;

    for (i = 0; i < 4; i++) v[i] = h[i];
    for (i = 0; i < 128; i++) {
        if ((x[i / 32] >> (31 - i % 32)) & 1) {
            z[0] ^= v[0]; z[1] ^= v[1]; z[2] ^= v[2]; z[3] ^= v[3];
        }
        b = (int)(v[3] & 1);
        v[3] = (v[3] >> 1) | (v[2] << 31);
        v[2] = (v[2] >> 1) | (v[1] << 31);
        v[1] = (v[1] >> 1) | (v[0] << 31);
        v[0] >>= 1;
        if (b) v[0] ^= 0xE1000000u;
    }
    for (i = 0; i < 4; i++) x[i] = z[i];
}

static u32 be32(const u8 *p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }

static void ghash_bytes(u32 y[4], const u32 h[4], const u8 *p, u32 len)
{
    u8 blk[16];
    u32 i, j;
    for (i = 0; i < len; i += 16) {
        for (j = 0; j < 16; j++) blk[j] = (i + j < len) ? p[i + j] : 0;
        for (j = 0; j < 4; j++) y[j] ^= be32(blk + 4 * j);
        gf_mul(y, h);
    }
}

static void gcm_tag(const AesKey *rk, const u8 iv[12], const u8 *aad, u32 alen,
                    const u8 *ct, u32 clen, u8 tag[16])
{
    u8 zero[16] = { 0 }, hb[16], j0[16], ej0[16];
    u32 h[4], y[4] = { 0, 0, 0, 0 }, lens[4];
    int i;

    encrypt_block(rk, zero, hb);
    for (i = 0; i < 4; i++) h[i] = be32(hb + 4 * i);
    ghash_bytes(y, h, aad, alen);
    ghash_bytes(y, h, ct, clen);
    lens[0] = 0; lens[1] = alen * 8; lens[2] = 0; lens[3] = clen * 8;
    for (i = 0; i < 4; i++) y[i] ^= lens[i];
    gf_mul(y, h);

    for (i = 0; i < 12; i++) j0[i] = iv[i];
    j0[12] = 0; j0[13] = 0; j0[14] = 0; j0[15] = 1;
    encrypt_block(rk, j0, ej0);
    for (i = 0; i < 16; i++) tag[i] = (u8)((y[i / 4] >> (24 - 8 * (i % 4))) ^ ej0[i]);
}

static void gcm_ctr(const AesKey *rk, const u8 iv[12], const u8 *in, u8 *out, u32 len)
{
    u8 ctr[16], ks[16];
    u32 i, j, n = 2;

    for (i = 0; i < 12; i++) ctr[i] = iv[i];
    for (i = 0; i < len; i += 16) {
        ctr[12] = (u8)(n >> 24); ctr[13] = (u8)(n >> 16); ctr[14] = (u8)(n >> 8); ctr[15] = (u8)n;
        n++;
        encrypt_block(rk, ctr, ks);
        for (j = 0; j < 16 && i + j < len; j++) out[i + j] = in[i + j] ^ ks[j];
    }
}

static void seal(const u8 *key, int klen, const u8 iv[12], const u8 *aad, u32 alen,
                 const u8 *pt, u32 len, u8 *ct, u8 tag[16])
{
    AesKey K;
    expand(key, klen, &K);
    gcm_ctr(&K, iv, pt, ct, len);
    gcm_tag(&K, iv, aad, alen, ct, len, tag);
}

static int open(const u8 *key, int klen, const u8 iv[12], const u8 *aad, u32 alen,
                const u8 *ct, u32 len, const u8 tag[16], u8 *pt)
{
    AesKey K;
    u8 want[16], diff = 0;
    int i;
    expand(key, klen, &K);
    gcm_tag(&K, iv, aad, alen, ct, len, want);
    for (i = 0; i < 16; i++) diff |= (u8)(want[i] ^ tag[i]);
    if (diff) return 0;
    gcm_ctr(&K, iv, ct, pt, len);
    return 1;
}

void aesgcm_seal(const u8 key[16], const u8 iv[12], const u8 *aad, u32 alen,
                 const u8 *pt, u32 len, u8 *ct, u8 tag[16])
{ seal(key, 16, iv, aad, alen, pt, len, ct, tag); }

int aesgcm_open(const u8 key[16], const u8 iv[12], const u8 *aad, u32 alen,
                const u8 *ct, u32 len, const u8 tag[16], u8 *pt)
{ return open(key, 16, iv, aad, alen, ct, len, tag, pt); }

void aes256gcm_seal(const u8 key[32], const u8 iv[12], const u8 *aad, u32 alen,
                    const u8 *pt, u32 len, u8 *ct, u8 tag[16])
{ seal(key, 32, iv, aad, alen, pt, len, ct, tag); }

int aes256gcm_open(const u8 key[32], const u8 iv[12], const u8 *aad, u32 alen,
                   const u8 *ct, u32 len, const u8 tag[16], u8 *pt)
{ return open(key, 32, iv, aad, alen, ct, len, tag, pt); }
