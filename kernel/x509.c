/* ===========================================================================
 * x509.c -- is this site who it says it is?
 *
 * A secure site sends a chain of certificates: its own, which names it and
 * carries its public key, then one or more from the authorities that
 * vouch for it, each signed by the next.  The chain is believed if it ends
 * at an authority this machine already trusts (a root, from ROOTS.DAT on
 * the floppy, made from Mozilla's list), every signature in it checks out,
 * every certificate is in date by the laptop's own clock, every authority
 * in it is allowed to vouch, and the first certificate names the site that
 * was asked for.
 *
 * Certificates arrive as DER: nested tag, length, value.  Everything read
 * here comes from whoever answered, so every length is checked against
 * what is left before it is believed, and anything not understood is
 * refused rather than guessed at.  An extension marked critical that this
 * does not know refuses the certificate, as the standard says it must.
 *
 * What it does not do: ask whether a certificate has been revoked (that
 * needs another connection, to the authority, for every site), or fetch an
 * authority's certificate a site forgot to send.
 * ===========================================================================
 */
#include "flopos.h"
#include "io.h"

typedef struct { const u8 *p; int len; } Span;

/* ---- DER ------------------------------------------------------------------ */
/* One element at *p: its tag, its value, and (if asked) the whole thing,
 * header included.  *p moves past it. */
static int der(const u8 **p, const u8 *end, int *tag, Span *val, Span *whole)
{
    const u8 *q = *p, *start = *p;
    int len;

    if (end - q < 2) return 0;
    *tag = q[0];
    if ((q[0] & 0x1F) == 0x1F) return 0;         /* long tags: never in a cert */
    len = q[1];
    q += 2;
    if (len & 0x80) {
        int k = len & 0x7F;
        if (k < 1 || k > 3 || end - q < k) return 0;
        len = 0;
        while (k--) len = (len << 8) | *q++;
    }
    if (len < 0 || end - q < len) return 0;
    val->p = q;
    val->len = len;
    if (whole) { whole->p = start; whole->len = (int)(q + len - start); }
    *p = q + len;
    return 1;
}

static int want(const u8 **p, const u8 *end, int tag, Span *val, Span *whole)
{
    int t;
    const u8 *save = *p;
    if (!der(p, end, &t, val, whole) || t != tag) { *p = save; return 0; }
    return 1;
}

static int same(const Span *a, const u8 *b, int blen)
{
    int i;
    if (a->len != blen) return 0;
    for (i = 0; i < blen; i++) if (a->p[i] != b[i]) return 0;
    return 1;
}

static int same_span(const Span *a, const Span *b) { return same(a, b->p, b->len); }

/* ---- the numbers that name things ------------------------------------------ */
static const u8 OID_RSA[]       = { 0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x01,0x01 };
static const u8 OID_RSA_PSS[]   = { 0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x01,0x0A };
static const u8 OID_RSA_256[]   = { 0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x01,0x0B };
static const u8 OID_RSA_384[]   = { 0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x01,0x0C };
static const u8 OID_RSA_512[]   = { 0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x01,0x0D };
static const u8 OID_MGF1[]      = { 0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x01,0x08 };
static const u8 OID_EC[]        = { 0x2A,0x86,0x48,0xCE,0x3D,0x02,0x01 };
static const u8 OID_P256[]      = { 0x2A,0x86,0x48,0xCE,0x3D,0x03,0x01,0x07 };
static const u8 OID_P384[]      = { 0x2B,0x81,0x04,0x00,0x22 };
static const u8 OID_ECDSA_256[] = { 0x2A,0x86,0x48,0xCE,0x3D,0x04,0x03,0x02 };
static const u8 OID_ECDSA_384[] = { 0x2A,0x86,0x48,0xCE,0x3D,0x04,0x03,0x03 };
static const u8 OID_ECDSA_512[] = { 0x2A,0x86,0x48,0xCE,0x3D,0x04,0x03,0x04 };
static const u8 OID_SHA256[]    = { 0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x01 };
static const u8 OID_SHA384[]    = { 0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x02 };
static const u8 OID_SHA512[]    = { 0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x03 };
static const u8 OID_BASIC[]     = { 0x55,0x1D,0x13 };
static const u8 OID_KU[]        = { 0x55,0x1D,0x0F };
static const u8 OID_EKU[]       = { 0x55,0x1D,0x25 };
static const u8 OID_SAN[]       = { 0x55,0x1D,0x11 };
static const u8 OID_NC[]        = { 0x55,0x1D,0x1E };
static const u8 OID_POLICIES[]  = { 0x55,0x1D,0x20 };
static const u8 OID_POLMAP[]    = { 0x55,0x1D,0x21 };
static const u8 OID_POLCONS[]   = { 0x55,0x1D,0x24 };
static const u8 OID_INHIBIT[]   = { 0x55,0x1D,0x36 };
static const u8 OID_SERVER[]    = { 0x2B,0x06,0x01,0x05,0x05,0x07,0x03,0x01 };
static const u8 OID_ANY_EKU[]   = { 0x55,0x1D,0x25,0x00 };
static const u8 OID_ORG[]       = { 0x55,0x04,0x0A };
static const u8 OID_CN[]        = { 0x55,0x04,0x03 };
#define IS(span, oid) same(&(span), oid, (int)sizeof oid)

/* ---- keys and signatures ------------------------------------------------------ */
enum { KEY_NONE, KEY_RSA, KEY_P256, KEY_P384 };
enum { SIG_NONE, SIG_PKCS1, SIG_PSS, SIG_ECDSA };

typedef struct { int type; Span n, e, pt; } Key;

static int read_key(const Span *spki, Key *k)
{
    const u8 *p = spki->p, *end = spki->p + spki->len;
    Span alg, oid, bits, rest;
    const u8 *a, *aend;

    k->type = KEY_NONE;
    if (!want(&p, end, 0x30, &rest, 0) || p != end) return 0;   /* the wrapper */
    p = rest.p; end = rest.p + rest.len;
    if (!want(&p, end, 0x30, &alg, 0) || !want(&p, end, 0x03, &bits, 0)) return 0;
    if (bits.len < 2 || bits.p[0] != 0) return 0;             /* whole bytes  */
    a = alg.p; aend = alg.p + alg.len;
    if (!want(&a, aend, 0x06, &oid, 0)) return 0;

    if (IS(oid, OID_RSA)) {
        const u8 *q = bits.p + 1, *qend = bits.p + bits.len;
        if (!want(&q, qend, 0x30, &rest, 0)) return 0;
        q = rest.p; qend = rest.p + rest.len;
        if (!want(&q, qend, 0x02, &k->n, 0) || !want(&q, qend, 0x02, &k->e, 0)) return 0;
        k->type = KEY_RSA;
        return 1;
    }
    if (IS(oid, OID_EC)) {
        Span curve;
        if (!want(&a, aend, 0x06, &curve, 0)) return 0;
        k->pt.p = bits.p + 1;
        k->pt.len = bits.len - 1;
        if (IS(curve, OID_P256) && k->pt.len == 65) { k->type = KEY_P256; return 1; }
        if (IS(curve, OID_P384) && k->pt.len == 97) { k->type = KEY_P384; return 1; }
    }
    return 0;                                   /* anything else: not ours  */
}

static int hash_of(const Span *oid)
{
    if (IS(*oid, OID_SHA256)) return HASH_SHA256;
    if (IS(*oid, OID_SHA384)) return HASH_SHA384;
    if (IS(*oid, OID_SHA512)) return HASH_SHA512;
    return 0;
}

/* An AlgorithmIdentifier for a signature: which kind, which hash, and for
 * PSS the salt length. */
static int read_sig_alg(const Span *alg, int *kind, int *hash, int *salt)
{
    const u8 *p = alg->p, *end = alg->p + alg->len;
    Span oid, params;

    *kind = SIG_NONE; *hash = 0; *salt = 0;
    if (!want(&p, end, 0x06, &oid, 0)) return 0;
    if (IS(oid, OID_RSA_256)) { *kind = SIG_PKCS1; *hash = HASH_SHA256; return 1; }
    if (IS(oid, OID_RSA_384)) { *kind = SIG_PKCS1; *hash = HASH_SHA384; return 1; }
    if (IS(oid, OID_RSA_512)) { *kind = SIG_PKCS1; *hash = HASH_SHA512; return 1; }
    if (IS(oid, OID_ECDSA_256)) { *kind = SIG_ECDSA; *hash = HASH_SHA256; return 1; }
    if (IS(oid, OID_ECDSA_384)) { *kind = SIG_ECDSA; *hash = HASH_SHA384; return 1; }
    if (IS(oid, OID_ECDSA_512)) { *kind = SIG_ECDSA; *hash = HASH_SHA512; return 1; }
    if (IS(oid, OID_RSA_PSS)) {
        /* SEQUENCE { [0] hash, [1] MGF1(hash), [2] salt length } -- the
         * defaults are SHA-1, which is not accepted here. */
        const u8 *q, *qend;
        Span h, inner, o, m, s;
        if (!want(&p, end, 0x30, &params, 0)) return 0;
        q = params.p; qend = params.p + params.len;
        if (!want(&q, qend, 0xA0, &h, 0)) return 0;
        { const u8 *r = h.p, *rend = h.p + h.len;
          if (!want(&r, rend, 0x30, &inner, 0)) return 0;
          r = inner.p; rend = inner.p + inner.len;
          if (!want(&r, rend, 0x06, &o, 0)) return 0;
          *hash = hash_of(&o); }
        if (!*hash) return 0;
        if (!want(&q, qend, 0xA1, &m, 0)) return 0;
        { const u8 *r = m.p, *rend = m.p + m.len;
          Span mo, mp, ho, hp;
          if (!want(&r, rend, 0x30, &inner, 0)) return 0;
          r = inner.p; rend = inner.p + inner.len;
          if (!want(&r, rend, 0x06, &mo, 0) || !IS(mo, OID_MGF1)) return 0;
          if (!want(&r, rend, 0x30, &mp, 0)) return 0;
          r = mp.p; rend = mp.p + mp.len;
          if (!want(&r, rend, 0x06, &ho, 0) || hash_of(&ho) != *hash) return 0;
          (void)hp; }
        *salt = 20;
        if (want(&q, qend, 0xA2, &s, 0)) {
            const u8 *r = s.p, *rend = s.p + s.len;
            Span si;
            if (!want(&r, rend, 0x02, &si, 0) || si.len < 1 || si.len > 2) return 0;
            *salt = si.len == 1 ? si.p[0] : (si.p[0] << 8) | si.p[1];
        }
        *kind = SIG_PSS;
        return 1;
    }
    return 0;
}

/* Does this key sign these bytes so?  (Shared with the handshake.) */
static int check_sig(const Key *k, int kind, int hash, int salt,
                     const u8 *data, int dlen, const u8 *sig, int slen)
{
    u8 h[64];

    hash_buf(hash, data, (u32)dlen, h);
    switch (kind) {
    case SIG_PKCS1:
        return k->type == KEY_RSA &&
               rsa_verify_pkcs1(k->n.p, k->n.len, k->e.p, k->e.len, sig, slen, hash, h);
    case SIG_PSS:
        return k->type == KEY_RSA &&
               rsa_verify_pss(k->n.p, k->n.len, k->e.p, k->e.len, sig, slen, hash, h, salt);
    case SIG_ECDSA:
        if (k->type == KEY_P256) return ecdsa_verify(CURVE_P256, k->pt.p, k->pt.len, sig, slen, h, hash_len(hash));
        if (k->type == KEY_P384) return ecdsa_verify(CURVE_P384, k->pt.p, k->pt.len, sig, slen, h, hash_len(hash));
        return 0;
    }
    return 0;
}

/* ---- a certificate ------------------------------------------------------------- */
typedef struct {
    Span tbs, issuer, subject, spki, sig, san, nc;
    int  kind, hash, salt;
    u32  from_d, from_t, until_d, until_t;  /* YYYYMMDD, hhmmss            */
    int  ca, path_len;                   /* path_len -1: no limit           */
    int  ku, ku_sign;                    /* keyUsage present / keyCertSign  */
    int  eku, eku_server;
    int  strange;                        /* a critical extension we do not know */
} Cert;

/* A time as two numbers that sort the way time does: the date as
 * YYYYMMDD and the time of day as hhmmss.  UTCTime has two-digit years,
 * 50 and over meaning the 1900s. */
static int read_time(const Span *t, int tag, u32 *date, u32 *tod)
{
    u32 d = 0, h = 0;
    int i, n = tag == 0x17 ? 12 : 14, dn = n - 6;
    if (t->len != n + 1 || t->p[n] != 'Z') return 0;
    for (i = 0; i < n; i++) {
        if (t->p[i] < '0' || t->p[i] > '9') return 0;
        if (i < dn) d = d * 10 + (u32)(t->p[i] - '0');
        else        h = h * 10 + (u32)(t->p[i] - '0');
    }
    if (tag == 0x17) d += t->p[0] < '5' ? 20000000u : 19000000u;
    *date = d;
    *tod = h;
    return 1;
}

static int before(u32 ad, u32 at, u32 bd, u32 bt) { return ad < bd || (ad == bd && at < bt); }

static void read_extension(Cert *c, const Span *ext)
{
    const u8 *p = ext->p, *end = ext->p + ext->len, *q, *qend;
    Span oid, crit, val, inner, x;
    int critical = 0;

    if (!want(&p, end, 0x06, &oid, 0)) { c->strange = 1; return; }
    if (want(&p, end, 0x01, &crit, 0)) critical = crit.len == 1 && crit.p[0] != 0;
    if (!want(&p, end, 0x04, &val, 0)) { c->strange = 1; return; }
    q = val.p; qend = val.p + val.len;

    if (IS(oid, OID_BASIC)) {
        if (!want(&q, qend, 0x30, &inner, 0)) { c->strange = 1; return; }
        q = inner.p; qend = inner.p + inner.len;
        if (want(&q, qend, 0x01, &x, 0)) c->ca = x.len == 1 && x.p[0] != 0;
        if (want(&q, qend, 0x02, &x, 0)) c->path_len = x.len == 1 ? x.p[0] : 255;
    } else if (IS(oid, OID_KU)) {
        if (!want(&q, qend, 0x03, &x, 0) || x.len < 2) { c->strange = 1; return; }
        c->ku = 1;
        c->ku_sign = (x.p[1] & 0x04) != 0;       /* bit 5: keyCertSign      */
    } else if (IS(oid, OID_EKU)) {
        if (!want(&q, qend, 0x30, &inner, 0)) { c->strange = 1; return; }
        c->eku = 1;
        q = inner.p; qend = inner.p + inner.len;
        while (q < qend) {
            if (!want(&q, qend, 0x06, &x, 0)) { c->strange = 1; return; }
            if (IS(x, OID_SERVER) || IS(x, OID_ANY_EKU)) c->eku_server = 1;
        }
    } else if (IS(oid, OID_SAN)) {
        if (!want(&q, qend, 0x30, &c->san, 0)) c->strange = 1;
    } else if (IS(oid, OID_NC)) {
        if (!want(&q, qend, 0x30, &c->nc, 0)) c->strange = 1;
    } else if (IS(oid, OID_POLICIES) || IS(oid, OID_POLMAP) ||
               IS(oid, OID_POLCONS) || IS(oid, OID_INHIBIT)) {
        /* Policies: understood well enough to be allowed, not enforced --
         * the web does not use them to keep anyone out. */
    } else if (critical) {
        c->strange = 1;
    }
}

static int read_cert(const u8 *der_, int len, Cert *c)
{
    const u8 *p = der_, *end = der_ + len, *t, *tend;
    Span cert, alg, alg2, v, val, validity, from, until, exts;
    int tag, tag2, kind2, hash2, salt2;

    for (tag = 0; tag < (int)sizeof *c; tag++) ((u8 *)c)[tag] = 0;
    c->path_len = -1;

    if (!want(&p, end, 0x30, &cert, 0) || p != end) return 0;
    p = cert.p; end = cert.p + cert.len;
    if (!want(&p, end, 0x30, &val, &c->tbs)) return 0;
    if (!want(&p, end, 0x30, &alg, 0)) return 0;
    if (!want(&p, end, 0x03, &c->sig, 0) || c->sig.len < 2 || c->sig.p[0] != 0) return 0;
    c->sig.p++; c->sig.len--;
    if (!read_sig_alg(&alg, &c->kind, &c->hash, &c->salt)) return 0;

    t = val.p; tend = val.p + val.len;
    if (want(&t, tend, 0xA0, &v, 0)) {                 /* version: must be 3 */
        const u8 *r = v.p;
        Span ver;
        if (!want(&r, v.p + v.len, 0x02, &ver, 0) || ver.len != 1 || ver.p[0] != 2) return 0;
    }
    if (!want(&t, tend, 0x02, &v, 0)) return 0;        /* serial            */
    if (!want(&t, tend, 0x30, &alg2, 0)) return 0;     /* the same algorithm */
    if (!read_sig_alg(&alg2, &kind2, &hash2, &salt2) ||
        kind2 != c->kind || hash2 != c->hash || salt2 != c->salt) return 0;
    if (!want(&t, tend, 0x30, &v, &c->issuer)) return 0;
    if (!want(&t, tend, 0x30, &validity, 0)) return 0;
    {
        const u8 *r = validity.p, *rend = validity.p + validity.len;
        if (!der(&r, rend, &tag, &from, 0) || (tag != 0x17 && tag != 0x18)) return 0;
        if (!read_time(&from, tag, &c->from_d, &c->from_t)) return 0;
        if (!der(&r, rend, &tag2, &until, 0) || (tag2 != 0x17 && tag2 != 0x18)) return 0;
        if (!read_time(&until, tag2, &c->until_d, &c->until_t)) return 0;
    }
    if (!want(&t, tend, 0x30, &v, &c->subject)) return 0;
    if (!want(&t, tend, 0x30, &v, &c->spki)) return 0;
    while (t < tend) {
        int et;
        Span ev;
        if (!der(&t, tend, &et, &ev, 0)) return 0;
        if (et == 0xA3) {
            const u8 *r = ev.p, *rend = ev.p + ev.len;
            if (!want(&r, rend, 0x30, &exts, 0)) return 0;
            r = exts.p; rend = exts.p + exts.len;
            while (r < rend) {
                Span one;
                if (!want(&r, rend, 0x30, &one, 0)) return 0;
                read_extension(c, &one);
            }
        }
    }
    return 1;
}

/* ---- names ---------------------------------------------------------------- */
static int lower(int ch) { return ch >= 'A' && ch <= 'Z' ? ch + 32 : ch; }

static int eq_host(const u8 *a, int alen, const char *b, int blen)
{
    int i;
    if (alen != blen) return 0;
    for (i = 0; i < alen; i++) if (lower(a[i]) != lower((u8)b[i])) return 0;
    return 1;
}

/* A certificate name against the host asked for.  "*.example.com" stands
 * for exactly one label in front of example.com, and never for a whole
 * domain on its own ("*.com"). */
static int name_ok(const u8 *pat, int plen, const char *host)
{
    int hlen = strlen8(host), dots = 0, i, first;

    if (plen >= 2 && pat[0] == '*' && pat[1] == '.') {
        for (i = 2; i < plen; i++) if (pat[i] == '.') dots++;
        if (dots < 1) return 0;
        for (first = 0; first < hlen && host[first] != '.'; first++) { }
        if (first == 0 || first >= hlen) return 0;
        return eq_host(pat + 1, plen - 1, host + first, hlen - first);
    }
    return eq_host(pat, plen, host, hlen);
}

/* Is host inside the subtree an authority was limited to?  "example.com"
 * covers example.com and everything under it; ".example.com" only what is
 * under it. */
static int within(const u8 *base, int blen, const char *host)
{
    int hlen = strlen8(host);
    if (blen == 0) return 1;
    if (hlen == blen) return base[0] != '.' && eq_host(base, blen, host, hlen);
    if (hlen < blen) return 0;
    if (base[0] == '.') return eq_host(base, blen, host + hlen - blen, blen);
    return host[hlen - blen - 1] == '.' && eq_host(base, blen, host + hlen - blen, blen);
}

/* Name constraints on an authority, applied to the names the site's own
 * certificate carries.  Only names of the dNSName kind are looked at. */
static int nc_allows(const Cert *ca, const Cert *leaf)
{
    const u8 *p = ca->nc.p, *end = ca->nc.p + ca->nc.len;
    int pass;

    if (!ca->nc.len) return 1;
    for (pass = 0; pass < 2; pass++) {
        const u8 *q = p;
        while (q < end) {
            int tag;
            Span set, tmp;
            if (!der(&q, end, &tag, &set, 0)) return 0;
            if ((pass == 0 && tag != 0xA0) || (pass == 1 && tag != 0xA1)) continue;
            {
                const u8 *s = leaf->san.p, *send = leaf->san.p + leaf->san.len;
                while (s < send) {
                    int nt, any_dns = 0, hit = 0;
                    Span nm;
                    char host[256];
                    const u8 *r;
                    if (!der(&s, send, &nt, &nm, 0)) return 0;
                    if (nt != 0x82 || nm.len >= (int)sizeof host) continue;
                    { int i; for (i = 0; i < nm.len; i++) host[i] = (char)nm.p[i]; host[nm.len] = 0; }
                    if (host[0] == '*') { int i; for (i = 0; host[i + 1]; i++) host[i] = host[i + 1]; host[i] = 0; if (host[0] == '.') { for (i = 0; host[i + 1]; i++) host[i] = host[i + 1]; host[i] = 0; } }
                    r = set.p;
                    while (r < set.p + set.len) {
                        Span sub, base;
                        int bt;
                        const u8 *b;
                        if (!want(&r, set.p + set.len, 0x30, &sub, 0)) return 0;
                        b = sub.p;
                        if (!der(&b, sub.p + sub.len, &bt, &base, &tmp)) return 0;
                        if (bt != 0x82) continue;
                        any_dns = 1;
                        if (within(base.p, base.len, host)) hit = 1;
                    }
                    if (pass == 0 && any_dns && !hit) return 0;   /* outside what is permitted */
                    if (pass == 1 && hit) return 0;               /* inside what is excluded  */
                }
            }
        }
    }
    return 1;
}

/* The organisation, or failing that the common name, out of a Name. */
static void org_of(const Span *name, char *out, int cap)
{
    const u8 *p = name->p, *end = name->p + name->len;
    Span set, seq, oid, val, whole;
    char cn[64];
    int tag, i;

    out[0] = 0; cn[0] = 0;
    if (!want(&p, end, 0x30, &seq, &whole)) return;
    p = seq.p; end = seq.p + seq.len;
    while (p < end) {
        const u8 *s, *a;
        if (!want(&p, end, 0x31, &set, 0)) return;
        s = set.p;
        while (s < set.p + set.len) {
            Span atv;
            if (!want(&s, set.p + set.len, 0x30, &atv, 0)) return;
            a = atv.p;
            if (!want(&a, atv.p + atv.len, 0x06, &oid, 0)) continue;
            if (!der(&a, atv.p + atv.len, &tag, &val, 0)) continue;
            if (IS(oid, OID_ORG) || IS(oid, OID_CN)) {
                char *dst = IS(oid, OID_ORG) ? out : cn;
                int lim = IS(oid, OID_ORG) ? cap : (int)sizeof cn;
                for (i = 0; i < val.len && i < lim - 1; i++)
                    dst[i] = val.p[i] >= 32 && val.p[i] < 127 ? (char)val.p[i] : '?';
                dst[i] = 0;
            }
        }
    }
    if (!out[0]) for (i = 0; cn[i] && i < cap - 1; i++) out[i] = cn[i], out[i + 1] = 0;
}

/* ---- the roots ---------------------------------------------------------------
 * ROOTS.DAT: "FLOPROOT", a count, then for each root its subject Name and
 * its SubjectPublicKeyInfo, as DER, each after a 16-bit length. */
static u8  *s_roots;
static int  s_roots_len, s_nroots;

static int roots_load(void)
{
    FsFile f;
    int n;
    if (s_roots) return 1;
    if (!fs_mount() || !fs_find("ROOTS.DAT", &f) || f.size < 10 || f.size > 400000) return 0;
    s_roots = (u8 *)halloc(f.size);
    if (!s_roots) return 0;
    n = fs_read(&f, s_roots, f.size);
    if (n < 10 || !same(&(Span){ s_roots, 8 }, (const u8 *)"FLOPROOT", 8)) { s_roots = 0; return 0; }
    s_roots_len = n;
    s_nroots = (s_roots[8] << 8) | s_roots[9];
    return 1;
}

int x509_roots(void) { return roots_load() ? s_nroots : 0; }

/* The k'th root after *at: subject and key. */
static int root_next(int *at, Span *subject, Span *spki)
{
    int a = *at, l;
    if (a < 10) a = 10;
    if (a + 2 > s_roots_len) return 0;
    l = (s_roots[a] << 8) | s_roots[a + 1]; a += 2;
    if (a + l + 2 > s_roots_len) return 0;
    subject->p = s_roots + a; subject->len = l; a += l;
    l = (s_roots[a] << 8) | s_roots[a + 1]; a += 2;
    if (a + l > s_roots_len) return 0;
    spki->p = s_roots + a; spki->len = l; a += l;
    *at = a;
    return 1;
}

/* ---- the chain ------------------------------------------------------------------ */
#define CHAIN_MAX 8

static Cert s_c[CHAIN_MAX];
static Key  s_leaf_key;
static char s_who[64];

const char *x509_who(void) { return s_who; }

static void date_of(u32 d, char *out)
{
    fmt_str(out, 12, "%u-%02u-%02u", d / 10000, (d / 100) % 100, d % 100);
}

/* certs: DER certificates, the site's own first.  Returns 1 if the chain
 * is good for host; otherwise 0, with the reason in why. */
int x509_check(const u8 *const *certs, const int *lens, int n, const char *host,
               char *why, int whycap)
{
    u8 h, mi, se, mo, da;
    u16 yr;
    u32 nd, nt;
    int i, cur = 0, depth, hosted = 0, used[CHAIN_MAX];

    s_who[0] = 0;
    s_leaf_key.type = KEY_NONE;
    if (n < 1) { fmt_str(why, (usize)whycap, "it sent no certificate"); return 0; }
    if (n > CHAIN_MAX) n = CHAIN_MAX;
    for (i = 0; i < n; i++) {
        used[i] = 0;
        if (!read_cert(certs[i], lens[i], &s_c[i])) {
            fmt_str(why, (usize)whycap, i ? "one of its authorities' certificates could not be read"
                                          : "its certificate could not be read");
            return 0;
        }
    }
    if (!roots_load()) { fmt_str(why, (usize)whycap, "ROOTS.DAT is missing from this floppy"); return 0; }

    if (!rtc_read(&h, &mi, &se, &yr, &mo, &da) || yr < 2024) {
        fmt_str(why, (usize)whycap, "this laptop's clock says %u, so no certificate can be dated; once the network is up it sets itself",
                (u32)yr);
        return 0;
    }
    nd = (u32)yr * 10000u + (u32)mo * 100u + da;
    nt = (u32)h * 10000u + (u32)mi * 100u + se;

    /* The site's own certificate: its name, its dates, its purpose. */
    {
        Cert *c = &s_c[0];
        const u8 *p = c->san.p, *end = c->san.p + c->san.len;
        while (p < end) {
            int tag;
            Span nm;
            if (!der(&p, end, &tag, &nm, 0)) break;
            if (tag == 0x82 && name_ok(nm.p, nm.len, host)) hosted = 1;
        }
        if (!hosted) { fmt_str(why, (usize)whycap, "its certificate is for a different site"); return 0; }
        if (before(nd, nt, c->from_d, c->from_t)) { char d[12]; date_of(c->from_d, d); fmt_str(why, (usize)whycap, "its certificate is not valid until %s", d); return 0; }
        if (before(c->until_d, c->until_t, nd, nt)) { char d[12]; date_of(c->until_d, d); fmt_str(why, (usize)whycap, "its certificate ran out on %s", d); return 0; }
        if (c->eku && !c->eku_server) { fmt_str(why, (usize)whycap, "its certificate is not for websites"); return 0; }
        if (c->strange) { fmt_str(why, (usize)whycap, "its certificate makes a demand this machine does not understand"); return 0; }
        if (!read_key(&c->spki, &s_leaf_key)) { fmt_str(why, (usize)whycap, "its key is of a kind this machine cannot check"); return 0; }
    }

    /* Up the chain until an authority this machine trusts signed it. */
    for (depth = 0; depth < CHAIN_MAX; depth++) {
        Cert *c = &s_c[cur];
        Span subj, spki;
        Key k;
        int at = 0, found = -1, mis = 0;

        while (root_next(&at, &subj, &spki)) {
            if (!same_span(&subj, &c->issuer)) continue;
            if (read_key(&spki, &k) &&
                check_sig(&k, c->kind, c->hash, c->salt, c->tbs.p, c->tbs.len, c->sig.p, c->sig.len)) {
                org_of(&s_c[0].issuer, s_who, (int)sizeof s_who);
                return 1;
            }
            mis = 1;
        }
        for (i = 1; i < n && found < 0; i++) {
            Cert *a = &s_c[i];
            if (i == cur || used[i] || !same_span(&a->subject, &c->issuer)) continue;
            if (!a->ca || (a->ku && !a->ku_sign) || a->strange) continue;
            if (a->path_len >= 0 && a->path_len < depth) continue;
            if (before(nd, nt, a->from_d, a->from_t) || before(a->until_d, a->until_t, nd, nt)) continue;
            if (!read_key(&a->spki, &k)) continue;
            if (!check_sig(&k, c->kind, c->hash, c->salt, c->tbs.p, c->tbs.len, c->sig.p, c->sig.len)) { mis = 1; continue; }
            if (!nc_allows(a, &s_c[0])) { fmt_str(why, (usize)whycap, "an authority in its chain was not allowed to vouch for this name"); return 0; }
            found = i;
        }
        if (found < 0) {
            fmt_str(why, (usize)whycap, mis ? "a signature in its certificate chain does not check out"
                                            : "nobody this machine trusts vouches for it");
            return 0;
        }
        used[found] = 1;
        cur = found;
    }
    fmt_str(why, (usize)whycap, "its certificate chain goes on too long");
    return 0;
}

/* The handshake's own signature, made with the site's key: TLS 1.3 says
 * which scheme by number. */
int x509_check_handshake(int scheme, const u8 *content, int clen, const u8 *sig, int slen)
{
    const Key *k = &s_leaf_key;
    switch (scheme) {
    case 0x0403: return k->type == KEY_P256 && check_sig(k, SIG_ECDSA, HASH_SHA256, 0, content, clen, sig, slen);
    case 0x0503: return k->type == KEY_P384 && check_sig(k, SIG_ECDSA, HASH_SHA384, 0, content, clen, sig, slen);
    case 0x0804: return check_sig(k, SIG_PSS, HASH_SHA256, 32, content, clen, sig, slen);
    case 0x0805: return check_sig(k, SIG_PSS, HASH_SHA384, 48, content, clen, sig, slen);
    case 0x0806: return check_sig(k, SIG_PSS, HASH_SHA512, 64, content, clen, sig, slen);
    }
    return 0;
}
