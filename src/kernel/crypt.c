/* ===========================================================================
 * crypt.c -- sealing and opening, for apps.
 *
 * An app cannot reach the ciphers itself (it lives behind the wall), so it
 * asks for these.  A passphrase is made into a 256-bit key with
 * PBKDF2-HMAC-SHA256 and a salt; data is sealed with one of three
 * authenticated ciphers, so that anything altered, or opened with the
 * wrong key, is refused whole rather than given back wrong.
 * ===========================================================================
 */
#include "flopos.h"

/* The ciphers, by the numbers apps use (CRYPT_* in app.h). */
enum { C_CHACHA = 0, C_AES256 = 1, C_AES128 = 2 };

void crypt_key(const char *pass, const u8 *salt, u32 slen, u32 iters, u8 key[32])
{
    u32 n = 0;
    while (pass[n]) n++;
    if (iters < 1) iters = 1;
    pbkdf2_sha256((const u8 *)pass, n, salt, slen, iters, key);
}

void crypt_seal(int alg, const u8 key[32], const u8 nonce[12], const u8 *aad, u32 alen,
                const u8 *in, u32 n, u8 *out, u8 tag[16])
{
    if (alg == C_AES256)      aes256gcm_seal(key, nonce, aad, alen, in, n, out, tag);
    else if (alg == C_AES128) aesgcm_seal(key, nonce, aad, alen, in, n, out, tag);
    else                      aead_seal(key, nonce, aad, alen, in, n, out, tag);
}

int crypt_open(int alg, const u8 key[32], const u8 nonce[12], const u8 *aad, u32 alen,
               const u8 *in, u32 n, const u8 tag[16], u8 *out)
{
    if (alg == C_AES256) return aes256gcm_open(key, nonce, aad, alen, in, n, tag, out);
    if (alg == C_AES128) return aesgcm_open(key, nonce, aad, alen, in, n, tag, out);
    if (alg != C_CHACHA) return 0;
    return aead_open(key, nonce, aad, alen, in, n, tag, out);
}

void crypt_random(u8 *out, u32 n) { rand_bytes(out, (int)n); }
