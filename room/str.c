/* ===========================================================================
 * str.c -- strings, as JavaScript has them: sixteen-bit characters.
 *
 * Property names are interned: one copy of each name, so finding a
 * property is comparing two pointers.  The names the engine itself uses
 * are made once and kept forever; names a script makes at run time are
 * forgotten again when nothing holds them.
 * ===========================================================================
 */
#include "js.h"

int js_strlen(const char *s) { int n = 0; while (s[n]) n++; return n; }

Str *str_new(u32 len)
{
    Str *s;
    if (len > 0x400000) return 0;
    s = gc_alloc(sizeof(Str) + len * 2, K_STR);
    if (s) s->len = len;
    return s;
}

Str *str_ascii(const char *a)
{
    u32 n = (u32)js_strlen(a), i;
    Str *s = str_new(n);
    if (s) for (i = 0; i < n; i++) s->c[i] = (u8)a[i];
    return s;
}

/* UTF-8 in; anything that does not hold together is taken as Latin-1. */
Str *str_utf8(const char *src, u32 n)
{
    const u8 *p = (const u8 *)src, *e = p + n;
    u32 len = 0, i;
    Str *s;
    while (p < e) {
        u32 c = *p++;
        int more = c >= 0xF0 && c <= 0xF4 ? 3 : c >= 0xE0 && c <= 0xEF ? 2 : c >= 0xC2 && c <= 0xDF ? 1 : 0;
        if (more) {
            const u8 *q = p; int m = more;
            while (m && q < e && (*q & 0xC0) == 0x80) { q++; m--; }
            if (!m) { p = q; if (more == 3) len++; }
        }
        len++;
    }
    s = str_new(len);
    if (!s) return 0;
    p = (const u8 *)src; i = 0;
    while (p < e && i < len) {
        u32 c = *p++;
        int more = c >= 0xF0 && c <= 0xF4 ? 3 : c >= 0xE0 && c <= 0xEF ? 2 : c >= 0xC2 && c <= 0xDF ? 1 : 0;
        if (more) {
            const u8 *q = p; u32 v = c & (0x3Fu >> more); int m = more;
            while (m && q < e && (*q & 0xC0) == 0x80) { v = (v << 6) | (*q++ & 0x3F); m--; }
            if (!m) { c = v; p = q; }
        }
        if (c >= 0x10000) {              /* a pair of surrogates            */
            c -= 0x10000;
            s->c[i++] = (u16)(0xD800 + (c >> 10));
            if (i < len) s->c[i++] = (u16)(0xDC00 + (c & 0x3FF));
        } else s->c[i++] = (u16)c;
    }
    return s;
}

Str *str_from16(const u16 *c, u32 n)
{
    Str *s = str_new(n);
    if (s) memcpy(s->c, c, n * 2);
    return s;
}

u32 str_hash(Str *s)
{
    u32 h = 2166136261u, i;
    if (s->hash) return s->hash;
    for (i = 0; i < s->len; i++) { h ^= s->c[i]; h *= 16777619u; }
    if (!h) h = 1;
    s->hash = h;
    return h;
}

int str_eq(Str *a, Str *b)
{
    u32 i;
    if (a == b) return 1;
    if (a->len != b->len) return 0;
    if (a->interned && b->interned) return 0;
    if (a->hash && b->hash && a->hash != b->hash) return 0;
    for (i = 0; i < a->len; i++) if (a->c[i] != b->c[i]) return 0;
    return 1;
}

int str_eq_ascii(Str *s, const char *a)
{
    u32 i;
    for (i = 0; i < s->len; i++) if (!a[i] || s->c[i] != (u8)a[i]) return 0;
    return a[i] == 0;
}

int str_cmp(Str *a, Str *b)
{
    u32 i, n = a->len < b->len ? a->len : b->len;
    for (i = 0; i < n; i++) if (a->c[i] != b->c[i]) return a->c[i] < b->c[i] ? -1 : 1;
    return a->len < b->len ? -1 : a->len > b->len;
}

Str *str_concat(Str *a, Str *b)
{
    Str *s;
    if (!a->len) return b;
    if (!b->len) return a;
    s = str_new(a->len + b->len);
    if (!s) return 0;
    memcpy(s->c, a->c, a->len * 2);
    memcpy(s->c + a->len, b->c, b->len * 2);
    return s;
}

Str *str_slice(Str *s, u32 from, u32 to)
{
    if (to > s->len) to = s->len;
    if (from >= to) return S_empty;
    if (from == 0 && to == s->len) return s;
    return str_from16(s->c + from, to - from);
}

int str_index(Str *s, Str *w, u32 from)
{
    u32 i, j;
    if (w->len == 0) return from <= s->len ? (int)from : (int)s->len;
    if (w->len > s->len) return -1;
    for (i = from; i + w->len <= s->len; i++) {
        for (j = 0; j < w->len && s->c[i + j] == w->c[j]; j++) { }
        if (j == w->len) return (int)i;
    }
    return -1;
}

int str_is_index(Str *s, u32 *out)
{
    u32 i, v = 0;
    if (!s->len || s->len > 10) return 0;
    if (s->len > 1 && s->c[0] == '0') return 0;
    for (i = 0; i < s->len; i++) {
        u32 d = s->c[i] - '0';
        if (d > 9) return 0;
        if (v > 429496729u || (v == 429496729u && d > 4)) return 0;
        v = v * 10 + d;
    }
    if (v == 0xFFFFFFFFu) return 0;
    *out = v;
    return 1;
}

u32 str_to_utf8(Str *s, char *out, u32 cap)
{
    u32 i, n = 0;
    for (i = 0; i < s->len; i++) {
        u32 c = s->c[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < s->len && s->c[i + 1] >= 0xDC00 && s->c[i + 1] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (s->c[i + 1] - 0xDC00);
            i++;
        }
        if (c < 0x80) { if (n + 1 > cap) break; out[n++] = (char)c; }
        else if (c < 0x800) { if (n + 2 > cap) break; out[n++] = (char)(0xC0 | (c >> 6)); out[n++] = (char)(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { if (n + 3 > cap) break; out[n++] = (char)(0xE0 | (c >> 12)); out[n++] = (char)(0x80 | ((c >> 6) & 0x3F)); out[n++] = (char)(0x80 | (c & 0x3F)); }
        else { if (n + 4 > cap) break; out[n++] = (char)(0xF0 | (c >> 18)); out[n++] = (char)(0x80 | ((c >> 12) & 0x3F)); out[n++] = (char)(0x80 | ((c >> 6) & 0x3F)); out[n++] = (char)(0x80 | (c & 0x3F)); }
    }
    return n;
}

/* ---- the intern table ------------------------------------------------------- */
#define ITAB 8192                        /* a power of two                   */
static Str *s_itab[ITAB];
static u32  s_icount;
#define TOMB ((Str *)1)

/* The interned string with these letters, made only if there is none:
 * the lexer asks this for every name it reads, and most are names it has
 * read before. */
Str *str_intern16(const u16 *c, u32 n)
{
    u32 h = 2166136261u, i;
    for (i = 0; i < n; i++) { h ^= c[i]; h *= 16777619u; }
    if (!h) h = 1;
    for (i = 0; i < ITAB; i++) {
        Str *t = s_itab[(h + i) & (ITAB - 1)];
        if (!t) break;
        if (t != TOMB && t->hash == h && t->len == n) {
            u32 k;
            for (k = 0; k < n && t->c[k] == c[k]; k++) { }
            if (k == n) return t;
        }
    }
    return str_intern(str_from16(c, n));
}

Str *str_intern(Str *s)
{
    u32 h, i, first_tomb = ITAB;
    if (!s) return 0;
    if (s->interned) return s;
    h = str_hash(s);
    for (i = 0; i < ITAB; i++) {
        u32 at = (h + i) & (ITAB - 1);
        Str *t = s_itab[at];
        if (!t) {
            if (first_tomb != ITAB) at = first_tomb;
            if (s_icount >= ITAB - ITAB / 8) return s;   /* full: works, just slower */
            s_itab[at] = s;
            s->interned = 1;
            s_icount++;
            return s;
        }
        if (t == TOMB) { if (first_tomb == ITAB) first_tomb = at; continue; }
        if (t->hash == h && t->len == s->len) {
            u32 k;
            for (k = 0; k < s->len && t->c[k] == s->c[k]; k++) { }
            if (k == s->len) return t;
        }
    }
    return s;
}

Str *atom(const char *a)
{
    Str *s = str_intern(str_ascii(a));
    if (s) s->h.extra = 1, s->h.mark = 2;   /* kept forever                  */
    return s;
}

void intern_mark(void) { }               /* atoms are marked forever already */

void intern_sweep(void)
{
    u32 i;
    for (i = 0; i < ITAB; i++) {
        Str *t = s_itab[i];
        if (t && t != TOMB && !t->h.mark) { s_itab[i] = TOMB; s_icount--; }
    }
}

Str *S_length, *S_prototype, *S_constructor, *S_name, *S_message,
    *S_toString, *S_valueOf, *S_empty, *S_undefined, *S_null,
    *S_true, *S_false, *S_object, *S_function, *S_number, *S_string,
    *S_boolean, *S_callee, *S_lastIndex, *S_index, *S_input,
    *S_get, *S_set, *S_value, *S_writable, *S_enumerable,
    *S_configurable, *S_then, *S_stack, *S_next, *S_done, *S_join,
    *S_NaN, *S_Infinity, *S_symbol, *S_toJSON;

void str_init_atoms(void)
{
    u32 i;
    for (i = 0; i < ITAB; i++) s_itab[i] = 0;
    s_icount = 0;
    S_length = atom("length"); S_prototype = atom("prototype");
    S_constructor = atom("constructor"); S_name = atom("name");
    S_message = atom("message"); S_toString = atom("toString");
    S_valueOf = atom("valueOf"); S_empty = atom("");
    S_undefined = atom("undefined"); S_null = atom("null");
    S_true = atom("true"); S_false = atom("false");
    S_object = atom("object"); S_function = atom("function");
    S_number = atom("number"); S_string = atom("string");
    S_boolean = atom("boolean"); S_callee = atom("callee");
    S_lastIndex = atom("lastIndex"); S_index = atom("index");
    S_input = atom("input"); S_get = atom("get"); S_set = atom("set");
    S_value = atom("value"); S_writable = atom("writable");
    S_enumerable = atom("enumerable"); S_configurable = atom("configurable");
    S_then = atom("then"); S_stack = atom("stack"); S_next = atom("next");
    S_done = atom("done"); S_join = atom("join"); S_NaN = atom("NaN");
    S_Infinity = atom("Infinity"); S_symbol = atom("symbol"); S_toJSON = atom("toJSON");
}
