/* ===========================================================================
 * tls.c -- TLS 1.3, the client half (RFC 8446), and TLS 1.2 (RFC 5246)
 * for servers that have nothing newer.
 *
 * How a secure web page is fetched.  The two ends agree on a secret with
 * X25519 in the first two messages, derive a ladder of keys from it with
 * HKDF, and from the third message on everything is encrypted with
 * ChaCha20-Poly1305 and sealed so that tampering is caught.  Each side
 * finishes the handshake by proving it saw exactly the same handshake as
 * the other -- a hash of every message so far, signed with a key only the
 * two of them could have.
 *
 * Two suites, ChaCha20-Poly1305 first because it is much the faster on
 * this processor, and AES-128-GCM because every TLS 1.3 server has to
 * speak it and some speak nothing else.
 *
 * Two key exchanges, offered together.  X25519MLKEM768 first: X25519 and
 * ML-KEM-768 at once, the secret the two of them agree on, so a recording
 * of the conversation stays private even from a future quantum computer
 * that could break X25519 alone.  Plain X25519 as well, for servers that
 * do not do the post-quantum one yet -- they simply pick that instead, no
 * extra round trip.
 *
 * And the server has to prove who it is.  It sends its certificate chain,
 * which x509.c checks up to an authority in ROOTS.DAT, and then signs the
 * handshake so far with the key in its own certificate, which is checked
 * here.  Without both, the connection is refused and says why -- unless
 * the person asked to go on anyway (tls_check(0)), and then everything
 * after says it was not checked.
 *
 * TLS 1.2, only as safe as 1.3 is: the same two ciphers, sealed the same
 * way (RFC 7905, RFC 5288), and a fresh key exchange every time -- X25519,
 * or P-256 for servers too old for that -- signed by the server.  Nothing
 * from 1.2's older half: no RSA key exchange, no CBC, no SHA-1, no
 * renegotiation, no compression.  The extended master secret (RFC 7627) is
 * used where the server has it.  And a server that speaks 1.3 but answers
 * 1.2 is refused: TLS 1.3 servers mark the answer so, and it means someone
 * in between has tampered with our hello to force the older one.
 * ===========================================================================
 */
#include "flopos.h"
#include "io.h"

#define REC_MAX   (16384 + 256 + 16 + 5)
#define HS_MAX    32768

#define CT_CCS       20
#define CT_ALERT     21
#define CT_HANDSHAKE 22
#define CT_APPDATA   23

#define HS_SERVER_HELLO   2
#define HS_SKE            12             /* TLS 1.2: ServerKeyExchange      */
#define HS_CERT_REQ       13
#define HS_HELLO_DONE     14
#define HS_CKE            16
#define HS_CERT_STATUS    22
#define HS_NEW_TICKET     4
#define HS_ENC_EXT        8
#define HS_CERT           11
#define HS_CERT_VERIFY    15
#define HS_FINISHED       20

typedef struct { u8 key[32], iv[12], mac[20]; u32 seq; } Dir;

#define SUITE_CHACHA 0x1303
#define SUITE_AES128 0x1301
#define GROUP_X25519 0x001d
#define GROUP_P256   0x0017
#define GROUP_P384   0x0018
#define S12_CHACHA_EC  0xCCA9            /* TLS 1.2: ECDHE, then the cipher */
#define S12_CHACHA_RSA 0xCCA8
#define S12_AES_EC     0xC02B
#define S12_AES_RSA    0xC02F
/* The two older TLS 1.2 ciphers Chrome still offers, last, for servers with
 * nothing newer: ECDHE-RSA, AES in CBC mode, SHA-1 checks.  Nothing older
 * (no RSA key exchange, no ECDSA-CBC, no 3DES, no RC4). */
#define S12_CBC128     0xC013
#define S12_CBC256     0xC014
#define GROUP_HYBRID 0x11ec              /* X25519MLKEM768                  */

static u32 s_suite, s_group;
static int s_offer_pq = 1;               /* offer the hybrid at all        */
static int s_pq_broke;                   /* it was chosen, and then failed  */
static int s_v12, s_ems, s_p256;         /* TLS 1.2; its extended master secret; P-256 */
/* When a TLS 1.3 server wants a curve we sent no share for (P-256 or
 * P-384), it says so in a HelloRetryRequest and we say hello again with
 * that share alone, and its cookie if it gave one (RFC 8446, 4.1.4). */
static u32 s_retry_group;
static u8  s_cookie[256];
static int s_cookie_len;
static u8  s_ec_secret[48], s_ec_pub[97], s_ec_peer[97];
static const u8 s_hrr[32] = {
    0xCF, 0x21, 0xAD, 0x74, 0xE5, 0x9A, 0x61, 0x11, 0xBE, 0x1D, 0x8C, 0x02,
    0x1E, 0x65, 0xB8, 0x91, 0xC2, 0xA2, 0x11, 0x16, 0x7A, 0xBB, 0x8C, 0x5E,
    0x07, 0x9E, 0x09, 0xE2, 0xC8, 0xA8, 0x33, 0x9C };
static int ec_bytes(u32 g) { return g == GROUP_P384 ? 48 : 32; }
static u8  s_srv_random[32];

static int aes(void) { return s_suite == SUITE_AES128 || s_suite == S12_AES_EC || s_suite == S12_AES_RSA; }
static int cbc(void) { return s_suite == S12_CBC128 || s_suite == S12_CBC256; }
static int cbc_klen(void) { return s_suite == S12_CBC256 ? 32 : 16; }

void tls_offer_pq(int on) { s_offer_pq = on; }
int  tls_pq_broke(void)   { return s_pq_broke; }

static u8 *s_rec;                        /* one record as it arrived        */
static u8 *s_plain;                      /* one record, decrypted           */
static u8 *s_hsbuf;                      /* handshake messages, gathered    */
static int s_hsbuf_len;
static int s_plain_len, s_plain_pos;     /* application data not yet read   */
static Dir s_rd, s_wr;
static Sha256 s_transcript;
static int s_up, s_eof;
static char s_why[128];

static int  s_check = 1;                 /* make the server prove itself   */
static int  s_untrusted;                 /* the last failure was about that */
static int  s_checked;                   /* this connection: proved        */
static u8  *s_certmsg;                   /* its Certificate message, kept  */
static void (*s_note)(const char *);

void tls_check(int on)       { s_check = on; }
int  tls_untrusted(void)     { return s_untrusted; }
int  tls_checked(void)       { return s_checked; }
const char *tls_who(void)    { return s_checked ? x509_who() : ""; }
void tls_set_note(void (*fn)(const char *)) { s_note = fn; }

const char *tls_why(void) { return s_why; }
int tls_post_quantum(void) { return s_group == GROUP_HYBRID; }
const char *tls_kex(void)
{
    if (s_v12) return s_p256 == 2 ? (s_ems ? "P-384" : "P-384, no extended master secret")
                    : s_p256 ? (s_ems ? "P-256" : "P-256, no extended master secret")
                             : (s_ems ? "X25519" : "X25519, no extended master secret");
    return s_group == GROUP_HYBRID ? "X25519MLKEM768 (post-quantum)"
         : s_group == GROUP_P384 ? "P-384" : s_group == GROUP_P256 ? "P-256" : "X25519";
}
const char *tls_cipher(void)
{
    if (s_v12 && cbc()) return s_suite == S12_CBC256 ? "AES-256-CBC with SHA-1, TLS 1.2" : "AES-128-CBC with SHA-1, TLS 1.2";
    if (s_v12) return aes() ? "AES-128-GCM, TLS 1.2" : "ChaCha20-Poly1305, TLS 1.2";
    return aes() ? "AES-128-GCM" : "ChaCha20-Poly1305";
}

static void why(const char *t)
{
    int i;
    for (i = 0; t[i] && i < (int)sizeof s_why - 1; i++) s_why[i] = t[i];
    s_why[i] = 0;
}

static void put16(u8 *p, u32 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
static u32 get16(const u8 *p) { return ((u32)p[0] << 8) | p[1]; }
static u32 get24(const u8 *p) { return ((u32)p[0] << 16) | ((u32)p[1] << 8) | p[2]; }

static int same(const u8 *a, const u8 *b, int n)
{
    u8 d = 0;
    int i;
    for (i = 0; i < n; i++) d |= (u8)(a[i] ^ b[i]);
    return d == 0;
}

/* ---- the key schedule ---------------------------------------------------
 * Every key is HKDF-Expand of some secret with a label saying what the
 * key is for, and every label begins "tls13 ". */
static void expand_label(const u8 secret[32], const char *label,
                         const u8 *ctx, int clen, u8 *out, int olen)
{
    u8 info[2 + 1 + 6 + 32 + 1 + 32];
    int n = 0, i, ll = 0;

    while (label[ll]) ll++;
    put16(info, (u32)olen); n = 2;
    info[n++] = (u8)(6 + ll);
    for (i = 0; i < 6; i++) info[n++] = (u8)"tls13 "[i];
    for (i = 0; i < ll; i++) info[n++] = (u8)label[i];
    info[n++] = (u8)clen;
    for (i = 0; i < clen; i++) info[n++] = ctx[i];
    hkdf_expand(secret, info, (u32)n, out, (u32)olen);
}

static void transcript_now(u8 out[32])
{
    Sha256 copy = s_transcript;          /* the running hash, not disturbed */
    sha256_final(&copy, out);
}

static void derive(const u8 secret[32], const char *label, u8 out[32])
{
    u8 h[32];
    transcript_now(h);
    expand_label(secret, label, h, 32, out, 32);
}

static void set_keys(Dir *d, const u8 secret[32])
{
    expand_label(secret, "key", 0, 0, d->key, s_suite == SUITE_AES128 ? 16 : 32);
    expand_label(secret, "iv", 0, 0, d->iv, 12);
    d->seq = 0;
}

static void nonce_for(const Dir *d, u8 nonce[12])
{
    int i;
    for (i = 0; i < 12; i++) nonce[i] = d->iv[i];
    for (i = 0; i < 4; i++) nonce[11 - i] ^= (u8)(d->seq >> (8 * i));
}

/* ---- records ------------------------------------------------------------ */
/* Fifteen seconds of silence is a server that is not going to answer.
 * Esc gives up sooner: a handshake waiting on a server that never replies
 * should never be something to sit and watch. */
static int s_cancelled;

static int read_exact(u8 *buf, int n)
{
    int got = 0, r;
    u32 until = ms_now() + 15000;
    while (got < n) {
        r = tcp_read(buf + got, n - got, 200);
        if (r == 0) return 0;
        if (r == -2) return -1;
        if (r > 0) { got += r; until = ms_now() + 15000; continue; }
        if (ui_key() == UK_ESC) { s_cancelled = 1; return -1; }
        if ((s32)(ms_now() - until) >= 0) return -1;
    }
    return n;
}

/* One record off the wire: its type and its body, in s_rec + 5. */
static int read_record(u8 *type, int *len)
{
    int r = read_exact(s_rec, 5);
    if (r <= 0) {
        why(r == 0 ? "the server hung up" : s_cancelled ? "stopped" : "the server never answered");
        return 0;
    }
    *type = s_rec[0];
    *len = (int)get16(s_rec + 3);
    if (*len > REC_MAX - 5) { why("the server sent a record too big to be real"); return 0; }
    if (read_exact(s_rec + 5, *len) <= 0) { why("the server hung up mid-record"); return 0; }
    return 1;
}

/* Decrypt the record in s_rec into s_plain; returns the real content type
 * that was hidden inside it, or 0 if the seal did not hold. */
static void aad12(u8 aad[13], u32 seq, u8 type, int n)
{
    int i;
    for (i = 0; i < 4; i++) aad[i] = 0;
    for (i = 0; i < 4; i++) aad[4 + i] = (u8)(seq >> (24 - 8 * i));
    aad[8] = type; aad[9] = 3; aad[10] = 3;
    put16(aad + 11, (u32)n);
}

/* TLS 1.2: the record's own type is the real one, nothing hidden inside.
 * AES-GCM carries eight bytes of its nonce in front; ChaCha20 makes it
 * from the sequence number, as 1.3 does. */
/* A CBC record's check, HMAC-SHA-1 over the sequence number, the record's
 * header and its text. */
static void mac12(const u8 key[20], const u8 hdr[13], const u8 *data, int len, u8 out[20])
{
    u8 k[64];
    Sha1 c;
    int i;
    for (i = 0; i < 64; i++) k[i] = (u8)((i < 20 ? key[i] : 0) ^ 0x36);
    sha1_init(&c); sha1_update(&c, k, 64); sha1_update(&c, hdr, 13); sha1_update(&c, data, (u32)len); sha1_final(&c, out);
    for (i = 0; i < 64; i++) k[i] = (u8)((i < 20 ? key[i] : 0) ^ 0x5c);
    sha1_init(&c); sha1_update(&c, k, 64); sha1_update(&c, out, 20); sha1_final(&c, out);
}

/* SHA-1 compressions for the inner hash of a check over len bytes. */
static int mac_blocks(int len) { return (64 + 13 + len + 9 + 63) / 64; }

/* A CBC record: a fresh IV, then the text, its check and padding, sealed.
 * The padding and the check are looked at the same way whatever they hold
 * -- every padding byte the record could have, and as many SHA-1 blocks as
 * the longest text it could hold -- so how long the answer takes says
 * nothing about which was wrong (the "Lucky 13" attack on CBC). */
static int open_cbc(int len, int *plen)
{
    int n = len - 16, i, pad, bad, L;
    u8 want[20], aad[13], diff = 0;
    if (len < 16 + 32 || (n & 15)) return 0;
    aes_cbc_decrypt(s_rd.key, cbc_klen(), s_rec + 5, s_rec + 5 + 16, s_plain, (u32)n);
    pad = s_plain[n - 1];
    bad = pad + 1 + 20 > n;
    if (bad) pad = 0;
    for (i = 0; i < 256 && i < n; i++) {
        u8 in_pad = (u8)(0u - (u32)(i <= pad));
        diff |= (u8)(in_pad & (s_plain[n - 1 - i] ^ (u8)pad));
    }
    L = n - 1 - pad - 20;
    aad12(aad, s_rd.seq, s_rec[0], L);
    mac12(s_rd.mac, aad, s_plain, L, want);
    sha1_waste(mac_blocks(n - 21) - mac_blocks(L));
    for (i = 0; i < 20; i++) diff |= (u8)(want[i] ^ s_plain[L + i]);
    if (diff || bad) return 0;
    s_rd.seq++;
    *plen = L;
    return s_rec[0] ? s_rec[0] : -1;
}

static int open12(int len, int *plen)
{
    u8 nonce[12], aad[13];
    const u8 *ct = s_rec + 5;
    int n = len - 16, i;
    if (cbc()) return open_cbc(len, plen);
    if (aes()) {
        if (len < 8 + 16) return 0;
        for (i = 0; i < 4; i++) nonce[i] = s_rd.iv[i];
        for (i = 0; i < 8; i++) nonce[4 + i] = ct[i];
        ct += 8; n -= 8;
    } else {
        if (n < 0) return 0;
        nonce_for(&s_rd, nonce);
    }
    aad12(aad, s_rd.seq, s_rec[0], n);
    if (aes() ? !aesgcm_open(s_rd.key, nonce, aad, 13, ct, (u32)n, ct + n, s_plain)
              : !aead_open(s_rd.key, nonce, aad, 13, ct, (u32)n, ct + n, s_plain))
        return 0;
    s_rd.seq++;
    *plen = n;
    return s_rec[0] ? s_rec[0] : -1;
}

static int open_record(int len, int *plen)
{
    u8 nonce[12];
    int n = len - 16;

    if (s_v12) return open12(len, plen);

    if (n < 1) return 0;
    nonce_for(&s_rd, nonce);
    if (s_suite == SUITE_AES128
        ? !aesgcm_open(s_rd.key, nonce, s_rec, 5, s_rec + 5, (u32)n, s_rec + 5 + n, s_plain)
        : !aead_open(s_rd.key, nonce, s_rec, 5, s_rec + 5, (u32)n, s_rec + 5 + n, s_plain))
        return 0;
    s_rd.seq++;
    while (n > 0 && s_plain[n - 1] == 0) n--;   /* padding                 */
    if (n < 1) return 0;
    *plen = n - 1;
    return s_plain[n - 1];
}

static int send_record(u8 type, const u8 *data, int len)
{
    static u8 *out;                      /* from the heap: low memory is short */
    u8 nonce[12];
    int i;

    if (len > 16384) return 0;
    if (!out) out = (u8 *)halloc(16384 + 128);
    if (!out) return 0;
    if (s_v12 && cbc()) {
        u8 aad[13];
        int body = len + 20, pad = 15 - (body % 16), total, k;
        out[0] = type; out[1] = 3; out[2] = 3;
        rand_bytes(out + 5, 16);                       /* the IV, fresh each time */
        for (i = 0; i < len; i++) out[21 + i] = data[i];
        aad12(aad, s_wr.seq, type, len);
        mac12(s_wr.mac, aad, out + 21, len, out + 21 + len);
        for (k = 0; k <= pad; k++) out[21 + body + k] = (u8)pad;
        total = body + pad + 1;
        aes_cbc_encrypt(s_wr.key, cbc_klen(), out + 5, out + 21, out + 21, (u32)total);
        put16(out + 3, (u32)(16 + total));
        s_wr.seq++;
        return tcp_write(out, 5 + 16 + total) == 5 + 16 + total;
    }
    if (s_v12) {
        u8 aad[13];
        int pre = aes() ? 8 : 0;
        out[0] = type; out[1] = 3; out[2] = 3;
        put16(out + 3, (u32)(pre + len + 16));
        if (aes()) {
            for (i = 0; i < 4; i++) nonce[i] = s_wr.iv[i];
            for (i = 0; i < 4; i++) nonce[4 + i] = 0;
            for (i = 0; i < 4; i++) nonce[8 + i] = (u8)(s_wr.seq >> (24 - 8 * i));
            for (i = 0; i < 8; i++) out[5 + i] = nonce[4 + i];   /* in the clear */
        } else
            nonce_for(&s_wr, nonce);
        for (i = 0; i < len; i++) out[5 + pre + i] = data[i];
        aad12(aad, s_wr.seq, type, len);
        if (aes()) aesgcm_seal(s_wr.key, nonce, aad, 13, out + 5 + pre, (u32)len, out + 5 + pre, out + 5 + pre + len);
        else       aead_seal(s_wr.key, nonce, aad, 13, out + 5 + pre, (u32)len, out + 5 + pre, out + 5 + pre + len);
        s_wr.seq++;
        return tcp_write(out, 5 + pre + len + 16) == 5 + pre + len + 16;
    }
    out[0] = CT_APPDATA; out[1] = 3; out[2] = 3;
    put16(out + 3, (u32)(len + 1 + 16));
    for (i = 0; i < len; i++) out[5 + i] = data[i];
    out[5 + len] = type;                 /* the real type, hidden inside   */
    nonce_for(&s_wr, nonce);
    if (s_suite == SUITE_AES128)
        aesgcm_seal(s_wr.key, nonce, out, 5, out + 5, (u32)(len + 1), out + 5, out + 5 + len + 1);
    else
        aead_seal(s_wr.key, nonce, out, 5, out + 5, (u32)(len + 1), out + 5, out + 5 + len + 1);
    s_wr.seq++;
    return tcp_write(out, 5 + len + 1 + 16) == 5 + len + 1 + 16;
}

static const char *alert_name(int d)
{
    switch (d) {
    case 0:   return "goodbye";
    case 40:  return "it found nothing we both speak";
    case 42: case 43: case 44: case 45: case 46: return "it did not like a certificate";
    case 47:  return "it did not like something we sent";
    case 50:  return "it could not read what we sent";
    case 51:  return "it could not decrypt what we sent";
    case 70:  return "it speaks only TLS 1.1 or older, which is not safe enough";
    case 71:  return "it wants stronger encryption than we offered";
    case 80:  return "it had an internal problem";
    case 109: return "it needs an extension we did not send";
    case 112: return "it does not know that name";
    default:  return "it refused";
    }
}

/* ---- the handshake ------------------------------------------------------ */
static int client_hello(const char *host, const u8 random[32], const u8 session[32],
                        const u8 pub[32], const u8 *mlkem_ek)
{
    static u8 m[2048];
    int n, hl = 0, i, ext_at, sni_len;

    while (host[hl]) hl++;
    n = 5 + 4;                           /* record header, message header  */
    m[n++] = 3; m[n++] = 3;              /* "TLS 1.2", as 1.3 insists       */
    for (i = 0; i < 32; i++) m[n++] = random[i];
    m[n++] = 32;                         /* a session id, for middleboxes  */
    for (i = 0; i < 32; i++) m[n++] = session[i];
    put16(m + n, 16); n += 2;
    put16(m + n, SUITE_CHACHA); n += 2;  /* the fast one here, first       */
    put16(m + n, SUITE_AES128); n += 2;  /* the one every server has       */
    put16(m + n, S12_CHACHA_EC); n += 2; /* and the same two in TLS 1.2    */
    put16(m + n, S12_CHACHA_RSA); n += 2;
    put16(m + n, S12_AES_EC); n += 2;
    put16(m + n, S12_AES_RSA); n += 2;
    put16(m + n, S12_CBC128); n += 2;    /* and, last, Chrome's two older ones */
    put16(m + n, S12_CBC256); n += 2;
    m[n++] = 1; m[n++] = 0;              /* no compression                 */

    ext_at = n; n += 2;

    /* server_name: which site, since one address can serve many.  Not
     * for a bare address, which the standard says must not be sent. */
    sni_len = hl;
    {
        u32 dummy;
        if (ip_parse(host, &dummy)) sni_len = 0;
    }
    if (sni_len) {
        put16(m + n, 0x0000); n += 2;
        put16(m + n, (u32)(sni_len + 5)); n += 2;
        put16(m + n, (u32)(sni_len + 3)); n += 2;
        m[n++] = 0;
        put16(m + n, (u32)sni_len); n += 2;
        for (i = 0; i < sni_len; i++) m[n++] = (u8)host[i];
    }

    /* supported_groups: the post-quantum hybrid, then plain x25519, and
     * P-256 and P-384 for servers that have neither -- the same four as
     * Chrome and Firefox. */
    put16(m + n, 0x000a); n += 2; put16(m + n, s_offer_pq ? 10 : 8); n += 2;
    put16(m + n, s_offer_pq ? 8 : 6); n += 2;
    if (s_offer_pq) { put16(m + n, GROUP_HYBRID); n += 2; }
    put16(m + n, GROUP_X25519); n += 2;
    put16(m + n, GROUP_P256); n += 2;
    put16(m + n, GROUP_P384); n += 2;

    /* cookie: what a server that asked us to say hello again gave us. */
    if (s_cookie_len) {
        put16(m + n, 0x002c); n += 2; put16(m + n, (u32)(s_cookie_len + 2)); n += 2;
        put16(m + n, (u32)s_cookie_len); n += 2;
        for (i = 0; i < s_cookie_len; i++) m[n++] = s_cookie[i];
    }

    /* For TLS 1.2: points sent whole; the extended master secret; and an
     * empty renegotiation_info, which says we will never renegotiate. */
    put16(m + n, 0x000b); n += 2; put16(m + n, 2); n += 2; m[n++] = 1; m[n++] = 0;
    put16(m + n, 0x0017); n += 2; put16(m + n, 0); n += 2;
    put16(m + n, 0xff01); n += 2; put16(m + n, 1); n += 2; m[n++] = 0;

    /* signature_algorithms: what the server may sign with, which is what
     * this machine can check -- ECDSA on P-256 and P-384, RSA-PSS for the
     * handshake, and RSA PKCS#1 for the certificates above it. */
    {
        static const u16 sa[] = { 0x0403, 0x0804, 0x0503, 0x0805, 0x0806,
                                  0x0401, 0x0501, 0x0601 };
        int k, cnt = (int)(sizeof sa / sizeof sa[0]);
        put16(m + n, 0x000d); n += 2;
        put16(m + n, (u32)(2 + 2 * cnt)); n += 2;
        put16(m + n, (u32)(2 * cnt)); n += 2;
        for (k = 0; k < cnt; k++) { put16(m + n, sa[k]); n += 2; }
    }

    /* supported_versions: 1.3, then 1.2. */
    put16(m + n, 0x002b); n += 2; put16(m + n, 5); n += 2;
    m[n++] = 4; m[n++] = 3; m[n++] = 4; m[n++] = 3; m[n++] = 3;

    /* key_share: our half of the secret, both ways.  The hybrid share is
     * the ML-KEM encapsulation key followed by the X25519 public key. */
    if (s_retry_group) {
        int pl = 1 + 2 * ec_bytes(s_retry_group);
        put16(m + n, 0x0033); n += 2; put16(m + n, (u32)(4 + pl + 2)); n += 2;
        put16(m + n, (u32)(4 + pl)); n += 2;
        put16(m + n, s_retry_group); n += 2; put16(m + n, (u32)pl); n += 2;
        for (i = 0; i < pl; i++) m[n++] = s_ec_pub[i];
    } else {
        int share = (s_offer_pq ? 4 + MLKEM_EK + 32 : 0) + 4 + 32;
        put16(m + n, 0x0033); n += 2; put16(m + n, (u32)(share + 2)); n += 2;
        put16(m + n, (u32)share); n += 2;
        if (s_offer_pq) {
            put16(m + n, GROUP_HYBRID); n += 2; put16(m + n, MLKEM_EK + 32); n += 2;
            for (i = 0; i < MLKEM_EK; i++) m[n++] = mlkem_ek[i];
            for (i = 0; i < 32; i++) m[n++] = pub[i];
        }
        put16(m + n, GROUP_X25519); n += 2; put16(m + n, 32); n += 2;
        for (i = 0; i < 32; i++) m[n++] = pub[i];
    }

    put16(m + ext_at, (u32)(n - ext_at - 2));

    m[5] = 1;                            /* ClientHello                    */
    m[6] = (u8)((n - 9) >> 16); m[7] = (u8)((n - 9) >> 8); m[8] = (u8)(n - 9);
    m[0] = CT_HANDSHAKE; m[1] = 3; m[2] = 1;
    put16(m + 3, (u32)(n - 5));

    sha256_update(&s_transcript, m + 5, (u32)(n - 5));
    return tcp_write(m, n) == n;
}

/* The server's half of the secret, out of its ServerHello. */
static u8 s_mlkem_ct[MLKEM_CT], s_mlkem_ek[MLKEM_EK], s_mlkem_dk[MLKEM_DK];

static int server_hello(const u8 *b, int len, u8 server_pub[32])
{
    static const u8 hrr[32] = {
        0xCF, 0x21, 0xAD, 0x74, 0xE5, 0x9A, 0x61, 0x11, 0xBE, 0x1D, 0x8C, 0x02,
        0x1E, 0x65, 0xB8, 0x91, 0xC2, 0xA2, 0x11, 0x16, 0x7A, 0xBB, 0x8C, 0x5E,
        0x07, 0x9E, 0x09, 0xE2, 0xC8, 0xA8, 0x33, 0x9C };
    int p, end, sid, got_ver = 0, got_key = 0;

    if (len < 38) { why("its hello was too short"); return 0; }
    if (same(b + 2, hrr, 32)) { why("it asked twice for another key exchange"); return 0; }
    for (p = 0; p < 32; p++) s_srv_random[p] = b[2 + p];
    p = 34;
    sid = b[p++];
    p += sid;
    if (p + 3 > len) { why("its hello did not add up"); return 0; }
    s_suite = get16(b + p);
    p += 3;
    if (p + 2 > len) {                   /* no extensions: TLS 1.2 at best  */
        end = p;
    } else {
        end = p + 2 + (int)get16(b + p);
        p += 2;
    }
    if (end > len) end = len;
    while (p + 4 <= end) {
        u32 type = get16(b + p), elen = get16(b + p + 2);
        const u8 *e = b + p + 4;
        if (p + 4 + (int)elen > end) break;
        if (type == 0x002b && elen == 2 && get16(e) == 0x0304) got_ver = 1;
        if (type == 0x0017 && elen == 0) s_ems = 1;
        if (type == 0x0033 && elen == 36 && get16(e) == GROUP_X25519 && get16(e + 2) == 32) {
            int i;
            for (i = 0; i < 32; i++) server_pub[i] = e[4 + i];
            s_group = GROUP_X25519;
            got_key = 1;
        }
        /* After a retry: the curve point it asked for. */
        if (type == 0x0033 && s_retry_group && get16(e) == s_retry_group &&
            elen == 4 + 1 + 2 * (u32)ec_bytes(s_retry_group) && get16(e + 2) == elen - 4) {
            u32 i;
            for (i = 0; i < elen - 4; i++) s_ec_peer[i] = e[4 + i];
            s_group = s_retry_group;
            got_key = 1;
        }
        /* The hybrid: the ML-KEM ciphertext, then the X25519 public key. */
        if (type == 0x0033 && elen == 4 + MLKEM_CT + 32 && get16(e) == GROUP_HYBRID &&
            get16(e + 2) == MLKEM_CT + 32) {
            int i;
            for (i = 0; i < MLKEM_CT; i++) s_mlkem_ct[i] = e[4 + i];
            for (i = 0; i < 32; i++) server_pub[i] = e[4 + MLKEM_CT + i];
            s_group = GROUP_HYBRID;
            got_key = 1;
        }
        p += 4 + (int)elen;
    }
    if (!got_ver) {
        /* TLS 1.2, then -- unless a 1.3 server says it was pushed into it. */
        static const u8 down[7] = { 'D', 'O', 'W', 'N', 'G', 'R', 'D' };
        if (b[0] != 3 || b[1] != 3) { why("it speaks only TLS 1.1 or older, which is not safe enough"); return 0; }
        if (same(s_srv_random + 24, down, 7)) {
            why("someone in the way tried to make it use an older TLS");
            serial_printf("[tls] a downgrade to TLS 1.2 was refused\n");
            return 0;
        }
        if (s_suite != S12_CHACHA_EC && s_suite != S12_CHACHA_RSA && s_suite != S12_AES_EC && s_suite != S12_AES_RSA &&
            !cbc()) {
            why("it chose a cipher we did not offer");
            return 0;
        }
        s_v12 = 1;
        return 1;
    }
    if (s_suite != SUITE_CHACHA && s_suite != SUITE_AES128) {
        why("it chose a cipher we did not offer");
        return 0;
    }
    if (!got_key) { why("it sent no key share"); return 0; }
    return 1;
}

/* Take one whole handshake message off the front of s_hsbuf, if there is
 * one there yet: its type, its body, its length.  The message is added to
 * the transcript by the caller, once it knows whether it needs the hash
 * from just before it. */
static int next_message(int *type, const u8 **body, int *len)
{
    int ml;
    if (s_hsbuf_len < 4) return 0;
    ml = (int)get24(s_hsbuf + 1);
    if (s_hsbuf_len < ml + 4) return 0;
    *type = s_hsbuf[0];
    *body = s_hsbuf + 4;
    *len = ml;
    return 1;
}

static void drop_message(int len)
{
    int i, n = len + 4;
    sha256_update(&s_transcript, s_hsbuf, (u32)n);
    for (i = 0; i + n < s_hsbuf_len; i++) s_hsbuf[i] = s_hsbuf[i + n];
    s_hsbuf_len -= n;
}

/* The server's certificates: copied out of the handshake buffer (which
 * moves on), split into the chain, and checked for host. */
static int check_certs(const char *host, const u8 *body, int len)
{
    const u8 *certs[8];
    int lens[8], n = 0, at, end;
    char reason[120];

    if (!s_certmsg) s_certmsg = (u8 *)halloc(HS_MAX);
    if (!s_certmsg || len > HS_MAX) { why("its certificates were too big for us"); return 0; }
    memcpy8(s_certmsg, body, (usize)len);
    body = s_certmsg;

    if (len < 4) goto bad;
    at = s_v12 ? 0 : 1 + body[0];                /* 1.3: request context */
    if (at + 3 > len) goto bad;
    end = at + 3 + (int)get24(body + at);
    at += 3;
    if (end > len) goto bad;
    while (at < end && n < 8) {
        int cl;
        if (at + 3 > end) goto bad;
        cl = (int)get24(body + at);
        at += 3;
        if (at + cl + (s_v12 ? 0 : 2) > end) goto bad;
        certs[n] = body + at;
        lens[n++] = cl;
        at += cl;
        if (!s_v12) at += 2 + (int)get16(body + at);   /* 1.3: its extensions */
    }
    if (s_note) s_note("checking who it is...");
    if (!x509_check(certs, lens, n, host, reason, (int)sizeof reason)) {
        why(reason);
        serial_printf("[tls] %s could not prove who it is: %s\n", host, reason);
        return 0;
    }
    return 1;
bad:
    why("its certificate message was malformed");
    return 0;
}

/* Its signature over the handshake so far, with its certificate's key:
 * the proof that the certificate is its own and not one it copied. */
static int check_verify(const u8 *body, int len)
{
    static const char ctx[] = "TLS 1.3, server CertificateVerify";
    u8 content[64 + 33 + 1 + 32];
    int i, scheme, sl;

    if (len < 4) return 0;
    scheme = (int)get16(body);
    sl = (int)get16(body + 2);
    if (4 + sl > len) return 0;
    for (i = 0; i < 64; i++) content[i] = 0x20;
    for (i = 0; i < 33; i++) content[64 + i] = (u8)ctx[i];
    content[97] = 0;
    transcript_now(content + 98);
    return x509_check_handshake(scheme, content, (int)sizeof content, body + 4, sl);
}

/* One more encrypted handshake record into s_hsbuf. */
static int more_handshake(void)
{
    u8 type;
    int len, plen, ct, i;

    for (;;) {
        if (!read_record(&type, &len)) return 0;
        if (type == CT_CCS) continue;
        if (type == CT_ALERT && len >= 2) { why(alert_name(s_rec[6])); return 0; }
        if (type != CT_APPDATA) { why("it sent something unencrypted mid-handshake"); return 0; }
        break;
    }
    ct = open_record(len, &plen);
    if (!ct) {
        why("a handshake record failed its seal");
        if (s_group == GROUP_HYBRID) s_pq_broke = 1;
        return 0;
    }
    if (ct == CT_ALERT) { why(alert_name(plen >= 2 ? s_plain[1] : 0)); return 0; }
    if (ct != CT_HANDSHAKE) { why("it sent data before finishing its hello"); return 0; }
    if (s_hsbuf_len + plen > HS_MAX) { why("its certificates were too big for us"); return 0; }
    for (i = 0; i < plen; i++) s_hsbuf[s_hsbuf_len + i] = s_plain[i];
    s_hsbuf_len += plen;
    return 1;
}

/* ---- TLS 1.2 -----------------------------------------------------------
 * Its pseudo-random function: HMAC-SHA-256, chained (RFC 5246, 5). */
static void prf(const u8 *secret, int slen, const char *label, const u8 *seed, int seedlen, u8 *out, int olen)
{
    u8 ls[16 + 64], a[32], buf[32 + 16 + 64], h[32];
    int ll = 0, n, i;
    while (label[ll]) ll++;
    for (i = 0; i < ll; i++) ls[i] = (u8)label[i];
    for (i = 0; i < seedlen; i++) ls[ll + i] = seed[i];
    n = ll + seedlen;
    hmac_sha256(secret, (u32)slen, ls, (u32)n, a);              /* A(1)         */
    while (olen > 0) {
        for (i = 0; i < 32; i++) buf[i] = a[i];
        for (i = 0; i < n; i++) buf[32 + i] = ls[i];
        hmac_sha256(secret, (u32)slen, buf, (u32)(32 + n), h);
        for (i = 0; i < 32 && i < olen; i++) *out++ = h[i];
        olen -= 32;
        hmac_sha256(secret, (u32)slen, a, 32, a);               /* A(i+1)       */
    }
}

/* Handshake messages in the clear, into s_hsbuf. */
static int more_plain(void)
{
    u8 type;
    int len, i;
    if (!read_record(&type, &len)) return 0;
    if (type == CT_ALERT && len >= 2) { why(alert_name(s_rec[6])); return 0; }
    if (type != CT_HANDSHAKE) { why("it sent something else in the middle of its hello"); return 0; }
    if (s_hsbuf_len + len > HS_MAX) { why("its certificates were too big for us"); return 0; }
    for (i = 0; i < len; i++) s_hsbuf[s_hsbuf_len + i] = s_rec[5 + i];
    s_hsbuf_len += len;
    return 1;
}

static int send_plain(const u8 *msg, int n)
{
    static u8 out[5 + 80];
    int i;
    out[0] = CT_HANDSHAKE; out[1] = 3; out[2] = 3;
    put16(out + 3, (u32)n);
    for (i = 0; i < n; i++) out[5 + i] = msg[i];
    sha256_update(&s_transcript, msg, (u32)n);
    return tcp_write(out, 5 + n) == 5 + n;
}

/* After the ServerHello: its certificates, its signed key exchange, our
 * key exchange, and both sides' Finished. */
static int tls12_rest(const char *host, const u8 cli_random[32], const u8 secret[32])
{
    u8 spoint[97], content[64 + 4 + 97], cke[4 + 1 + 97], pms[48], master[48], kb[2 * 20 + 2 * 32];
    int pmslen = 32;
    u8 seed[64], h[32], fin[4 + 12], want[12];
    int mt, ml, i, done = 0, have_cert = 0, ske = 0, asked_cert = 0, curve = 0, plen = 0, n;
    const u8 *body;

    while (!done) {
        if (!next_message(&mt, &body, &ml)) {
            if (!more_plain()) return 0;
            continue;
        }
        if (mt == HS_CERT) {
            if (s_check) {
                if (!check_certs(host, body, ml)) { s_untrusted = 1; return 0; }
                have_cert = 1;
            }
        } else if (mt == HS_SKE) {
            /* curve_type 3 (named), the curve, its point, then its
             * signature over both randoms and all of that. */
            int sl, scheme;
            if (ml < 4 || body[0] != 3) { why("it offered a key exchange we do not do"); return 0; }
            curve = (int)get16(body + 1);
            plen = body[3];
            if (!((curve == GROUP_X25519 && plen == 32) || (curve == GROUP_P256 && plen == 65) ||
                  (curve == GROUP_P384 && plen == 97)) || 4 + plen + 4 > ml) {
                why("it offered a key exchange we do not do");
                return 0;
            }
            for (i = 0; i < plen; i++) spoint[i] = body[4 + i];
            scheme = (int)get16(body + 4 + plen);
            sl = (int)get16(body + 6 + plen);
            if (8 + plen + sl > ml) { why("its key exchange did not add up"); return 0; }
            if (s_check) {
                for (i = 0; i < 32; i++) { content[i] = cli_random[i]; content[32 + i] = s_srv_random[i]; }
                for (i = 0; i < 4 + plen; i++) content[64 + i] = body[i];
                if (!have_cert || !x509_check_handshake12(scheme, content, 68 + plen, body + 8 + plen, sl)) {
                    why("it could not prove the certificate is its own");
                    s_untrusted = 1;
                    return 0;
                }
            }
            ske = 1;
        } else if (mt == HS_CERT_REQ) {
            asked_cert = 1;                  /* we have none: an empty one, then */
        } else if (mt == HS_HELLO_DONE) {
            done = 1;
        } else if (mt != HS_CERT_STATUS) {
            why("it sent a handshake message out of turn");
            return 0;
        }
        drop_message(ml);
    }
    if (!ske) { why("it offered no key exchange we do"); return 0; }
    if (s_check && !have_cert) { why("it never proved who it is"); s_untrusted = 1; return 0; }

    /* Our half of the key exchange, and the secret the two make. */
    if (asked_cert) {
        static const u8 none[7] = { HS_CERT, 0, 0, 3, 0, 0, 0 };
        if (!send_plain(none, 7)) { why("could not send our answer"); return 0; }
    }
    cke[0] = HS_CKE; cke[1] = 0; cke[2] = 0;
    if (curve == GROUP_X25519) {
        u8 z = 0;
        cke[3] = 33; cke[4] = 32;
        x25519_public(cke + 5, secret);
        x25519(pms, secret, spoint);
        for (i = 0; i < 32; i++) z |= pms[i];
        if (!z) { why("its key exchange was not a real one"); return 0; }
        n = 4 + 33;
    } else if (curve == GROUP_P384) {
        cke[3] = 98; cke[4] = 97;
        if (s_note) s_note("working out a P-384 secret...");
        s_p256 = 2;
        rand_bytes(s_ec_secret, 48);
        if (!ecdh(CURVE_P384, s_ec_secret, spoint, pms, cke + 5)) { why("its key exchange was not on the curve"); return 0; }
        pmslen = 48;
        n = 4 + 98;
    } else {
        cke[3] = 66; cke[4] = 65;
        if (s_note) s_note("working out a P-256 secret...");
        s_p256 = 1;
        if (!ecdh_p256(secret, spoint, pms, cke + 5)) { why("its key exchange was not on the curve"); return 0; }
        n = 4 + 66;
    }
    if (!send_plain(cke, n)) { why("could not send our key exchange"); return 0; }

    if (s_ems) {
        transcript_now(h);
        prf(pms, pmslen, "extended master secret", h, 32, master, 48);
    } else {
        for (i = 0; i < 32; i++) { seed[i] = cli_random[i]; seed[32 + i] = s_srv_random[i]; }
        prf(pms, pmslen, "master secret", seed, 64, master, 48);
    }
    for (i = 0; i < 32; i++) { seed[i] = s_srv_random[i]; seed[32 + i] = cli_random[i]; }
    if (cbc()) {                         /* check keys, then cipher keys; the IVs travel with each record */
        int kl = cbc_klen();
        prf(master, 48, "key expansion", seed, 64, kb, 2 * (20 + kl));
        for (i = 0; i < 20; i++) { s_wr.mac[i] = kb[i]; s_rd.mac[i] = kb[20 + i]; }
        for (i = 0; i < kl; i++) { s_wr.key[i] = kb[40 + i]; s_rd.key[i] = kb[40 + kl + i]; }
        s_wr.seq = s_rd.seq = 0;
    } else {
        int kl = aes() ? 16 : 32, il = aes() ? 4 : 12;
        prf(master, 48, "key expansion", seed, 64, kb, 2 * (kl + il));
        for (i = 0; i < kl; i++) { s_wr.key[i] = kb[i]; s_rd.key[i] = kb[kl + i]; }
        for (i = 0; i < il; i++) { s_wr.iv[i] = kb[2 * kl + i]; s_rd.iv[i] = kb[2 * kl + il + i]; }
        s_wr.seq = s_rd.seq = 0;
    }

    /* ChangeCipherSpec, then our Finished, sealed. */
    {
        static const u8 ccs[6] = { CT_CCS, 3, 3, 0, 1, 1 };
        if (tcp_write(ccs, 6) != 6) { why("could not send our Finished"); return 0; }
    }
    transcript_now(h);
    fin[0] = HS_FINISHED; fin[1] = 0; fin[2] = 0; fin[3] = 12;
    prf(master, 48, "client finished", h, 32, fin + 4, 12);
    sha256_update(&s_transcript, fin, 16);
    if (!send_record(CT_HANDSHAKE, fin, 16)) { why("could not send our Finished"); return 0; }

    /* Theirs: ChangeCipherSpec, then a Finished that proves it saw the
     * same handshake. */
    transcript_now(h);
    prf(master, 48, "server finished", h, 32, want, 12);
    for (;;) {
        u8 type;
        int len;
        if (!read_record(&type, &len)) return 0;
        if (type == CT_ALERT && len >= 2) { why(alert_name(s_rec[6])); return 0; }
        if (type == CT_HANDSHAKE) continue;          /* a session ticket: not wanted */
        if (type == CT_CCS) break;
        why("it did not finish its hello");
        return 0;
    }
    {
        u8 type;
        int len, got, ct;
        if (!read_record(&type, &len)) return 0;
        if (type != CT_HANDSHAKE || !(ct = open_record(len, &got)) || got != 16 ||
            s_plain[0] != HS_FINISHED || !same(s_plain + 4, want, 12)) {
            why("its proof of the handshake did not match");
            return 0;
        }
    }
    s_up = 1;
    s_checked = s_check;
    return 1;
}

/* The server answered with a HelloRetryRequest: it wants a share on a
 * curve we listed but sent none for.  Its suite must be one we offered;
 * the transcript starts again from a hash of our first hello; then the
 * same hello again, with that share alone, and its cookie. */
static int hello_again(const char *host, const u8 random[32], const u8 session[32], const u8 pub[32], int ml)
{
    const u8 *b = s_rec + 9;
    int p, end, sid, i;
    u32 group = 0, ver = 0, suite;
    u8 h1[32], head[4];
    Sha256 c;

    if (s_retry_group) { why("it asked twice for another key exchange"); return 0; }
    p = 34;
    sid = b[p++];
    p += sid;
    if (p + 5 > ml) { why("its request for another hello did not add up"); return 0; }
    suite = get16(b + p);
    p += 3;
    end = p + 2 + (int)get16(b + p);
    p += 2;
    if (end > ml) { why("its request for another hello did not add up"); return 0; }
    while (p + 4 <= end) {
        u32 type = get16(b + p), elen = get16(b + p + 2);
        const u8 *e = b + p + 4;
        if (p + 4 + (int)elen > end) break;
        if (type == 0x002b && elen == 2) ver = get16(e);
        if (type == 0x0033 && elen == 2) group = get16(e);
        if (type == 0x002c && elen >= 2 && get16(e) == elen - 2) {
            if (elen - 2 > sizeof s_cookie) { why("its cookie was too big for us"); return 0; }
            for (i = 0; i < (int)elen - 2; i++) s_cookie[i] = e[2 + i];
            s_cookie_len = (int)elen - 2;
        }
        p += 4 + (int)elen;
    }
    if (ver != 0x0304 || (suite != SUITE_CHACHA && suite != SUITE_AES128)) { why("its request for another hello was not one we can answer"); return 0; }
    if (group != GROUP_P256 && group != GROUP_P384) { why("it wanted a key exchange we do not do"); return 0; }

    /* The transcript: message_hash(our first hello), then this request. */
    c = s_transcript;
    sha256_final(&c, h1);
    sha256_init(&s_transcript);
    head[0] = 254; head[1] = 0; head[2] = 0; head[3] = 32;
    sha256_update(&s_transcript, head, 4);
    sha256_update(&s_transcript, h1, 32);
    sha256_update(&s_transcript, s_rec + 5, (u32)(ml + 4));

    if (s_note) s_note(group == GROUP_P384 ? "working out a P-384 key..." : "working out a P-256 key...");
    s_retry_group = group;
    rand_bytes(s_ec_secret, 48);
    if (!ecdh(group == GROUP_P384 ? CURVE_P384 : CURVE_P256, s_ec_secret, 0, 0, s_ec_pub)) { why("could not make a key"); return 0; }
    if (!client_hello(host, random, session, pub, s_mlkem_ek)) { why("could not send our hello"); return 0; }
    serial_printf("[tls] it asked for %s: said hello again\n", group == GROUP_P384 ? "P-384" : "P-256");
    return 1;
}

int tls_connect(const char *host)
{
    u8 secret[32], pub[32], random[32], session[32], server_pub[32];
    u8 shared[64], early[32], derived[32], hs_secret[32], master[32];
    u8 cli_hs[32], srv_hs[32], cli_ap[32], srv_ap[32], zero[32], h_empty[32];
    u8 h[32], fkey[32], mac[32], fin[4 + 32];
    u8 type;
    int len, i, mt, ml, done = 0, have_cert = 0, proved = 0;
    const u8 *body;

    s_up = 0;
    s_untrusted = 0;
    s_checked = 0;
    s_eof = 0;
    s_hsbuf_len = 0;
    s_plain_len = s_plain_pos = 0;
    s_why[0] = 0;
    if (!s_rec) {
        s_rec = (u8 *)halloc(REC_MAX + 64);
        s_plain = (u8 *)halloc(REC_MAX + 64);
        s_hsbuf = (u8 *)halloc(HS_MAX);
    }

    rand_bytes(secret, 32);
    rand_bytes(random, 32);
    rand_bytes(session, 32);
    x25519_public(pub, secret);
    {
        u8 d[32], z[32];
        rand_bytes(d, 32);
        rand_bytes(z, 32);
        mlkem_keygen(d, z, s_mlkem_ek, s_mlkem_dk);
    }
    s_cancelled = 0;
    s_group = 0;
    s_pq_broke = 0;
    s_v12 = 0;
    s_ems = 0;
    s_p256 = 0;
    s_retry_group = 0;
    s_cookie_len = 0;

    sha256_init(&s_transcript);
    if (!client_hello(host, random, session, pub, s_mlkem_ek)) { why("could not send our hello"); return 0; }

    /* ServerHello arrives in the clear; a ChangeCipherSpec sent for the
     * benefit of old middleboxes is skipped. */
    for (;;) {
        if (!read_record(&type, &len)) return 0;
        if (type == CT_CCS) continue;
        if (type == CT_ALERT && len >= 2) { why(alert_name(s_rec[6])); return 0; }
        if (type != CT_HANDSHAKE || len < 4 || s_rec[5] != HS_SERVER_HELLO) {
            why("its first answer was not a hello");
            return 0;
        }
        break;
    }
    ml = (int)get24(s_rec + 6);
    if (ml + 4 > len) { why("its hello was cut short"); return 0; }
    if (ml >= 38 && same(s_rec + 11, s_hrr, 32)) {
        if (!hello_again(host, random, session, pub, ml)) return 0;
        for (;;) {
            if (!read_record(&type, &len)) return 0;
            if (type == CT_CCS) continue;
            if (type == CT_ALERT && len >= 2) { why(alert_name(s_rec[6])); return 0; }
            if (type != CT_HANDSHAKE || len < 4 || s_rec[5] != HS_SERVER_HELLO) {
                why("its second answer was not a hello");
                return 0;
            }
            break;
        }
        ml = (int)get24(s_rec + 6);
        if (ml + 4 > len) { why("its hello was cut short"); return 0; }
    }
    if (!server_hello(s_rec + 9, ml, server_pub)) return 0;
    if (s_retry_group && s_group != s_retry_group) { why("it did not use the key exchange it asked for"); return 0; }
    sha256_update(&s_transcript, s_rec + 5, (u32)(ml + 4));
    if (s_v12) {
        /* What else came in the same record -- its certificates, often --
         * is the start of the rest. */
        for (i = 0; i < len - 4 - ml; i++) s_hsbuf[i] = s_rec[9 + ml + i];
        s_hsbuf_len = len - 4 - ml;
        return tls12_rest(host, random, secret);
    }

    /* The shared secret -- for the hybrid, the ML-KEM secret and then the
     * X25519 one, sixty-four bytes -- and the ladder of keys from it. */
    if (s_group == GROUP_HYBRID) {
        mlkem_decaps(s_mlkem_dk, s_mlkem_ct, shared);
        x25519(shared + 32, secret, server_pub);
    } else if (s_group == GROUP_P256 || s_group == GROUP_P384) {
        if (!ecdh(s_group == GROUP_P384 ? CURVE_P384 : CURVE_P256, s_ec_secret, s_ec_peer, shared, 0)) {
            why("its key exchange was not on the curve");
            return 0;
        }
    } else {
        x25519(shared, secret, server_pub);
    }
    for (i = 0; i < 32; i++) zero[i] = 0;
    sha256(0, 0, h_empty);
    hkdf_extract(0, 0, zero, 32, early);
    expand_label(early, "derived", h_empty, 32, derived, 32);
    hkdf_extract(derived, 32, shared, s_group == GROUP_HYBRID ? 64 : s_group == GROUP_P384 ? 48 : 32, hs_secret);
    derive(hs_secret, "c hs traffic", cli_hs);
    derive(hs_secret, "s hs traffic", srv_hs);
    set_keys(&s_rd, srv_hs);
    set_keys(&s_wr, cli_hs);
    expand_label(hs_secret, "derived", h_empty, 32, derived, 32);
    hkdf_extract(derived, 32, zero, 32, master);

    /* The rest of the server's handshake: extensions, its certificate,
     * its signature over the handshake, and its Finished. */
    while (!done) {
        if (!next_message(&mt, &body, &ml)) {
            if (!more_handshake()) return 0;
            continue;
        }
        if (mt == HS_FINISHED) {
            if (s_check && (!have_cert || !proved)) {
                why("it never proved who it is");
                s_untrusted = 1;
                return 0;
            }
            /* Its proof: an HMAC, keyed from its handshake secret, of the
             * hash of every message before this one.  It matching means
             * nobody changed a byte of the handshake on the way. */
            transcript_now(h);
            expand_label(srv_hs, "finished", 0, 0, fkey, 32);
            hmac_sha256(fkey, 32, h, 32, mac);
            if (ml != 32 || !same(mac, body, 32)) {
                why("its proof of the handshake did not match");
                if (s_group == GROUP_HYBRID) s_pq_broke = 1;
                return 0;
            }
            drop_message(ml);
            done = 1;
        } else if (mt == HS_CERT) {
            if (s_check) {
                if (!check_certs(host, body, ml)) { s_untrusted = 1; return 0; }
                have_cert = 1;
            }
            drop_message(ml);
        } else if (mt == HS_CERT_VERIFY) {
            if (s_check) {
                if (!have_cert || !check_verify(body, ml)) {
                    why("it could not prove the certificate is its own");
                    s_untrusted = 1;
                    return 0;
                }
                proved = 1;
            }
            drop_message(ml);
        } else if (mt == HS_ENC_EXT) {
            drop_message(ml);
        } else {
            why("it sent a handshake message out of turn");
            return 0;
        }
    }

    /* The application keys come from the transcript up to their Finished. */
    derive(master, "c ap traffic", cli_ap);
    derive(master, "s ap traffic", srv_ap);

    /* Our Finished, under our handshake key. */
    transcript_now(h);
    expand_label(cli_hs, "finished", 0, 0, fkey, 32);
    fin[0] = HS_FINISHED; fin[1] = 0; fin[2] = 0; fin[3] = 32;
    hmac_sha256(fkey, 32, h, 32, fin + 4);
    {
        static const u8 ccs[6] = { CT_CCS, 3, 3, 0, 1, 1 };
        tcp_write(ccs, 6);               /* for middleboxes, as 1.3 allows  */
    }
    if (!send_record(CT_HANDSHAKE, fin, 36)) { why("could not send our Finished"); return 0; }

    set_keys(&s_rd, srv_ap);
    set_keys(&s_wr, cli_ap);
    s_up = 1;
    s_checked = s_check;
    return 1;
}

int tls_write(const u8 *data, int len)
{
    int done = 0;
    while (s_up && done < len) {
        int n = len - done;
        if (n > 4096) n = 4096;
        if (!send_record(CT_APPDATA, data + done, n)) { s_up = 0; break; }
        done += n;
    }
    return done;
}

/* Like tcp_read: bytes, 0 when the server has said goodbye, -1 on silence,
 * -2 when something broke. */
int tls_read(u8 *buf, int max, int wait_ms)
{
    u8 type;
    int len, plen, ct, i;

    (void)wait_ms;
    for (;;) {
        if (s_plain_pos < s_plain_len) {
            int n = s_plain_len - s_plain_pos;
            if (n > max) n = max;
            for (i = 0; i < n; i++) buf[i] = s_plain[s_plain_pos + i];
            s_plain_pos += n;
            return n;
        }
        if (s_eof || !s_up) return s_eof ? 0 : -2;

        if (!read_record(&type, &len)) {
            /* A server that just hangs up without saying goodbye is
             * common enough to treat as the end. */
            if (tcp_done()) { s_eof = 1; return 0; }
            return -1;
        }
        if (type == CT_CCS) continue;
        if (type != CT_APPDATA && !(s_v12 && (type == CT_ALERT || type == CT_HANDSHAKE))) {
            why("an unencrypted record after the handshake"); s_up = 0; return -2;
        }
        ct = open_record(len, &plen);
        if (!ct) { why("a record failed its seal"); s_up = 0; return -2; }
        if (ct == CT_ALERT) {
            if (plen >= 2 && s_plain[1] == 0) { s_eof = 1; return 0; }   /* close_notify */
            why(alert_name(plen >= 2 ? s_plain[1] : 0));
            s_up = 0;
            return -2;
        }
        if (ct == CT_HANDSHAKE) continue;     /* session tickets: not wanted */
        if (ct == CT_APPDATA) {
            /* s_plain holds it; plen bytes of data, then the type byte. */
            s_plain_len = plen;
            s_plain_pos = 0;
        }
    }
}

void tls_close(void)
{
    static const u8 bye[2] = { 1, 0 };   /* warning, close_notify          */
    if (s_up) send_record(CT_ALERT, bye, 2);
    s_up = 0;
}
