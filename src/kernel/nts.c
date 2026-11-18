/* ===========================================================================
 * nts.c -- the time from the network, signed (Network Time Security,
 * RFC 8915).
 *
 * Plain NTP has no signature: somebody on the same network could answer
 * first with a time of their choosing, and a clock wound back makes an
 * expired certificate look good.  NTS closes that:
 *
 *   1. Key exchange, once: a TLS 1.3 connection to the time server's
 *      NTS-KE port (4460), asking for protocol "ntske/1" (ALPN), with the
 *      certificate checked as for any secure site.  Over it, a few short
 *      records agree on NTPv4 and the cipher AEAD_AES_SIV_CMAC_256, and
 *      the server hands back eight cookies.  Two keys are exported from
 *      the TLS session itself, one each way.
 *   2. Each time it is asked: an ordinary NTP packet carrying a fresh
 *      random identifier, one cookie, and a tag over the whole packet with
 *      our key.  The answer must carry the same identifier and a tag that
 *      opens with the server's key; inside it are new cookies.  A cookie
 *      is used once, so nobody can link one request to the next.
 *
 * How far to trust the network beyond that is a setting (the clock menu,
 * clockui.c, kept in STATE.BIN): signed only; signed, then plain NTP from
 * the time server the local network names (DHCP); or signed, then any.
 * Asked when the network comes up.  The key exchange is never made while
 * an app has its connection open (there is only the one).
 * ===========================================================================
 */
#include "flopos.h"

int  conn_open(const char *host, u16 port, int secure, void (*note)(const char *t));
int  conn_write(const u8 *d, int n);
int  conn_read(u8 *b, int n, int wait_ms);
void conn_close(void);
const char *conn_why(void);

/* Servers that wait for a 120 MHz machine: its TLS handshake, with the
 * certificate chain checked, takes about two seconds, and some (Cloudflare's,
 * time.nl's) hang up before it is done. */
static const char *const s_default[3] = { "nts.netnod.se", "ptbtime1.ptb.de", "ptbtime2.ptb.de" };
static char s_srv[3][64];                /* the signed servers, in order   */
static int  s_trust = 1, s_loaded;
static char s_source[80];                /* where the time last came from  */
#define KE_PORT  4460
#define AEAD_SIV 15                      /* AEAD_AES_SIV_CMAC_256          */
#define COOKIES  8
#define COOKIE_MAX 256
#define LABEL "EXPORTER-network-time-security"

static u8   s_c2s[32], s_s2c[32];        /* our key, the server's          */
static u8   s_cookie[COOKIES][COOKIE_MAX];
static u16  s_cookie_len[COOKIES];
static int  s_ncookies;
static char s_host[64];                  /* the NTP server, as the KE named it */
static u16  s_port;

static void put16(u8 *p, u32 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
static u32  get16(const u8 *p) { return ((u32)p[0] << 8) | p[1]; }
static u32  get32(const u8 *p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }

static void keep_cookie(const u8 *c, u32 n)
{
    u32 i;
    if (n == 0 || n > COOKIE_MAX || s_ncookies >= COOKIES) return;
    for (i = 0; i < n; i++) s_cookie[s_ncookies][i] = c[i];
    s_cookie_len[s_ncookies++] = (u16)n;
}

/* 1. The key exchange.  1 with keys and cookies in hand. */
static int key_exchange(const char *host)
{
    static const u8 ask[] = {
        0x80, 0x01, 0x00, 0x02, 0x00, 0x00,          /* next protocol: NTPv4 (critical) */
        0x80, 0x04, 0x00, 0x02, 0x00, AEAD_SIV,      /* the cipher                      */
        0x80, 0x00, 0x00, 0x00                       /* end of message                  */
    };
    static u8 r[4096];
    u8 ctx[5] = { 0, 0, 0, AEAD_SIV, 0 };
    int n = 0, got, ok = 0, proto = 0, aead = 0, i, was = tls_checking();

    s_ncookies = 0; s_host[0] = 0; s_port = 123;
    tls_check(1);                                    /* always proved, whatever an app chose */
    tls_alpn("ntske/1");
    got = conn_open(host, KE_PORT, 1, 0);
    tls_alpn("");
    tls_check(was);
    if (!got) { serial_printf("[clock] %s: no signed time (%s)\n", host, conn_why()); return 0; }
    i = 0;
    if (conn_write(ask, (int)sizeof ask) != (int)sizeof ask) { serial_printf("[clock] %s: could not send the request\n", host); goto out; }
    /* The answer: records until end of message, or the server hangs up. */
    for (;;) {
        int at = 0, end = 0;
        while (at + 4 <= n) {
            u32 t = get16(r + at), len = get16(r + at + 2);
            if (at + 4 + (int)len > n) break;
            at += 4 + (int)len;
            if ((t & 0x7FFF) == 0) { end = 1; break; }
        }
        if (end) break;
        if (n >= (int)sizeof r) goto out;
        got = conn_read(r + n, (int)sizeof r - n, 4000);
        if (got <= 0) { if (!n) serial_printf("[clock] %s: no answer (%d: %s)\n", host, got, tls_why()); break; }
        n += got;
    }
    for (i = 0; i + 4 <= n; ) {
        u32 t = get16(r + i), len = get16(r + i + 2), type = t & 0x7FFF;
        const u8 *b = r + i + 4;
        if (i + 4 + (int)len > n) goto out;
        if (type == 0) { ok = 1; break; }
        if (type == 1) { if (len >= 2 && get16(b) == 0) proto = 1; }
        else if (type == 2) { serial_printf("[clock] %s: refused (%u)\n", host, len >= 2 ? get16(b) : 0); goto out; }
        else if (type == 4) { if (len >= 2 && get16(b) == AEAD_SIV) aead = 1; }
        else if (type == 5) keep_cookie(b, len);
        else if (type == 6) {
            u32 k;
            for (k = 0; k < len && k < sizeof s_host - 1; k++) s_host[k] = (char)b[k];
            s_host[k] = 0;
        }
        else if (type == 7) { if (len == 2) s_port = (u16)get16(b); }
        else if (t & 0x8000) goto out;               /* critical and unknown */
        i += 4 + (int)len;
    }
    ok = ok && proto && aead && s_ncookies > 0 &&
         tls_export(LABEL, ctx, 5, s_c2s, 32) &&
         (ctx[4] = 1, tls_export(LABEL, ctx, 5, s_s2c, 32));
out:
    conn_close();
    if (!ok) {
        serial_printf("[clock] %s: the key exchange did not finish (%u bytes; end %u, ntp %u, cipher %u, cookies %u)\n",
                      host, (u32)n, (u32)(i + 4 <= n), (u32)proto, (u32)aead, (u32)s_ncookies);
        s_ncookies = 0;
        return 0;
    }
    if (!s_host[0]) for (i = 0; host[i] && i < (int)sizeof s_host - 1; i++) s_host[i] = host[i], s_host[i + 1] = 0;
    serial_printf("[clock] %s: signed time agreed, %u cookies\n", host, (u32)s_ncookies);
    return 1;
}

/* 2. One signed question and answer.  UTC since 2000, or 0. */
static u32 ask_time(void)
{
    static u8 q[1024], a[1500], plain[1500];
    u8 uid[32], nonce[16], sent[8];
    const u8 *ad[2]; u32 al[2];
    u32 to, at, i, n, ck;
    int got, matched = 0, signedok = 0, k;

    if (!s_ncookies) return 0;
    if (!ip_parse(s_host, &to)) to = ip_lookup(s_host);   /* the KE may name it by address */
    if (!to) { serial_printf("[clock] cannot find the time server %s\n", s_host); return 0; }
    for (i = 0; i < 48; i++) q[i] = 0;
    q[0] = 0x23;                                     /* version 4, a client */
    rand_bytes(sent, 8);
    for (i = 0; i < 8; i++) q[40 + i] = sent[i];
    at = 48;
    rand_bytes(uid, 32);                             /* unique identifier */
    put16(q + at, 0x0104); put16(q + at + 2, 36);
    for (i = 0; i < 32; i++) q[at + 4 + i] = uid[i];
    at += 36;
    ck = (u32)--s_ncookies;                          /* one cookie, used once */
    n = s_cookie_len[ck];
    put16(q + at, 0x0204); put16(q + at + 2, 4 + ((n + 3) & ~3u));
    for (i = 0; i < ((n + 3) & ~3u); i++) q[at + 4 + i] = i < n ? s_cookie[ck][i] : 0;
    at += 4 + ((n + 3) & ~3u);
    for (k = s_ncookies; k < 3 && at + 4 + ((n + 3) & ~3u) + 40 < sizeof q; k++) {   /* ask for more */
        put16(q + at, 0x0304); put16(q + at + 2, 4 + ((n + 3) & ~3u));
        for (i = 0; i < ((n + 3) & ~3u); i++) q[at + 4 + i] = 0;
        at += 4 + ((n + 3) & ~3u);
    }
    rand_bytes(nonce, 16);                           /* the tag over all of it */
    put16(q + at, 0x0404); put16(q + at + 2, 40);
    put16(q + at + 4, 16); put16(q + at + 6, 16);
    for (i = 0; i < 16; i++) q[at + 8 + i] = nonce[i];
    ad[0] = q; al[0] = at; ad[1] = nonce; al[1] = 16;
    aessiv_seal(s_c2s, ad, al, 2, 0, 0, q + at + 24);
    at += 40;

    got = ip_udp_ask(to, s_port, q, (int)at, a, (int)sizeof a, 2000);
    if (got < 48) { serial_printf("[clock] %s: no answer to the signed question (%u bytes sent)\n", s_host, at); return 0; }
    if ((a[0] & 7) != 4 || a[1] == 0 || a[1] > 15) {                /* a server, not refusing */
        serial_printf("[clock] %s: refused (stratum %u, %c%c%c%c)\n", s_host, (u32)a[1], a[12], a[13], a[14], a[15]);
        return 0;
    }
    for (i = 0; i < 8; i++) if (a[24 + i] != sent[i]) return 0;  /* our question */
    for (i = 48; i + 4 <= (u32)got; ) {
        u32 t = get16(a + i), len = get16(a + i + 2);
        if (len < 4 || (len & 3) || i + len > (u32)got) return 0;
        if (t == 0x0104 && len == 36) {
            for (k = 0, matched = 1; k < 32; k++) if (a[i + 4 + k] != uid[k]) matched = 0;
        } else if (t == 0x0404 && len >= 8) {
            u32 nl = get16(a + i + 4), cl = get16(a + i + 6), np = (nl + 3) & ~3u;
            if (8 + np + cl > len || cl < 16 || cl - 16 > sizeof plain) return 0;
            ad[0] = a; al[0] = i; ad[1] = a + i + 8; al[1] = nl;
            if (!aessiv_open(s_s2c, ad, al, 2, a + i + 8 + np, cl, plain)) { serial_printf("[clock] the time's signature did not check out\n"); return 0; }
            signedok = 1;
            for (k = 0; k + 4 <= (int)cl - 16; ) {       /* new cookies, inside */
                u32 et = get16(plain + k), el = get16(plain + k + 2);
                if (el < 4 || k + (int)el > (int)cl - 16) break;
                if (et == 0x0204) keep_cookie(plain + k + 4, el - 4);
                k += (int)el;
            }
            break;                                       /* nothing after it is believed */
        }
        i += len;
    }
    if (!matched || !signedok) {
        serial_printf("[clock] an answer that was not signed for us: not believed\n");
        return 0;
    }
    return get32(a + 40) - 3155673600u;              /* since 2000; past 2036 too */
}

/* The settings, from the floppy, or the defaults. */
static void load(void)
{
    u32 t;
    int i, k;
    if (s_loaded) return;
    s_loaded = 1;
    if (store_clockset_get(&t, s_srv) && t <= 2) { s_trust = (int)t; return; }
    s_trust = 1;
    for (i = 0; i < 3; i++) {
        for (k = 0; s_default[i][k]; k++) s_srv[i][k] = s_default[i][k];
        s_srv[i][k] = 0;
    }
}

int nts_trust(void) { load(); return s_trust; }
const char *nts_server(int i) { load(); return i >= 0 && i < 3 ? s_srv[i] : ""; }
const char *nts_source(void) { return s_source; }

int nts_settings(int trust, const char server[3][64])
{
    int i, k;
    load();
    s_trust = trust < 0 || trust > 2 ? 1 : trust;
    for (i = 0; i < 3; i++) {
        for (k = 0; k < 63 && server[i][k]; k++) s_srv[i][k] = server[i][k];
        s_srv[i][k] = 0;
    }
    s_ncookies = 0;                              /* other servers: a fresh exchange */
    return store_clockset_set((u32)s_trust, s_srv);
}

/* Plain NTP, unsigned, from one server: the time it says, or 0.  Its
 * answer must come from that address, to the port asked from, and carry
 * back our random transmit time. */
static u32 plain_time(u32 to)
{
    u8 q[48], a[96], sent[8];
    int i, got;
    for (i = 0; i < 48; i++) q[i] = 0;
    q[0] = 0x23;
    rand_bytes(sent, 8);
    for (i = 0; i < 8; i++) q[40 + i] = sent[i];
    got = ip_udp_ask(to, 123, q, 48, a, (int)sizeof a, 1500);
    if (got < 48 || (a[0] & 7) != 4 || a[1] == 0 || a[1] > 15) return 0;
    for (i = 0; i < 8; i++) if (a[24 + i] != sent[i]) return 0;
    return get32(a + 40) - 3155673600u;
}

static void source(const char *what, u32 ip, const char *how)
{
    if (what) fmt_str(s_source, sizeof s_source, "%s, %s", what, how);
    else fmt_str(s_source, sizeof s_source, "%u.%u.%u.%u, %s", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255, how);
}

u32 nts_time(void)
{
    u32 t;
    int s;
    load();
    if (!conn_busy())
        for (s = 0; s < 3; s++) {
            if (!s_srv[s][0]) continue;
            if (!s_ncookies && !key_exchange(s_srv[s])) continue;
            t = ask_time();
            if (t) { source(s_srv[s], 0, "signed"); return t; }
            s_ncookies = 0;                              /* a fresh exchange next time */
        }
    if (s_trust >= 1 && ip_ntp_server()) {               /* the local network's own */
        t = plain_time(ip_ntp_server());
        if (t) { source(0, ip_ntp_server(), "the network's own, not signed"); return t; }
    }
    if (s_trust == 2) {
        u32 to = ip_lookup("pool.ntp.org");
        t = to ? plain_time(to) : 0;
        if (t) { source("pool.ntp.org", 0, "not signed"); return t; }
    }
    fmt_str(s_source, sizeof s_source, "nothing %s answered", s_trust ? "trusted" : "signed");
    return 0;
}
