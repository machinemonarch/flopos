/* ===========================================================================
 * tls.c -- TLS 1.3, the client half (RFC 8446).
 *
 * How a secure web page is fetched.  The two ends agree on a secret with
 * X25519 in the first two messages, derive a ladder of keys from it with
 * HKDF, and from the third message on everything is encrypted with
 * ChaCha20-Poly1305 and sealed so that tampering is caught.  Each side
 * finishes the handshake by proving it saw exactly the same handshake as
 * the other -- a hash of every message so far, signed with a key only the
 * two of them could have.
 *
 * One suite, one group: TLS_CHACHA20_POLY1305_SHA256 and x25519.  Both
 * are what a machine without fast multiplication or AES instructions
 * should want, and almost every server on the internet offers both.
 *
 * WHAT THIS DOES NOT DO YET: check the server's certificate.  The
 * connection is encrypted and sealed, and nobody who is only listening
 * can read it -- but nothing yet establishes that the far end is the site
 * it says it is, so somebody able to intercept and answer in its place
 * could.  That needs signature checking (RSA and ECDSA) and a set of
 * trusted roots, and until it exists every fetch says so.
 * ===========================================================================
 */
#include "flopos.h"

#define REC_MAX   (16384 + 256 + 16 + 5)
#define HS_MAX    32768

#define CT_CCS       20
#define CT_ALERT     21
#define CT_HANDSHAKE 22
#define CT_APPDATA   23

#define HS_SERVER_HELLO   2
#define HS_NEW_TICKET     4
#define HS_ENC_EXT        8
#define HS_CERT           11
#define HS_CERT_VERIFY    15
#define HS_FINISHED       20

typedef struct { u8 key[32], iv[12]; u32 seq; } Dir;

static u8 *s_rec;                        /* one record as it arrived        */
static u8 *s_plain;                      /* one record, decrypted           */
static u8 *s_hsbuf;                      /* handshake messages, gathered    */
static int s_hsbuf_len;
static int s_plain_len, s_plain_pos;     /* application data not yet read   */
static Dir s_rd, s_wr;
static Sha256 s_transcript;
static int s_up, s_eof;
static char s_why[64];

const char *tls_why(void) { return s_why; }

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
    expand_label(secret, "key", 0, 0, d->key, 32);
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
static int read_exact(u8 *buf, int n)
{
    int got = 0, r;
    while (got < n) {
        r = tcp_read(buf + got, n - got, 15000);
        if (r == 0) return 0;
        if (r < 0) return -1;
        got += r;
    }
    return n;
}

/* One record off the wire: its type and its body, in s_rec + 5. */
static int read_record(u8 *type, int *len)
{
    int r = read_exact(s_rec, 5);
    if (r <= 0) { why(r == 0 ? "the server hung up" : "the server went quiet"); return 0; }
    *type = s_rec[0];
    *len = (int)get16(s_rec + 3);
    if (*len > REC_MAX - 5) { why("the server sent a record too big to be real"); return 0; }
    if (read_exact(s_rec + 5, *len) <= 0) { why("the server hung up mid-record"); return 0; }
    return 1;
}

/* Decrypt the record in s_rec into s_plain; returns the real content type
 * that was hidden inside it, or 0 if the seal did not hold. */
static int open_record(int len, int *plen)
{
    u8 nonce[12];
    int n = len - 16;

    if (n < 1) return 0;
    nonce_for(&s_rd, nonce);
    if (!aead_open(s_rd.key, nonce, s_rec, 5, s_rec + 5, (u32)n, s_rec + 5 + n, s_plain))
        return 0;
    s_rd.seq++;
    while (n > 0 && s_plain[n - 1] == 0) n--;   /* padding                 */
    if (n < 1) return 0;
    *plen = n - 1;
    return s_plain[n - 1];
}

static int send_record(u8 type, const u8 *data, int len)
{
    static u8 out[16384 + 64];
    u8 nonce[12];
    int i;

    if (len > 16384) return 0;
    out[0] = CT_APPDATA; out[1] = 3; out[2] = 3;
    put16(out + 3, (u32)(len + 1 + 16));
    for (i = 0; i < len; i++) out[5 + i] = data[i];
    out[5 + len] = type;                 /* the real type, hidden inside   */
    nonce_for(&s_wr, nonce);
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
    case 70:  return "it does not speak TLS 1.3";
    case 71:  return "it wants stronger encryption than we offered";
    case 80:  return "it had an internal problem";
    case 109: return "it needs an extension we did not send";
    case 112: return "it does not know that name";
    default:  return "it refused";
    }
}

/* ---- the handshake ------------------------------------------------------ */
static int client_hello(const char *host, const u8 random[32], const u8 session[32],
                        const u8 pub[32])
{
    static u8 m[512];
    int n, hl = 0, i, ext_at, sni_len;

    while (host[hl]) hl++;
    n = 5 + 4;                           /* record header, message header  */
    m[n++] = 3; m[n++] = 3;              /* "TLS 1.2", as 1.3 insists       */
    for (i = 0; i < 32; i++) m[n++] = random[i];
    m[n++] = 32;                         /* a session id, for middleboxes  */
    for (i = 0; i < 32; i++) m[n++] = session[i];
    put16(m + n, 2); n += 2;
    m[n++] = 0x13; m[n++] = 0x03;        /* TLS_CHACHA20_POLY1305_SHA256   */
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

    /* supported_groups: x25519 only. */
    put16(m + n, 0x000a); n += 2; put16(m + n, 4); n += 2;
    put16(m + n, 2); n += 2; put16(m + n, 0x001d); n += 2;

    /* signature_algorithms: what the server may sign with.  Not checked
     * yet, but a server will refuse a client that names none it has. */
    {
        static const u16 sa[] = { 0x0403, 0x0804, 0x0401, 0x0503, 0x0805,
                                  0x0501, 0x0806, 0x0601, 0x0807 };
        int k, cnt = (int)(sizeof sa / sizeof sa[0]);
        put16(m + n, 0x000d); n += 2;
        put16(m + n, (u32)(2 + 2 * cnt)); n += 2;
        put16(m + n, (u32)(2 * cnt)); n += 2;
        for (k = 0; k < cnt; k++) { put16(m + n, sa[k]); n += 2; }
    }

    /* supported_versions: 1.3, and nothing else. */
    put16(m + n, 0x002b); n += 2; put16(m + n, 3); n += 2;
    m[n++] = 2; m[n++] = 3; m[n++] = 4;

    /* key_share: our half of the secret. */
    put16(m + n, 0x0033); n += 2; put16(m + n, 38); n += 2;
    put16(m + n, 36); n += 2;
    put16(m + n, 0x001d); n += 2; put16(m + n, 32); n += 2;
    for (i = 0; i < 32; i++) m[n++] = pub[i];

    put16(m + ext_at, (u32)(n - ext_at - 2));

    m[5] = 1;                            /* ClientHello                    */
    m[6] = (u8)((n - 9) >> 16); m[7] = (u8)((n - 9) >> 8); m[8] = (u8)(n - 9);
    m[0] = CT_HANDSHAKE; m[1] = 3; m[2] = 1;
    put16(m + 3, (u32)(n - 5));

    sha256_update(&s_transcript, m + 5, (u32)(n - 5));
    return tcp_write(m, n) == n;
}

/* The server's half of the secret, out of its ServerHello. */
static int server_hello(const u8 *b, int len, u8 server_pub[32])
{
    static const u8 hrr[32] = {
        0xCF, 0x21, 0xAD, 0x74, 0xE5, 0x9A, 0x61, 0x11, 0xBE, 0x1D, 0x8C, 0x02,
        0x1E, 0x65, 0xB8, 0x91, 0xC2, 0xA2, 0x11, 0x16, 0x7A, 0xBB, 0x8C, 0x5E,
        0x07, 0x9E, 0x09, 0xE2, 0xC8, 0xA8, 0x33, 0x9C };
    int p, end, sid, got_ver = 0, got_key = 0;

    if (len < 38) { why("its hello was too short"); return 0; }
    if (same(b + 2, hrr, 32)) { why("it wanted a key exchange we do not do"); return 0; }
    p = 34;
    sid = b[p++];
    p += sid;
    if (p + 3 > len) { why("its hello did not add up"); return 0; }
    if (get16(b + p) != 0x1303) { why("it chose a cipher we did not offer"); return 0; }
    p += 3;
    if (p + 2 > len) { why("its hello had no extensions"); return 0; }
    end = p + 2 + (int)get16(b + p);
    p += 2;
    if (end > len) end = len;
    while (p + 4 <= end) {
        u32 type = get16(b + p), elen = get16(b + p + 2);
        const u8 *e = b + p + 4;
        if (p + 4 + (int)elen > end) break;
        if (type == 0x002b && elen == 2 && get16(e) == 0x0304) got_ver = 1;
        if (type == 0x0033 && elen == 36 && get16(e) == 0x001d && get16(e + 2) == 32) {
            int i;
            for (i = 0; i < 32; i++) server_pub[i] = e[4 + i];
            got_key = 1;
        }
        p += 4 + (int)elen;
    }
    if (!got_ver) { why("it does not speak TLS 1.3"); return 0; }
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
    if (!ct) { why("a handshake record failed its seal"); return 0; }
    if (ct == CT_ALERT) { why(alert_name(plen >= 2 ? s_plain[1] : 0)); return 0; }
    if (ct != CT_HANDSHAKE) { why("it sent data before finishing its hello"); return 0; }
    if (s_hsbuf_len + plen > HS_MAX) { why("its certificates were too big for us"); return 0; }
    for (i = 0; i < plen; i++) s_hsbuf[s_hsbuf_len + i] = s_plain[i];
    s_hsbuf_len += plen;
    return 1;
}

int tls_connect(const char *host)
{
    u8 secret[32], pub[32], random[32], session[32], server_pub[32];
    u8 shared[32], early[32], derived[32], hs_secret[32], master[32];
    u8 cli_hs[32], srv_hs[32], cli_ap[32], srv_ap[32], zero[32], h_empty[32];
    u8 h[32], fkey[32], mac[32], fin[4 + 32];
    u8 type;
    int len, i, mt, ml, done = 0;
    const u8 *body;

    s_up = 0;
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

    sha256_init(&s_transcript);
    if (!client_hello(host, random, session, pub)) { why("could not send our hello"); return 0; }

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
    if (!server_hello(s_rec + 9, ml, server_pub)) return 0;
    sha256_update(&s_transcript, s_rec + 5, (u32)(ml + 4));

    /* The shared secret, and the ladder of keys from it. */
    x25519(shared, secret, server_pub);
    for (i = 0; i < 32; i++) zero[i] = 0;
    sha256(0, 0, h_empty);
    hkdf_extract(0, 0, zero, 32, early);
    expand_label(early, "derived", h_empty, 32, derived, 32);
    hkdf_extract(derived, 32, shared, 32, hs_secret);
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
            /* Its proof: an HMAC, keyed from its handshake secret, of the
             * hash of every message before this one.  It matching means
             * nobody changed a byte of the handshake on the way. */
            transcript_now(h);
            expand_label(srv_hs, "finished", 0, 0, fkey, 32);
            hmac_sha256(fkey, 32, h, 32, mac);
            if (ml != 32 || !same(mac, body, 32)) {
                why("its proof of the handshake did not match");
                return 0;
            }
            drop_message(ml);
            done = 1;
        } else if (mt == HS_ENC_EXT || mt == HS_CERT || mt == HS_CERT_VERIFY) {
            drop_message(ml);            /* certificate not checked: see top */
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
        if (type != CT_APPDATA) { why("an unencrypted record after the handshake"); s_up = 0; return -2; }
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
