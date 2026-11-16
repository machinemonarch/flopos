/* ===========================================================================
 * num.c -- numbers: reading them, writing them, and the maths.
 *
 * JavaScript has one kind of number, a 64-bit double.  Reading "0.1" and
 * writing it back as "0.1" (not 0.1000000000000000055...) takes care: a
 * number is written with the fewest digits that read back as exactly the
 * same number.  The Pentium's floating-point unit works in 80 bits, which
 * gives the arithmetic here room to spare.
 *
 * The maths functions are the floating-point unit's own instructions
 * (fsqrt, fsin, fpatan, fyl2x, f2xm1): there is no C library here.
 * ===========================================================================
 */
#include "js.h"

typedef long double ld;

f64 m_nan(void) { union { u64 u; f64 d; } x; x.u = 0x7FF8000000000000ull; return x.d; }
f64 m_inf(void) { union { u64 u; f64 d; } x; x.u = 0x7FF0000000000000ull; return x.d; }
int m_isnan(f64 x) { return x != x; }
int m_isinf(f64 x) { return x == m_inf() || x == -m_inf(); }

static ld p10(int e)                     /* 10^e, 0 <= e < 8192              */
{
    static const ld small[] = { 1e0L, 1e1L, 1e2L, 1e3L, 1e4L, 1e5L, 1e6L, 1e7L, 1e8L, 1e9L,
        1e10L, 1e11L, 1e12L, 1e13L, 1e14L, 1e15L, 1e16L, 1e17L, 1e18L, 1e19L, 1e20L, 1e21L, 1e22L };
    static const ld pow2[] = { 1e1L, 1e2L, 1e4L, 1e8L, 1e16L, 1e32L, 1e64L, 1e128L, 1e256L,
        1e512L, 1e1024L, 1e2048L, 1e4096L };
    ld r = 1;
    int i;
    if (e <= 22) return small[e];
    for (i = 0; e && i < 13; i++, e >>= 1) if (e & 1) r *= pow2[i];
    return r;
}

static int dig(u32 c) { return c >= '0' && c <= '9' ? (int)(c - '0') : -1; }

/* A decimal number at s (sign, digits, point, exponent): how many
 * characters it took (0: none). */
static u32 decimal(const u16 *s, u32 n, f64 *out)
{
    u32 i = 0, digits = 0;
    int neg = 0, e = 0, any = 0;
    u64 m = 0;
    ld r;
    if (i < n && (s[i] == '+' || s[i] == '-')) neg = s[i++] == '-';
    if (i + 8 <= n && s[i] == 'I') {
        const char *inf = "Infinity";
        u32 k;
        for (k = 0; k < 8 && s[i + k] == (u16)inf[k]; k++) { }
        if (k == 8) { *out = neg ? -m_inf() : m_inf(); return i + 8; }
    }
    while (i < n && dig(s[i]) >= 0) {
        any = 1;
        if (digits < 19) { m = m * 10 + (u64)dig(s[i]); if (m) digits++; }
        else e++;
        i++;
    }
    if (i < n && s[i] == '.') {
        u32 at = i++;
        int frac = 0;
        while (i < n && dig(s[i]) >= 0) {
            frac = 1;
            if (digits < 19) { m = m * 10 + (u64)dig(s[i]); if (m) digits++; e--; }
            i++;
        }
        if (!any && !frac) { i = at; }
        any |= frac;
    }
    if (!any) return 0;
    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        u32 j = i + 1;
        int en = 0, eneg = 0, ed = 0;
        if (j < n && (s[j] == '+' || s[j] == '-')) eneg = s[j++] == '-';
        while (j < n && dig(s[j]) >= 0) { if (en < 10000) en = en * 10 + dig(s[j]); j++; ed = 1; }
        if (ed) { e += eneg ? -en : en; i = j; }
    }
    r = (ld)m;
    if (m == 0) r = 0;
    else if (e > 0) r = e > 400 ? m_inf() : r * p10(e);
    else if (e < 0) r = e < -400 ? 0 : r / p10(-e);
    *out = (f64)(neg ? -r : r);
    return i;
}

static int ws(u32 c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0x0B || c == 0x0C ||
           c == 0xA0 || c == 0xFEFF || c == 0x2028 || c == 0x2029 || (c >= 0x2000 && c <= 0x200A);
}

/* Number("...") */
f64 num_parse(const u16 *s, u32 n, int *ok)
{
    u32 i = 0, used;
    f64 v;
    while (i < n && ws(s[i])) i++;
    while (n > i && ws(s[n - 1])) n--;
    *ok = 1;
    if (i == n) return 0;
    if (n - i > 2 && s[i] == '0' && (s[i + 1] | 32) >= 'b' && ((s[i + 1] | 32) == 'x' || (s[i + 1] | 32) == 'o' || (s[i + 1] | 32) == 'b')) {
        int base = (s[i + 1] | 32) == 'x' ? 16 : (s[i + 1] | 32) == 'o' ? 8 : 2;
        ld r = 0;
        for (i += 2; i < n; i++) {
            u32 c = s[i] | 32;
            int d = c >= '0' && c <= '9' ? (int)(c - '0') : c >= 'a' && c <= 'f' ? (int)(c - 'a' + 10) : 99;
            if (d >= base) { *ok = 0; return m_nan(); }
            r = r * base + d;
        }
        return (f64)r;
    }
    used = decimal(s + i, n - i, &v);
    if (!used || i + used != n) { *ok = 0; return m_nan(); }
    return v;
}

f64 num_parse_prefix(const u16 *s, u32 n, u32 *used)
{
    u32 i = 0, u;
    f64 v;
    while (i < n && ws(s[i])) i++;
    u = decimal(s + i, n - i, &v);
    *used = u ? i + u : 0;
    return u ? v : m_nan();
}

/* ---- writing numbers --------------------------------------------------------- */
/* The digits of v (> 0) to p significant places: into d, and the decimal
 * exponent of the first. */
static void digits_of(f64 v, int p, char *d, int *exp10)
{
    ld x = v, scale;
    int e = 0, i;
    u64 m;
    /* rough exponent */
    while (x >= 1e16L) { x /= 1e16L; e += 16; }
    while (x >= 10) { x /= 10; e++; }
    while (x < 1e-16L) { x *= 1e16L; e -= 16; }
    while (x < 1) { x *= 10; e--; }
    /* exact-ish: v / 10^(e - p + 1), rounded */
    if (e - p + 1 >= 0) scale = (ld)v / p10(e - p + 1);
    else scale = (ld)v * p10(-(e - p + 1));
    m = (u64)(scale + 0.5L);
    if (m >= (u64)p10(p)) { m = (m + 5) / 10; e++; }
    for (i = p - 1; i >= 0; i--) { d[i] = (char)('0' + m % 10); m /= 10; }
    d[p] = 0;
    *exp10 = e;
}

int num_fmt(f64 v, char *out)
{
    char d[24];
    int n = 0, p, e = 0, k, i;
    if (m_isnan(v)) { const char *s = "NaN"; while (*s) out[n++] = *s++; out[n] = 0; return n; }
    if (v == 0) { out[0] = '0'; out[1] = 0; return 1; }
    if (v < 0) { out[n++] = '-'; v = -v; }
    if (m_isinf(v)) { const char *s = "Infinity"; while (*s) out[n++] = *s++; out[n] = 0; return n; }
    if (v < 9007199254740992.0 && v == (f64)(u64)v) {   /* a whole number */
        u64 m = (u64)v;
        char t[24];
        k = 0;
        do { t[k++] = (char)('0' + m % 10); m /= 10; } while (m);
        while (k) out[n++] = t[--k];
        out[n] = 0;
        return n;
    }
    for (p = 1; p <= 17; p++) {
        u16 back[40];
        int ok, bl = 0;
        digits_of(v, p, d, &e);
        for (i = 0; d[i]; i++) back[bl++] = (u16)d[i];
        back[bl++] = 'e';
        {
            int ee = e - p + 1, neg = ee < 0;
            char t[8]; int tk = 0;
            if (neg) { back[bl++] = '-'; ee = -ee; }
            do { t[tk++] = (char)('0' + ee % 10); ee /= 10; } while (ee);
            while (tk) back[bl++] = (u16)t[--tk];
        }
        if (num_parse(back, (u32)bl, &ok) == v) break;
    }
    if (p > 17) p = 17;
    while (p > 1 && d[p - 1] == '0') d[--p] = 0;
    /* JS's rules: plain between 1e-7 and 1e21, else d.ddde+x */
    if (e >= 21 || e < -6) {
        out[n++] = d[0];
        if (p > 1) { out[n++] = '.'; for (i = 1; i < p; i++) out[n++] = d[i]; }
        out[n++] = 'e';
        out[n++] = e < 0 ? '-' : '+';
        {
            int ee = e < 0 ? -e : e; char t[8]; int tk = 0;
            do { t[tk++] = (char)('0' + ee % 10); ee /= 10; } while (ee);
            while (tk) out[n++] = t[--tk];
        }
    } else if (e < 0) {
        out[n++] = '0'; out[n++] = '.';
        for (i = 0; i < -e - 1; i++) out[n++] = '0';
        for (i = 0; i < p; i++) out[n++] = d[i];
    } else {
        for (i = 0; i <= e; i++) out[n++] = i < p ? d[i] : '0';
        if (p > e + 1) { out[n++] = '.'; for (i = e + 1; i < p; i++) out[n++] = d[i]; }
    }
    out[n] = 0;
    return n;
}

Str *num_to_str(f64 v, int radix)
{
    char b[80];
    int n;
    if (radix == 10 || radix < 2 || radix > 36 || m_isnan(v) || m_isinf(v)) {
        num_fmt(v, b);
        return str_ascii(b);
    }
    {
        int neg = v < 0, k = 0, i;
        f64 ip, fp;
        char t[80];
        if (neg) v = -v;
        ip = m_floor(v); fp = v - ip;
        n = 0;
        if (neg) b[n++] = '-';
        do { f64 q = m_floor(ip / radix); int d = (int)(ip - q * radix); t[k++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); ip = q; } while (ip > 0 && k < 70);
        while (k) b[n++] = t[--k];
        if (fp > 0) {
            b[n++] = '.';
            for (i = 0; i < 20 && fp > 0 && n < 78; i++) { int d; fp *= radix; d = (int)fp; fp -= d; b[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); }
        }
        b[n] = 0;
        return str_ascii(b);
    }
}

/* ---- conversions JS's bitwise operators use --------------------------------- */
s32 to_int32(f64 x) { return (s32)to_uint32(x); }

u32 to_uint32(f64 x)
{
    f64 t;
    if (m_isnan(x) || m_isinf(x)) return 0;
    t = m_trunc(x);
    t = m_fmod(t, 4294967296.0);
    if (t < 0) t += 4294967296.0;
    return (u32)(u64)t;
}

/* ---- maths with the floating-point unit ---------------------------------- */
f64 m_sqrt(f64 x) { ld r; __asm__("fsqrt" : "=t"(r) : "0"((ld)x)); return (f64)r; }

static ld big_reduce(ld x)               /* fsin wants |x| < 2^63            */
{
    ld r;
    if (x < 9e18L && x > -9e18L) return x;
    __asm__("1: fprem1; fnstsw %%ax; sahf; jp 1b" : "=t"(r) : "0"(x), "u"(6.28318530717958647692L) : "ax", "cc");
    return r;
}

f64 m_sin(f64 x) { ld r; if (m_isinf(x)) return m_nan(); __asm__("fsin" : "=t"(r) : "0"(big_reduce(x))); return (f64)r; }
f64 m_cos(f64 x) { ld r; if (m_isinf(x)) return m_nan(); __asm__("fcos" : "=t"(r) : "0"(big_reduce(x))); return (f64)r; }
f64 m_tan(f64 x) { ld r, one; if (m_isinf(x)) return m_nan(); __asm__("fptan" : "=t"(one), "=u"(r) : "0"(big_reduce(x))); return (f64)r; }
f64 m_atan2(f64 y, f64 x) { ld r; __asm__("fpatan" : "=t"(r) : "0"((ld)x), "u"((ld)y) : "st(1)"); return (f64)r; }

f64 m_trunc(f64 x)
{
    if (m_isnan(x) || m_isinf(x) || x >= 4503599627370496.0 || x <= -4503599627370496.0) return x;
    return (f64)(s64)x;
}

f64 m_floor(f64 x) { f64 t; if (x == 0) return x; t = m_trunc(x); return t > x ? t - 1 : t; }
f64 m_ceil(f64 x) { f64 t = m_trunc(x); if (t < x) t += 1; if (t == 0 && x < 0) return -0.0; return t; }
f64 m_round(f64 x)                       /* JS rounds halves up            */
{
    f64 f;
    if (m_isnan(x) || m_isinf(x)) return x;
    f = m_floor(x);
    if (x - f >= 0.5) f += 1;
    if (f == 0 && x < 0) return -0.0;
    return f;
}

/* 2^x for any x: f2xm1 takes |x| <= 1, fscale the whole part. */
static ld exp2l_(ld x)
{
    ld i, f, r;
    __asm__("frndint" : "=t"(i) : "0"(x));
    f = x - i;
    __asm__("f2xm1" : "=t"(r) : "0"(f));
    r += 1;
    __asm__("fscale" : "=t"(r) : "0"(r), "u"(i));
    return r;
}

f64 m_exp(f64 x)
{
    if (m_isnan(x)) return x;
    if (x > 11400) return m_inf();
    if (x < -11400) return 0;
    return (f64)exp2l_((ld)x * 1.44269504088896340736L);
}

f64 m_log(f64 x)
{
    ld r;
    if (m_isnan(x) || x < 0) return m_nan();
    if (x == 0) return -m_inf();
    if (m_isinf(x)) return x;
    __asm__("fyl2x" : "=t"(r) : "0"((ld)x), "u"(0.69314718055994530942L) : "st(1)");
    return (f64)r;
}

f64 m_fmod(f64 x, f64 y)
{
    ld r;
    if (m_isnan(x) || m_isnan(y) || m_isinf(x) || y == 0) return m_nan();
    if (m_isinf(y)) return x;
    __asm__("1: fprem; fnstsw %%ax; sahf; jp 1b" : "=t"(r) : "0"((ld)x), "u"((ld)y) : "ax", "cc");
    return (f64)r;
}

f64 m_pow(f64 x, f64 y)
{
    ld r;
    int neg = 0;
    if (m_isnan(y)) return m_nan();
    if (y == 0) return 1;
    if (m_isnan(x)) return m_nan();
    if (x == 1 && !m_isinf(y)) return 1;
    if (y == m_trunc(y) && y > -64 && y < 64) {          /* exact for small whole powers */
        ld b = x, acc = 1;
        int n = (int)y, inv = n < 0;
        if (inv) n = -n;
        while (n) { if (n & 1) acc *= b; b *= b; n >>= 1; }
        return (f64)(inv ? 1 / acc : acc);
    }
    if (x == 0) return y > 0 ? 0 : m_inf();
    if (m_isinf(y)) {
        f64 ax = x < 0 ? -x : x;
        if (ax == 1) return m_nan();
        return (ax > 1) == (y > 0) ? m_inf() : 0;
    }
    if (m_isinf(x)) return y > 0 ? (x < 0 && y == m_trunc(y) && m_fmod(y, 2) != 0 ? x : m_inf()) : 0;
    if (x < 0) {
        if (y != m_trunc(y)) return m_nan();
        neg = m_fmod(y, 2) != 0;
        x = -x;
    }
    __asm__("fyl2x" : "=t"(r) : "0"((ld)x), "u"((ld)y) : "st(1)");    /* y*log2(x) */
    if (r > 16400) r = m_inf();
    else if (r < -16400) r = 0;
    else r = exp2l_(r);
    return (f64)(neg ? -r : r);
}
