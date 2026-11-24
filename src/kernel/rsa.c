/* ===========================================================================
 * rsa.c -- checking an RSA signature (RFC 8017).
 *
 * The signer raised a padded hash to their secret power; anybody can raise
 * the signature to the public power e, modulo the public n, and must get
 * that padded hash back.  e is nearly always 65537, so this is seventeen
 * multiplications of numbers up to 4096 bits.
 *
 * Two ways of padding, and both turn up:
 *   PKCS#1 v1.5 -- 00 01 FF FF ... FF 00, then the hash labelled with its
 *     algorithm.  What certificates are almost always signed with.
 *   PSS -- the hash mixed with a random salt and masked.  What TLS 1.3
 *     requires when a server signs the handshake with an RSA key.
 * ===========================================================================
 */
#include "flopos.h"

/* "This is a SHA-xxx hash", as DER, ahead of the hash itself. */
static const u8 s_di256[] = { 0x30,0x31,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x01,0x05,0x00,0x04,0x20 };
static const u8 s_di384[] = { 0x30,0x41,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x02,0x05,0x00,0x04,0x30 };
static const u8 s_di512[] = { 0x30,0x51,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x03,0x05,0x00,0x04,0x40 };

static Mont s_m;
static u32  s_x[BN_LIMBS];
static u8   s_em[BN_LIMBS * 4];

/* sig^e mod n into s_em, k bytes long.  0 if anything is the wrong size. */
static int rsa_open(const u8 *n, int nlen, const u8 *e, int elen,
                    const u8 *sig, int slen, int *k)
{
    while (nlen > 0 && *n == 0) { n++; nlen--; }
    /* e: at most 32 bits, odd, at least 3 (as browsers insist).  A key
     * from a certificate not yet checked could otherwise carry an
     * exponent as long as the modulus, and the sum would take minutes. */
    while (elen > 0 && *e == 0) { e++; elen--; }
    if (elen < 1 || elen > 4 || !(e[elen - 1] & 1) || (elen == 1 && e[0] < 3)) return 0;
    while (slen > 0 && *sig == 0 && slen > nlen) { sig++; slen--; }
    if (nlen < 128 || slen > nlen) return 0;            /* under 1024 bits: no */
    if (!mont_init(&s_m, n, nlen)) return 0;
    if (!bn_from_be(s_x, s_m.n, sig, slen)) return 0;
    if (bn_cmp(s_x, s_m.m, s_m.n) >= 0) return 0;
    mont_pow(&s_m, s_x, s_x, e, elen);
    *k = nlen;
    bn_to_be(s_em, nlen, s_x, s_m.n);
    return 1;
}

int rsa_verify_pkcs1(const u8 *n, int nlen, const u8 *e, int elen,
                     const u8 *sig, int slen, int alg, const u8 *hash)
{
    const u8 *di = alg == HASH_SHA384 ? s_di384 : alg == HASH_SHA512 ? s_di512 : s_di256;
    int dlen = (int)sizeof s_di256, hlen = hash_len(alg), k, i, pad;

    if (!rsa_open(n, nlen, e, elen, sig, slen, &k)) return 0;
    pad = k - 3 - dlen - hlen;
    if (pad < 8) return 0;
    if (s_em[0] != 0x00 || s_em[1] != 0x01) return 0;
    for (i = 0; i < pad; i++) if (s_em[2 + i] != 0xFF) return 0;
    if (s_em[2 + pad] != 0x00) return 0;
    for (i = 0; i < dlen; i++) if (s_em[3 + pad + i] != di[i]) return 0;
    for (i = 0; i < hlen; i++) if (s_em[3 + pad + dlen + i] != hash[i]) return 0;
    return 1;
}

/* MGF1: a mask of any length, made by hashing the seed with a counter. */
static void mgf1_xor(int alg, const u8 *seed, int slen, u8 *out, int olen)
{
    u8 buf[64 + 4], h[64];
    int hlen = hash_len(alg), done = 0, i;
    u32 ctr = 0;

    for (i = 0; i < slen; i++) buf[i] = seed[i];
    while (done < olen) {
        buf[slen] = (u8)(ctr >> 24); buf[slen + 1] = (u8)(ctr >> 16);
        buf[slen + 2] = (u8)(ctr >> 8); buf[slen + 3] = (u8)ctr;
        hash_buf(alg, buf, (u32)(slen + 4), h);
        for (i = 0; i < hlen && done < olen; i++) out[done++] ^= h[i];
        ctr++;
    }
}

/* salt: the salt length the signer used; TLS insists on the hash's own. */
int rsa_verify_pss(const u8 *n, int nlen, const u8 *e, int elen,
                   const u8 *sig, int slen, int alg, const u8 *hash, int salt)
{
    static u8 mp[8 + 64 + 64];
    int hlen = hash_len(alg), k, embits, emlen, dblen, i, zeros;
    u8 *em, *db, *H, h2[64];

    if (salt < 0 || salt > 64) return 0;
    if (!rsa_open(n, nlen, e, elen, sig, slen, &k)) return 0;
    embits = s_m.bits - 1;
    emlen = (embits + 7) / 8;
    em = s_em + (k - emlen);                    /* a leading zero byte, if any */
    if (k > emlen && s_em[0] != 0) return 0;
    if (emlen < hlen + salt + 2) return 0;
    if (em[emlen - 1] != 0xBC) return 0;

    dblen = emlen - hlen - 1;
    db = em;
    H = em + dblen;
    zeros = 8 * emlen - embits;
    if (db[0] & (u8)(0xFF << (8 - zeros))) return 0;
    mgf1_xor(alg, H, hlen, db, dblen);
    db[0] &= (u8)(0xFF >> zeros);

    for (i = 0; i < dblen - salt - 1; i++) if (db[i] != 0) return 0;
    if (db[dblen - salt - 1] != 0x01) return 0;

    for (i = 0; i < 8; i++) mp[i] = 0;
    for (i = 0; i < hlen; i++) mp[8 + i] = hash[i];
    for (i = 0; i < salt; i++) mp[8 + hlen + i] = db[dblen - salt + i];
    hash_buf(alg, mp, (u32)(8 + hlen + salt), h2);
    for (i = 0; i < hlen; i++) if (h2[i] != H[i]) return 0;
    return 1;
}
