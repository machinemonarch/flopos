/* ===========================================================================
 * lib2.c -- String, Date, Promise, and the timers.
 *
 * Dates: the laptop's clock has no time zone, so local time is UTC.
 *
 * Promises: a promise is pending, then kept or broken once.  What a
 * script asked to happen afterwards (then, catch) waits in a queue and
 * runs when the current piece of script has finished, as JavaScript
 * requires.
 *
 * Timers: kept here, run when browzer says their time has come.  A timer
 * given text instead of a function is refused: text is never run as code.
 * ===========================================================================
 */
#include "js.h"

#define ARG(i) ((i) < argc ? args[i] : v_undef())
#define NATIVE(name) static Val name(Vm *vm, Val self, Val *args, int argc, u32 magic)
#define UNUSED (void)self; (void)args; (void)argc; (void)magic

Str *array_join(Vm *vm, Obj *a, Str *sep, int depth);
void math_seed(u32 a, u32 b);

static Str *this_str(Vm *vm, Val self)
{
    if (self.t == T_STR) return self.u.s;
    if (IS_NULLISH(self)) { throw_err(vm, E_TYPE, "a String method on null or undefined"); return 0; }
    return to_str(vm, self);
}

static s32 clampi(Vm *vm, Val v, s32 lo, s32 hi, s32 dflt)
{
    f64 d;
    if (v.t == T_UNDEF) return dflt;
    d = m_trunc(to_num(vm, v));
    if (m_isnan(d)) d = 0;
    if (d < lo) return lo;
    if (d > hi) return hi;
    return (s32)d;
}

/* ---- String ------------------------------------------------------------------------- */
NATIVE(string_ctor)
{
    Str *s = argc ? to_str(vm, args[0]) : S_empty;
    UNUSED;
    if (vm->in_new) { Obj *o = obj_new(vm, vm->string_proto, C_BOXED); if (o) o->u.boxed.v = v_str(s); return o ? v_obj(o) : v_undef(); }
    return v_str(s);
}

/* String.raw`a${x}b`, or String.raw({raw: [...]}, x): the pieces as typed, the values between. */
NATIVE(string_raw)
{
    Val raw = IS_NULLISH(ARG(0)) ? v_undef() : get_prop(vm, ARG(0), atom("raw"));
    Str *r = str_ascii("");
    u32 n, i;
    UNUSED;
    if (raw.t != T_OBJ) return throw_err(vm, E_TYPE, "String.raw needs the pieces of a template");
    n = (u32)to_num(vm, get_prop(vm, raw, atom("length")));
    TEMP(vm, raw);
    TEMP(vm, v_str(r));                  /* what is made so far, kept from the sweeper */
    for (i = 0; i < n && !vm->throwing && r; i++) {
        Str *piece = to_str(vm, get_elem(vm, raw, v_num(i)));
        r = piece ? str_concat(vm->temps[vm->ntemps - 1].u.s, piece) : 0;
        if (r) vm->temps[vm->ntemps - 1] = v_str(r);
        if (r && i + 1 < n && (int)i + 1 < argc) {
            piece = to_str(vm, args[i + 1]);
            r = piece ? str_concat(vm->temps[vm->ntemps - 1].u.s, piece) : 0;
            if (r) vm->temps[vm->ntemps - 1] = v_str(r);
        }
    }
    UNTEMP(vm, 2);
    return r ? v_str(r) : v_undef();
}

NATIVE(string_fromcharcode)
{
    Str *s = str_new((u32)argc);
    int i;
    UNUSED;
    if (!s) { vm->out_of_room = 1; return v_undef(); }
    if (magic == 0) for (i = 0; i < argc; i++) s->c[i] = (u16)to_uint32(to_num(vm, args[i]));
    else {                                /* fromCodePoint                   */
        u32 n = 0;
        s = str_new((u32)argc * 2);
        if (!s) { vm->out_of_room = 1; return v_undef(); }
        for (i = 0; i < argc; i++) {
            u32 c = to_uint32(to_num(vm, args[i]));
            if (c > 0x10FFFF) return throw_err(vm, E_RANGE, "not a code point");
            if (c >= 0x10000) { c -= 0x10000; s->c[n++] = (u16)(0xD800 + (c >> 10)); s->c[n++] = (u16)(0xDC00 + (c & 0x3FF)); }
            else s->c[n++] = (u16)c;
        }
        s->len = n;
    }
    return v_str(s);
}

NATIVE(str_tostring)
{
    UNUSED;
    if (self.t == T_STR) return self;
    if (self.t == T_OBJ && self.u.o->cls == C_BOXED && IS_STR(self.u.o->u.boxed.v)) return self.u.o->u.boxed.v;
    return throw_err(vm, E_TYPE, "String.prototype.toString on something that is not a string");
}

NATIVE(str_charat)                       /* 0 charAt, 1 charCodeAt, 2 codePointAt, 3 at */
{
    Str *s = this_str(vm, self);
    f64 d;
    UNUSED;
    if (!s) return v_undef();
    d = m_trunc(to_num(vm, ARG(0)));
    if (m_isnan(d)) d = 0;
    if (magic == 3 && d < 0) d += s->len;
    if (d < 0 || d >= s->len) return magic == 0 ? v_str(S_empty) : magic == 3 ? v_undef() : v_num(m_nan());
    if (magic == 0 || magic == 3) return v_str(str_from16(&s->c[(u32)d], 1));
    if (magic == 2) {
        u32 i = (u32)d, c = s->c[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < s->len) return v_num(0x10000 + ((c - 0xD800) << 10) + (s->c[i + 1] - 0xDC00));
        return v_num(c);
    }
    return v_num(s->c[(u32)d]);
}

NATIVE(str_indexof)                      /* 0 indexOf, 1 lastIndexOf, 2 includes, 3 startsWith, 4 endsWith */
{
    Str *s = this_str(vm, self), *w;
    s32 i;
    UNUSED;
    if (!s) return v_undef();
    if (ARG(0).t == T_OBJ && ARG(0).u.o->cls == C_REGEXP && magic >= 2) return throw_err(vm, E_TYPE, "a regular expression here is not allowed");
    w = to_str(vm, ARG(0));
    switch (magic) {
    case 0: return v_num(str_index(s, w, (u32)clampi(vm, ARG(1), 0, (s32)s->len, 0)));
    case 1: {
        s32 from = clampi(vm, ARG(1), 0, (s32)s->len, (s32)s->len);
        for (i = from; i >= 0; i--) {
            u32 j;
            if ((u32)i + w->len > s->len) continue;
            for (j = 0; j < w->len && s->c[(u32)i + j] == w->c[j]; j++) { }
            if (j == w->len) return v_num(i);
        }
        return v_num(-1);
    }
    case 2: return v_bool(str_index(s, w, (u32)clampi(vm, ARG(1), 0, (s32)s->len, 0)) >= 0);
    case 3: {
        u32 at = (u32)clampi(vm, ARG(1), 0, (s32)s->len, 0), j;
        if (at + w->len > s->len) return v_bool(0);
        for (j = 0; j < w->len; j++) if (s->c[at + j] != w->c[j]) return v_bool(0);
        return v_bool(1);
    }
    default: {
        u32 end = (u32)clampi(vm, ARG(1), 0, (s32)s->len, (s32)s->len), j;
        if (w->len > end) return v_bool(0);
        for (j = 0; j < w->len; j++) if (s->c[end - w->len + j] != w->c[j]) return v_bool(0);
        return v_bool(1);
    }
    }
}

NATIVE(strp_slice)                         /* 0 slice, 1 substring, 2 substr */
{
    Str *s = this_str(vm, self);
    s32 len, a, b;
    UNUSED;
    if (!s) return v_undef();
    len = (s32)s->len;
    if (magic == 0) {
        f64 x = ARG(0).t == T_UNDEF ? 0 : m_trunc(to_num(vm, ARG(0))), y = ARG(1).t == T_UNDEF ? len : m_trunc(to_num(vm, ARG(1)));
        if (m_isnan(x)) x = 0;
        if (m_isnan(y)) y = 0;
        if (x < 0) x += len;
        if (y < 0) y += len;
        a = x < 0 ? 0 : x > len ? len : (s32)x;
        b = y < 0 ? 0 : y > len ? len : (s32)y;
        return v_str(a < b ? str_slice(s, (u32)a, (u32)b) : S_empty);
    }
    if (magic == 1) {
        a = clampi(vm, ARG(0), 0, len, 0);
        b = clampi(vm, ARG(1), 0, len, len);
        if (a > b) { s32 t = a; a = b; b = t; }
        return v_str(str_slice(s, (u32)a, (u32)b));
    }
    {
        f64 x = m_trunc(to_num(vm, ARG(0)));
        if (m_isnan(x)) x = 0;
        if (x < 0) { x += len; if (x < 0) x = 0; }
        if (x > len) x = len;
        a = (s32)x;
        b = clampi(vm, ARG(1), 0, len - a, len - a);
        return v_str(str_slice(s, (u32)a, (u32)(a + b)));
    }
}

static u16 upper1(u16 c)
{
    if (c >= 'a' && c <= 'z') return (u16)(c - 32);
    if ((c >= 0xE0 && c <= 0xFE && c != 0xF7)) return (u16)(c - 32);
    if (c >= 0x430 && c <= 0x44F) return (u16)(c - 32);       /* Cyrillic */
    if (c >= 0x450 && c <= 0x45F) return (u16)(c - 80);
    if (c >= 0x3B1 && c <= 0x3C9 && c != 0x3C2) return (u16)(c - 32);   /* Greek */
    if (c >= 0x100 && c <= 0x17F && (c & 1) && c != 0x131) return (u16)(c - 1);
    return c;
}
static u16 lower1(u16 c)
{
    if (c >= 'A' && c <= 'Z') return (u16)(c + 32);
    if ((c >= 0xC0 && c <= 0xDE && c != 0xD7)) return (u16)(c + 32);
    if (c >= 0x410 && c <= 0x42F) return (u16)(c + 32);
    if (c >= 0x400 && c <= 0x40F) return (u16)(c + 80);
    if (c >= 0x391 && c <= 0x3A9) return (u16)(c + 32);
    if (c >= 0x100 && c <= 0x17F && !(c & 1) && c != 0x130) return (u16)(c + 1);
    return c;
}

NATIVE(str_case)                          /* 0 upper, 1 lower */
{
    Str *s = this_str(vm, self), *r;
    u32 i;
    UNUSED;
    if (!s) return v_undef();
    r = str_new(s->len);
    if (!r) { vm->out_of_room = 1; return v_undef(); }
    for (i = 0; i < s->len; i++) r->c[i] = magic ? lower1(s->c[i]) : upper1(s->c[i]);
    return v_str(r);
}

static int is_space(u32 c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0x0B || c == 0x0C || c == 0xA0 ||
           c == 0xFEFF || c == 0x2028 || c == 0x2029 || (c >= 0x2000 && c <= 0x200A) || c == 0x3000;
}

NATIVE(str_trim)                          /* 0 both, 1 start, 2 end */
{
    Str *s = this_str(vm, self);
    u32 a = 0, b;
    UNUSED;
    if (!s) return v_undef();
    b = s->len;
    if (magic != 2) while (a < b && is_space(s->c[a])) a++;
    if (magic != 1) while (b > a && is_space(s->c[b - 1])) b--;
    return v_str(str_slice(s, a, b));
}

NATIVE(str_pad)                           /* 0 padStart, 1 padEnd */
{
    Str *s = this_str(vm, self), *fill, *r;
    s32 want;
    u32 i, add;
    UNUSED;
    if (!s) return v_undef();
    want = clampi(vm, ARG(0), 0, 0x100000, 0);
    fill = ARG(1).t == T_UNDEF ? atom(" ") : to_str(vm, ARG(1));
    if ((u32)want <= s->len || !fill->len) return v_str(s);
    add = (u32)want - s->len;
    r = str_new((u32)want);
    if (!r) { vm->out_of_room = 1; return v_undef(); }
    if (magic == 0) {
        for (i = 0; i < add; i++) r->c[i] = fill->c[i % fill->len];
        memcpy(r->c + add, s->c, s->len * 2);
    } else {
        memcpy(r->c, s->c, s->len * 2);
        for (i = 0; i < add; i++) r->c[s->len + i] = fill->c[i % fill->len];
    }
    return v_str(r);
}

NATIVE(str_repeat)
{
    Str *s = this_str(vm, self), *r;
    f64 n = to_num(vm, ARG(0));
    u32 i;
    UNUSED;
    if (!s) return v_undef();
    if (n < 0 || m_isinf(n)) return throw_err(vm, E_RANGE, "repeat count must be positive");
    if (n * s->len > 0x100000) return throw_err(vm, E_RANGE, "repeated string too long");
    r = str_new((u32)n * s->len);
    if (!r) { vm->out_of_room = 1; return v_undef(); }
    for (i = 0; i < (u32)n; i++) memcpy(r->c + i * s->len, s->c, s->len * 2);
    return v_str(r);
}

NATIVE(strp_concat)
{
    Str *s = this_str(vm, self);
    int i;
    UNUSED;
    if (!s) return v_undef();
    for (i = 0; i < argc && s; i++) {
        Str *t = to_str(vm, args[i]);
        TEMP(vm, v_str(s));
        s = str_concat(s, t);
        UNTEMP(vm, 1);
    }
    return s ? v_str(s) : v_undef();
}

NATIVE(str_localecompare)
{
    Str *s = this_str(vm, self), *t;
    UNUSED;
    if (!s) return v_undef();
    TEMP(vm, v_str(s));
    t = to_str(vm, ARG(0));
    UNTEMP(vm, 1);
    return v_num(str_cmp(s, t));
}

NATIVE(str_split)
{
    Str *s = this_str(vm, self), *sep;
    Obj *r;
    u32 limit = ARG(1).t == T_UNDEF ? 0xFFFFFFFFu : to_uint32(to_num(vm, ARG(1)));
    UNUSED;
    if (!s) return v_undef();
    r = arr_new(vm, 0);
    if (!r) return v_undef();
    if (!limit) return v_obj(r);
    TEMP(vm, v_obj(r));
    TEMP(vm, v_str(s));
    if (ARG(0).t == T_UNDEF) { arr_push(vm, r, v_str(s)); UNTEMP(vm, 2); return v_obj(r); }
    if (ARG(0).t == T_OBJ && ARG(0).u.o->cls == C_REGEXP) {
        u32 at = 0, from = 0;
        Obj *re = ARG(0).u.o;
        if (!s->len) {
            int found; u32 ms, me;
            re_exec(vm, re, s, 0, &found, &ms, &me);
            if (!found) arr_push(vm, r, v_str(s));
            UNTEMP(vm, 2);
            return v_obj(r);
        }
        while (from < s->len && r->u.arr.len < limit && !vm->throwing) {
            int found; u32 ms, me;
            Val m = re_exec(vm, re, s, from, &found, &ms, &me);
            if (!found) break;
            if (me == ms && ms >= s->len) break;
            if (me == at || (me == ms && ms == at)) { from = ms + 1; if (me == ms && ms == at && at < s->len) { if (ms >= s->len) break; } continue; }
            arr_push(vm, r, v_str(str_slice(s, at, ms)));
            if (m.t == T_OBJ) {               /* captured groups go in too  */
                u32 g, n = m.u.o->u.arr.len;
                for (g = 1; g < n && r->u.arr.len < limit; g++) arr_push(vm, r, arr_get(m.u.o, g));
            }
            at = from = me;
            if (me == ms) from++;
        }
        if (r->u.arr.len < limit) arr_push(vm, r, v_str(str_slice(s, at, s->len)));
        UNTEMP(vm, 2);
        return v_obj(r);
    }
    sep = to_str(vm, ARG(0));
    if (!sep->len) {
        u32 i;
        for (i = 0; i < s->len && r->u.arr.len < limit; i++) arr_push(vm, r, v_str(str_from16(&s->c[i], 1)));
    } else {
        u32 at = 0;
        int i;
        while (r->u.arr.len < limit && (i = str_index(s, sep, at)) >= 0) {
            arr_push(vm, r, v_str(str_slice(s, at, (u32)i)));
            at = (u32)i + sep->len;
        }
        if (r->u.arr.len < limit) arr_push(vm, r, v_str(str_slice(s, at, s->len)));
    }
    UNTEMP(vm, 2);
    return v_obj(r);
}

/* The replacement for one match: $& $1 $` $' $$, or a function's answer. */
static Str *replacement(Vm *vm, Val with, Str *s, u32 ms, u32 me, Val groups)
{
    if (is_callable(with)) {
        Val a[12];
        int n = 0;
        u32 g;
        a[n++] = v_str(str_slice(s, ms, me));
        if (groups.t == T_OBJ) for (g = 1; g < groups.u.o->u.arr.len && n < 10; g++) a[n++] = arr_get(groups.u.o, g);
        a[n++] = v_num(ms);
        a[n++] = v_str(s);
        return to_str(vm, vm_call(vm, with, v_undef(), a, n));
    }
    {
        Str *t = with.u.s, *r;
        u32 i, n = 0, cap = t->len * 2 + (me - ms) * 4 + s->len + 16;
        u16 *buf;
        if (cap > 0x100000) cap = 0x100000;
        r = str_new(cap);
        if (!r) return S_empty;
        buf = r->c;
        for (i = 0; i < t->len && n < cap; i++) {
            u16 c = t->c[i];
            const u16 *src = 0;
            u32 len = 0;
            if (c == '$' && i + 1 < t->len) {
                u16 d = t->c[i + 1];
                if (d == '$') { buf[n++] = '$'; i++; continue; }
                if (d == '&') { src = s->c + ms; len = me - ms; i++; }
                else if (d == '`') { src = s->c; len = ms; i++; }
                else if (d == '\'') { src = s->c + me; len = s->len - me; i++; }
                else if (d >= '0' && d <= '9' && groups.t == T_OBJ) {
                    u32 g = d - '0';
                    if (i + 2 < t->len && t->c[i + 2] >= '0' && t->c[i + 2] <= '9' && g * 10 + (t->c[i + 2] - '0') < groups.u.o->u.arr.len) { g = g * 10 + (t->c[i + 2] - '0'); i++; }
                    if (g > 0 && g < groups.u.o->u.arr.len) {
                        Val gv = arr_get(groups.u.o, g);
                        if (gv.t == T_STR) { src = gv.u.s->c; len = gv.u.s->len; }
                        i++;
                    } else { buf[n++] = c; continue; }
                } else { buf[n++] = c; continue; }
                while (len-- && n < cap) buf[n++] = *src++;
                continue;
            }
            buf[n++] = c;
        }
        r->len = n;
        return r;
    }
}

NATIVE(str_replace)                       /* 0 replace, 1 replaceAll */
{
    Str *s = this_str(vm, self), *out;
    Val pat = ARG(0), with = ARG(1);
    Obj *parts;
    UNUSED;
    if (!s) return v_undef();
    if (!is_callable(with)) with = v_str(to_str(vm, with));
    parts = arr_new(vm, 0);
    if (!parts) return v_undef();
    TEMP(vm, v_obj(parts)); TEMP(vm, v_str(s)); TEMP(vm, with);
    if (pat.t == T_OBJ && pat.u.o->cls == C_REGEXP) {
        Obj *re = pat.u.o;
        int global = (re->u.re.flags & 1) != 0;
        u32 at = 0, from = 0;
        if (magic && !global) { UNTEMP(vm, 3); return throw_err(vm, E_TYPE, "replaceAll needs a /g expression"); }
        while (from <= s->len && !vm->throwing) {
            int found; u32 ms, me;
            Val m = re_exec(vm, re, s, from, &found, &ms, &me);
            if (!found) break;
            TEMP(vm, m);
            arr_push(vm, parts, v_str(str_slice(s, at, ms)));
            arr_push(vm, parts, v_str(replacement(vm, with, s, ms, me, m)));
            UNTEMP(vm, 1);
            at = me;
            from = me == ms ? me + 1 : me;
            if (!global) break;
        }
        if (global) re->u.re.last = 0;
        arr_push(vm, parts, v_str(str_slice(s, at, s->len)));
    } else {
        Str *p = to_str(vm, pat);
        u32 at = 0;
        int i;
        TEMP(vm, v_str(p));
        while ((i = str_index(s, p, at)) >= 0 && (u32)i <= s->len && !vm->throwing) {
            arr_push(vm, parts, v_str(str_slice(s, at, (u32)i)));
            arr_push(vm, parts, v_str(replacement(vm, with, s, (u32)i, (u32)i + p->len, v_undef())));
            at = (u32)i + p->len;
            if (!magic) break;
            if (!p->len) { if (at < s->len) arr_push(vm, parts, v_str(str_from16(&s->c[at], 1))); at++; if (at > s->len) break; }
        }
        if (at <= s->len) arr_push(vm, parts, v_str(str_slice(s, at, s->len)));
        UNTEMP(vm, 1);
    }
    out = array_join(vm, parts, S_empty, 0);
    UNTEMP(vm, 3);
    return v_str(out);
}

NATIVE(str_match)                          /* 0 match, 1 search, 2 matchAll (as an array) */
{
    Str *s = this_str(vm, self);
    Obj *re;
    UNUSED;
    if (!s) return v_undef();
    if (ARG(0).t == T_OBJ && ARG(0).u.o->cls == C_REGEXP) re = ARG(0).u.o;
    else {
        Str *src = ARG(0).t == T_UNDEF ? S_empty : to_str(vm, ARG(0));
        re = re_new(vm, src, magic == 2 ? atom("g") : S_empty);
        if (!re) return v_undef();
    }
    TEMP(vm, v_obj(re)); TEMP(vm, v_str(s));
    if (magic == 1) {
        int found; u32 ms, me;
        re_exec(vm, re, s, 0, &found, &ms, &me);
        UNTEMP(vm, 2);
        return v_num(found ? (f64)ms : -1);
    }
    if (!(re->u.re.flags & 1)) {
        int found; u32 ms, me;
        Val m = re_exec(vm, re, s, 0, &found, &ms, &me);
        UNTEMP(vm, 2);
        return found ? m : v_null();
    }
    {
        Obj *r = arr_new(vm, 0);
        u32 from = 0;
        if (!r) { UNTEMP(vm, 2); return v_undef(); }
        TEMP(vm, v_obj(r));
        while (from <= s->len && !vm->throwing) {
            int found; u32 ms, me;
            Val m = re_exec(vm, re, s, from, &found, &ms, &me);
            if (!found) break;
            arr_push(vm, r, magic == 2 ? m : v_str(str_slice(s, ms, me)));
            from = me == ms ? me + 1 : me;
        }
        re->u.re.last = 0;
        UNTEMP(vm, 3);
        if (!r->u.arr.len && magic == 0) return v_null();
        return v_obj(r);
    }
}

NATIVE(str_normalize) { Str *s = this_str(vm, self); UNUSED; return s ? v_str(s) : v_undef(); }

/* ---- Date ---------------------------------------------------------------------------- */
static s64 days_from_civil(s64 y, u32 m, u32 d)
{
    s64 era;
    u32 yoe, doy, doe;
    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = (u32)(y - era * 400);
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (s64)doe - 719468;
}

static void civil(s64 z, s64 *y, u32 *m, u32 *d)
{
    s64 era;
    u32 doe, yoe, doy, mp;
    z += 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = (u32)(z - era * 146097);
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    *y = (s64)yoe + era * 400;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y += *m <= 2;
}

static f64 make_time(f64 y, f64 mo, f64 d, f64 h, f64 mi, f64 s, f64 ms)
{
    f64 ym = y + m_floor(mo / 12), mn = m_fmod(mo, 12);
    if (mn < 0) mn += 12;
    if (m_isnan(y) || m_isnan(mo) || m_isnan(d) || m_isnan(h) || m_isnan(mi) || m_isnan(s) || m_isnan(ms)) return m_nan();
    return ((f64)days_from_civil((s64)ym, (u32)mn + 1, 1) + d - 1) * 86400000.0 + h * 3600000.0 + mi * 60000.0 + s * 1000.0 + ms;
}

f64 date_now(Vm *vm) { return vm->now_base + (f64)host_ms(); }

/* "2024-05-01", "2024-05-01T12:30:00Z", "2024-05-01T12:30:00.000+02:00" */
static f64 parse_date(Str *s)
{
    u32 i = 0;
    f64 part[7] = { 0, 0, 1, 0, 0, 0, 0 };
    int k, neg = 0;
    if (i < s->len && (s->c[i] == '+' || s->c[i] == '-')) neg = s->c[i++] == '-';
    for (k = 0; k < 7 && i < s->len; k++) {
        f64 v = 0;
        u32 digits = 0, start = i;
        while (i < s->len && s->c[i] >= '0' && s->c[i] <= '9') { v = v * 10 + (s->c[i] - '0'); i++; digits++; }
        if (!digits) break;
        if (k == 6) { while (digits < 3) { v *= 10; digits++; } while (digits > 3) { v = m_floor(v / 10); digits--; } }
        part[k] = v;
        (void)start;
        if (i >= s->len) { k++; break; }
        if (k < 2 && s->c[i] == '-') { i++; continue; }
        if (k == 2 && (s->c[i] == 'T' || s->c[i] == ' ')) { i++; continue; }
        if (k >= 3 && k < 5 && s->c[i] == ':') { i++; continue; }
        if (k == 5 && s->c[i] == '.') { i++; continue; }
        k++;
        break;
    }
    if (k < 1) return m_nan();
    {
        f64 t = make_time(neg ? -part[0] : part[0], part[1] - 1, part[2], part[3], part[4], part[5], part[6]);
        if (i < s->len && (s->c[i] == '+' || s->c[i] == '-')) {
            int sg = s->c[i] == '-' ? -1 : 1;
            f64 hh = 0, mm = 0;
            i++;
            if (i + 1 < s->len) { hh = (s->c[i] - '0') * 10 + (s->c[i + 1] - '0'); i += 2; }
            if (i < s->len && s->c[i] == ':') i++;
            if (i + 1 < s->len) { mm = (s->c[i] - '0') * 10 + (s->c[i + 1] - '0'); }
            t -= sg * (hh * 3600000.0 + mm * 60000.0);
        }
        return t;
    }
}

NATIVE(date_ctor)
{
    Obj *d;
    f64 t;
    UNUSED;
    if (!vm->in_new) {
        Val now = v_num(date_now(vm));
        Obj *tmp = obj_new(vm, vm->date_proto, C_DATE);
        Val fn;
        if (!tmp) return v_undef();
        tmp->u.date.t = now.u.n;
        TEMP(vm, v_obj(tmp));
        fn = get_prop(vm, v_obj(tmp), S_toString);
        now = vm_call(vm, fn, v_obj(tmp), 0, 0);
        UNTEMP(vm, 1);
        return now;
    }
    if (argc == 0) t = date_now(vm);
    else if (argc == 1) {
        Val v = args[0];
        if (v.t == T_OBJ && v.u.o->cls == C_DATE) t = v.u.o->u.date.t;
        else {
            v = to_prim(vm, v, 0);
            t = v.t == T_STR ? parse_date(v.u.s) : to_num(vm, v);
        }
    } else {
        f64 p[7] = { 0, 0, 1, 0, 0, 0, 0 };
        int i;
        for (i = 0; i < argc && i < 7; i++) p[i] = to_num(vm, args[i]);
        if (p[0] >= 0 && p[0] <= 99) p[0] += 1900;
        t = make_time(p[0], p[1], p[2], p[3], p[4], p[5], p[6]);
    }
    d = obj_new(vm, vm->date_proto, C_DATE);
    if (!d) return v_undef();
    d->u.date.t = m_isnan(t) || t > 8.64e15 || t < -8.64e15 ? m_nan() : m_trunc(t);
    return v_obj(d);
}

NATIVE(date_now_n) { UNUSED; return v_num(m_floor(date_now(vm))); }
NATIVE(date_parse) { Str *s = to_str(vm, ARG(0)); UNUSED; return v_num(parse_date(s)); }
NATIVE(date_utc)
{
    f64 p[7] = { 0, 0, 1, 0, 0, 0, 0 };
    int i;
    UNUSED;
    for (i = 0; i < argc && i < 7; i++) p[i] = to_num(vm, args[i]);
    return v_num(make_time(p[0], p[1], p[2], p[3], p[4], p[5], p[6]));
}

static int this_date(Vm *vm, Val self, f64 *t)
{
    if (self.t == T_OBJ && self.u.o->cls == C_DATE) { *t = self.u.o->u.date.t; return 1; }
    throw_err(vm, E_TYPE, "a Date method on something that is not a date");
    return 0;
}

/* 0 getTime, 1 FullYear, 2 Month, 3 Date, 4 Day, 5 Hours, 6 Minutes,
 * 7 Seconds, 8 Milliseconds, 9 TimezoneOffset */
NATIVE(date_get)
{
    f64 t;
    s64 days, y;
    u32 m, d;
    f64 ms;
    UNUSED;
    if (!this_date(vm, self, &t)) return v_undef();
    if (magic == 9) return v_num(0);
    if (m_isnan(t)) return v_num(m_nan());
    if (magic == 0) return v_num(t);
    days = (s64)m_floor(t / 86400000.0);
    ms = t - (f64)days * 86400000.0;
    civil(days, &y, &m, &d);
    switch (magic) {
    case 1: return v_num((f64)y);
    case 2: return v_num(m - 1);
    case 3: return v_num(d);
    case 4: { s64 w = (days + 4) % 7; if (w < 0) w += 7; return v_num((f64)w); }
    case 5: return v_num(m_floor(ms / 3600000.0));
    case 6: return v_num(m_fmod(m_floor(ms / 60000.0), 60));
    case 7: return v_num(m_fmod(m_floor(ms / 1000.0), 60));
    default: return v_num(m_fmod(ms, 1000));
    }
}

/* setTime 0, setFullYear 1, setMonth 2, setDate 3, setHours 5, setMinutes 6, setSeconds 7, setMilliseconds 8 */
NATIVE(date_set)
{
    f64 t, p[7];
    s64 days, y;
    u32 m, d;
    f64 ms;
    int i, at;
    UNUSED;
    if (!this_date(vm, self, &t)) return v_undef();
    if (magic == 0) { t = to_num(vm, ARG(0)); self.u.o->u.date.t = t; return v_num(t); }
    if (m_isnan(t)) t = 0;
    days = (s64)m_floor(t / 86400000.0);
    ms = t - (f64)days * 86400000.0;
    civil(days, &y, &m, &d);
    p[0] = (f64)y; p[1] = m - 1; p[2] = d;
    p[3] = m_floor(ms / 3600000.0); p[4] = m_fmod(m_floor(ms / 60000.0), 60);
    p[5] = m_fmod(m_floor(ms / 1000.0), 60); p[6] = m_fmod(ms, 1000);
    at = magic == 1 ? 0 : magic == 2 ? 1 : magic == 3 ? 2 : (int)magic - 2;
    for (i = 0; i < argc && at + i < 7; i++) p[at + i] = to_num(vm, args[i]);
    t = make_time(p[0], p[1], p[2], p[3], p[4], p[5], p[6]);
    self.u.o->u.date.t = t;
    return v_num(t);
}

static const char *s_days[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *s_months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

static int put2(char *b, int n, u32 v) { b[n++] = (char)('0' + v / 10 % 10); b[n++] = (char)('0' + v % 10); return n; }

/* 0 toString, 1 toISOString/toJSON, 2 toDateString, 3 toTimeString, 4 toLocaleDateString,
 * 5 toLocaleTimeString, 6 toLocaleString, 7 toUTCString */
NATIVE(date_str)
{
    f64 t, ms;
    s64 days, y, w;
    u32 m, d, hh, mi, ss, mss;
    char b[64];
    int n = 0, k;
    UNUSED;
    if (!this_date(vm, self, &t)) return v_undef();
    if (m_isnan(t)) {
        if (magic == 1) return throw_err(vm, E_RANGE, "invalid date");
        return v_str(atom("Invalid Date"));
    }
    days = (s64)m_floor(t / 86400000.0);
    ms = t - (f64)days * 86400000.0;
    civil(days, &y, &m, &d);
    w = (days + 4) % 7; if (w < 0) w += 7;
    hh = (u32)(ms / 3600000.0); mi = (u32)(ms / 60000.0) % 60; ss = (u32)(ms / 1000.0) % 60; mss = (u32)m_fmod(ms, 1000);
    if (magic == 1) {
        u32 yy = (u32)(y < 0 ? -y : y);
        n = 0;
        if (y < 0 || y > 9999) { b[n++] = y < 0 ? '-' : '+'; b[n++] = '0'; b[n++] = '0'; }
        b[n++] = (char)('0' + yy / 1000 % 10); b[n++] = (char)('0' + yy / 100 % 10); n = put2(b, n, yy % 100);
        b[n++] = '-'; n = put2(b, n, m); b[n++] = '-'; n = put2(b, n, d);
        b[n++] = 'T'; n = put2(b, n, hh); b[n++] = ':'; n = put2(b, n, mi); b[n++] = ':'; n = put2(b, n, ss);
        b[n++] = '.'; b[n++] = (char)('0' + mss / 100); n = put2(b, n, mss % 100); b[n++] = 'Z';
    } else {
        if (magic == 0 || magic == 2 || magic == 7) {
            const char *dn = s_days[w], *mn = s_months[m - 1];
            for (k = 0; dn[k]; k++) b[n++] = dn[k];
            b[n++] = magic == 7 ? ',' : ' ';
            if (magic == 7) { b[n++] = ' '; n = put2(b, n, d); b[n++] = ' '; for (k = 0; mn[k]; k++) b[n++] = mn[k]; }
            else { for (k = 0; mn[k]; k++) b[n++] = mn[k]; b[n++] = ' '; n = put2(b, n, d); }
            b[n++] = ' ';
            { char t2[12]; int tk = 0; s64 yy = y < 0 ? -y : y; if (y < 0) b[n++] = '-'; do { t2[tk++] = (char)('0' + yy % 10); yy /= 10; } while (yy); while (tk) b[n++] = t2[--tk]; }
        } else if (magic == 4 || magic == 6) {
            { char t2[12]; int tk = 0; s64 yy = y; do { t2[tk++] = (char)('0' + yy % 10); yy /= 10; } while (yy > 0); while (tk) b[n++] = t2[--tk]; }
            b[n++] = '-'; n = put2(b, n, m); b[n++] = '-'; n = put2(b, n, d);
        }
        if (magic == 0 || magic == 3 || magic == 5 || magic == 6 || magic == 7) {
            if (n) b[n++] = ' ';
            n = put2(b, n, hh); b[n++] = ':'; n = put2(b, n, mi); b[n++] = ':'; n = put2(b, n, ss);
            if (magic == 0 || magic == 3) { const char *z = " GMT+0000"; for (k = 0; z[k]; k++) b[n++] = z[k]; }
            if (magic == 7) { const char *z = " GMT"; for (k = 0; z[k]; k++) b[n++] = z[k]; }
        }
    }
    b[n] = 0;
    return v_str(str_ascii(b));
}

NATIVE(date_valueof) { f64 t; UNUSED; if (!this_date(vm, self, &t)) return v_undef(); return v_num(t); }

/* ---- Promise --------------------------------------------------------------------------- */
static Str *P_p, *P_h, *P_c, *P_k, *P_done, *P_i, *P_list, *P_count;

static void enqueue(Vm *vm, Val fn, Val self, Val arg)
{
    if (!vm->microq) { vm->microq = arr_new(vm, 0); if (!vm->microq) return; }
    arr_push(vm, vm->microq, fn);
    arr_push(vm, vm->microq, self);
    arr_push(vm, vm->microq, arg);
}

static Obj *s_job;                        /* the one native that runs reactions */

static void settle(Vm *vm, Obj *p, int state, Val v)
{
    Vals *rs;
    u32 i;
    if (p->u.prom.state) return;
    p->u.prom.state = (u32)state;
    p->u.prom.value = v;
    rs = p->u.prom.reactions;
    p->u.prom.reactions = 0;
    if (!rs) return;
    for (i = 0; i < rs->n; i++) {
        Val r = rs->v[i];
        if (r.t != T_OBJ) continue;
        if ((int)to_num(vm, get_prop(vm, r, P_k)) + 1 == state) enqueue(vm, v_obj(s_job), r, v);
    }
}

static Obj *resolvers(Vm *vm, Obj *p, int which);

void promise_resolve_fn(Vm *vm, Obj *p, Val v, int rejected)
{
    if (p->u.prom.state) return;
    if (rejected) { settle(vm, p, 2, v); return; }
    if (v.t == T_OBJ && v.u.o == p) { Obj *e = make_err(vm, E_TYPE, str_ascii("a promise cannot resolve to itself")); settle(vm, p, 2, e ? v_obj(e) : v_undef()); return; }
    if (v.t == T_OBJ) {
        Val then = get_prop(vm, v, S_then);
        if (vm->throwing) { Val e = vm->exception; vm->throwing = 0; settle(vm, p, 2, e); return; }
        if (is_callable(then)) {
            /* a promise-like: let it decide, a moment later */
            Obj *ok = resolvers(vm, p, 0), *bad;
            Val a[2];
            if (!ok) return;
            TEMP(vm, v_obj(ok));
            bad = resolvers(vm, p, 1);
            if (!bad) { UNTEMP(vm, 1); return; }
            TEMP(vm, v_obj(bad));
            a[0] = v_obj(ok); a[1] = v_obj(bad);
            vm_call(vm, then, v, a, 2);
            if (vm->throwing) { Val e = vm->exception; vm->throwing = 0; settle(vm, p, 2, e); }
            UNTEMP(vm, 2);
            return;
        }
    }
    settle(vm, p, 1, v);
}

NATIVE(resolver_fn)                       /* magic 0 resolve, 1 reject */
{
    Obj *me = vm->callee;
    Val pv = get_prop(vm, v_obj(me), P_p), done = get_prop(vm, v_obj(me), P_done);
    UNUSED;
    if (pv.t != T_OBJ) return v_undef();
    if (done.t == T_OBJ) {               /* resolve and reject share one "done" */
        if (truthy(get_prop(vm, done, P_done))) return v_undef();
        set_prop(vm, done, P_done, v_bool(1));
    }
    promise_resolve_fn(vm, pv.u.o, ARG(0), (int)magic);
    return v_undef();
}

static Obj *s_last_done;
static Obj *resolvers(Vm *vm, Obj *p, int which)
{
    Obj *f = native_new(vm, which ? "reject" : "resolve", resolver_fn, 1, (u32)which);
    if (!f) return 0;
    TEMP(vm, v_obj(f));
    if (!which || !s_last_done) {
        s_last_done = obj_new(vm, vm->object_proto, C_OBJECT);
    }
    prop_put(vm, f, P_p, v_obj(p), P_HIDDEN);
    if (s_last_done) prop_put(vm, f, P_done, v_obj(s_last_done), P_HIDDEN);
    if (which) s_last_done = 0;
    UNTEMP(vm, 1);
    return f;
}

Obj *promise_new(Vm *vm)
{
    Obj *p = obj_new(vm, vm->promise_proto, C_PROMISE);
    if (p) p->u.prom.value = v_undef();
    return p;
}

NATIVE(promise_job)
{
    Val h = get_prop(vm, self, P_h), c = get_prop(vm, self, P_c);
    int k = (int)to_num(vm, get_prop(vm, self, P_k));
    UNUSED;
    if (is_callable(h)) {
        Val r = vm_call(vm, h, v_undef(), args, argc ? 1 : 0);
        if (c.t != T_OBJ) { vm->throwing = 0; return v_undef(); }
        if (vm->throwing) {
            Val e = vm->exception;
            if (vm->fatal) return v_undef();
            vm->throwing = 0;
            promise_resolve_fn(vm, c.u.o, e, 1);
        } else promise_resolve_fn(vm, c.u.o, r, 0);
    } else if (c.t == T_OBJ) promise_resolve_fn(vm, c.u.o, ARG(0), k);
    return v_undef();
}

Val promise_then(Vm *vm, Obj *p, Val ok, Val bad)
{
    Obj *child = promise_new(vm), *r[2];
    int i;
    if (!child) return v_undef();
    TEMP(vm, v_obj(child));
    for (i = 0; i < 2; i++) {
        r[i] = obj_new(vm, vm->object_proto, C_OBJECT);
        if (!r[i]) { UNTEMP(vm, (u32)i + 1); return v_undef(); }
        TEMP(vm, v_obj(r[i]));
        prop_put(vm, r[i], P_h, i ? bad : ok, 0);
        prop_put(vm, r[i], P_c, v_obj(child), 0);
        prop_put(vm, r[i], P_k, v_num(i), 0);
    }
    if (!p->u.prom.state) {
        Vals *old = p->u.prom.reactions, *nv;
        u32 n = old ? old->n : 0;
        nv = gc_alloc(sizeof(Vals) + (n + 2) * sizeof(Val), K_VALS);
        if (!nv) { vm->out_of_room = 1; UNTEMP(vm, 3); return v_undef(); }
        nv->n = n + 2;
        if (old) memcpy(nv->v, old->v, n * sizeof(Val));
        nv->v[n] = v_obj(r[0]); nv->v[n + 1] = v_obj(r[1]);
        p->u.prom.reactions = nv;
    } else enqueue(vm, v_obj(s_job), v_obj(r[p->u.prom.state - 1]), p->u.prom.value);
    UNTEMP(vm, 3);
    return v_obj(child);
}

NATIVE(promise_ctor)
{
    Obj *p, *ok, *bad;
    Val a[2];
    UNUSED;
    if (!is_callable(ARG(0))) return throw_err(vm, E_TYPE, "a Promise needs a function");
    p = promise_new(vm);
    if (!p) return v_undef();
    TEMP(vm, v_obj(p));
    ok = resolvers(vm, p, 0);
    if (!ok) { UNTEMP(vm, 1); return v_undef(); }
    TEMP(vm, v_obj(ok));
    bad = resolvers(vm, p, 1);
    if (!bad) { UNTEMP(vm, 2); return v_undef(); }
    TEMP(vm, v_obj(bad));
    a[0] = v_obj(ok); a[1] = v_obj(bad);
    vm_call(vm, ARG(0), v_undef(), a, 2);
    if (vm->throwing && !vm->fatal) { Val e = vm->exception; vm->throwing = 0; promise_resolve_fn(vm, p, e, 1); }
    UNTEMP(vm, 3);
    return v_obj(p);
}

static Obj *as_promise(Vm *vm, Val v)
{
    Obj *p;
    if (v.t == T_OBJ && v.u.o->cls == C_PROMISE) return v.u.o;
    p = promise_new(vm);
    if (!p) return 0;
    TEMP(vm, v_obj(p));
    promise_resolve_fn(vm, p, v, 0);
    UNTEMP(vm, 1);
    return p;
}

NATIVE(promise_resolve_n)                  /* 0 Promise.resolve, 1 Promise.reject */
{
    Obj *p;
    UNUSED;
    if (magic == 0) { p = as_promise(vm, ARG(0)); return p ? v_obj(p) : v_undef(); }
    p = promise_new(vm);
    if (p) settle(vm, p, 2, ARG(0));
    return p ? v_obj(p) : v_undef();
}

NATIVE(promise_then_n)                     /* 0 then, 1 catch, 2 finally */
{
    UNUSED;
    if (self.t != T_OBJ || self.u.o->cls != C_PROMISE) return throw_err(vm, E_TYPE, "then on something that is not a promise");
    if (magic == 0) return promise_then(vm, self.u.o, ARG(0), ARG(1));
    if (magic == 1) return promise_then(vm, self.u.o, v_undef(), ARG(0));
    return promise_then(vm, self.u.o, ARG(0), ARG(0));   /* finally: close enough */
}

/* Promise.all and friends: each item reports back into a shared list. */
NATIVE(all_item)                          /* magic: 0 kept, 1 broken (all), 2 settled kept, 3 settled broken */
{
    Obj *me = vm->callee;
    Val p = get_prop(vm, v_obj(me), P_p), list = get_prop(vm, v_obj(me), P_list), count = get_prop(vm, v_obj(me), P_count);
    u32 i = (u32)to_num(vm, get_prop(vm, v_obj(me), P_i));
    UNUSED;
    if (p.t != T_OBJ || list.t != T_OBJ || count.t != T_OBJ) return v_undef();
    if (magic == 1) { promise_resolve_fn(vm, p.u.o, ARG(0), 1); return v_undef(); }
    if (magic >= 2) {
        Obj *o = obj_new(vm, vm->object_proto, C_OBJECT);
        if (!o) return v_undef();
        TEMP(vm, v_obj(o));
        prop_put(vm, o, atom("status"), v_str(atom(magic == 2 ? "fulfilled" : "rejected")), 0);
        prop_put(vm, o, atom(magic == 2 ? "value" : "reason"), ARG(0), 0);
        arr_set(vm, list.u.o, i, v_obj(o));
        UNTEMP(vm, 1);
    } else arr_set(vm, list.u.o, i, ARG(0));
    {
        f64 left = to_num(vm, get_prop(vm, count, P_count)) - 1;
        set_prop(vm, count, P_count, v_num(left));
        if (left <= 0) promise_resolve_fn(vm, p.u.o, list, 0);
    }
    return v_undef();
}

NATIVE(promise_all)                       /* 0 all, 1 allSettled, 2 race */
{
    Obj *p = promise_new(vm), *list, *count;
    Val src = ARG(0);
    u32 i, n;
    UNUSED;
    if (!p) return v_undef();
    TEMP(vm, v_obj(p));
    list = arr_new(vm, 0);
    count = obj_new(vm, vm->object_proto, C_OBJECT);
    if (!list || !count) { UNTEMP(vm, 1); return v_undef(); }
    TEMP(vm, v_obj(list)); TEMP(vm, v_obj(count));
    n = src.t == T_OBJ && src.u.o->cls == C_ARRAY ? src.u.o->u.arr.len : 0;
    prop_put(vm, count, P_count, v_num(n), 0);
    arr_set_len(vm, list, n);
    if (!n && magic != 2) promise_resolve_fn(vm, p, v_obj(list), 0);
    for (i = 0; i < n && !vm->throwing; i++) {
        Obj *item = as_promise(vm, arr_get(src.u.o, i)), *ok, *bad;
        if (!item) break;
        TEMP(vm, v_obj(item));
        if (magic == 2) {
            ok = resolvers(vm, p, 0);
            TEMP(vm, v_obj(ok));
            bad = resolvers(vm, p, 1);
            TEMP(vm, v_obj(bad));
        } else {
            ok = native_new(vm, "", all_item, 1, magic ? 2 : 0);
            TEMP(vm, v_obj(ok));
            bad = native_new(vm, "", all_item, 1, magic ? 3 : 1);
            TEMP(vm, v_obj(bad));
            prop_put(vm, ok, P_p, v_obj(p), P_HIDDEN); prop_put(vm, bad, P_p, v_obj(p), P_HIDDEN);
            prop_put(vm, ok, P_list, v_obj(list), P_HIDDEN); prop_put(vm, bad, P_list, v_obj(list), P_HIDDEN);
            prop_put(vm, ok, P_count, v_obj(count), P_HIDDEN); prop_put(vm, bad, P_count, v_obj(count), P_HIDDEN);
            prop_put(vm, ok, P_i, v_num(i), P_HIDDEN); prop_put(vm, bad, P_i, v_num(i), P_HIDDEN);
        }
        promise_then(vm, item, v_obj(ok), v_obj(bad));
        UNTEMP(vm, 3);
    }
    UNTEMP(vm, 3);
    return v_obj(p);
}

/* ---- timers -------------------------------------------------------------------------- */
/* vm->timers: an array of [id, due, every, fn, [args]] lists. */
NATIVE(timer_set)                         /* 0 setTimeout, 1 setInterval, 2 requestAnimationFrame */
{
    Obj *t, *a;
    f64 ms = magic == 2 ? 50 : to_num(vm, ARG(1));
    int i;
    UNUSED;
    if (!is_callable(ARG(0))) {
        if (ARG(0).t == T_STR) return throw_err(vm, E_EVAL, "a timer with text instead of a function is not allowed here");
        return throw_err(vm, E_TYPE, "a timer needs a function");
    }
    if (m_isnan(ms) || ms < 0) ms = 0;
    if (magic == 1 && ms < 100) ms = 100;     /* no faster than ten a second */
    if (!vm->timers) { vm->timers = arr_new(vm, 0); if (!vm->timers) return v_undef(); }
    if (vm->timers->u.arr.len >= 256) return throw_err(vm, E_RANGE, "too many timers");
    t = arr_new(vm, 0);
    if (!t) return v_undef();
    TEMP(vm, v_obj(t));
    a = arr_new(vm, 0);
    if (!a) { UNTEMP(vm, 1); return v_undef(); }
    if (magic == 2) arr_push(vm, a, v_num(host_ms()));
    else for (i = 2; i < argc; i++) arr_push(vm, a, args[i]);
    vm->next_timer++;
    arr_push(vm, t, v_num(vm->next_timer));
    arr_push(vm, t, v_num((f64)host_ms() + ms));
    arr_push(vm, t, v_num(magic == 1 ? ms : 0));
    arr_push(vm, t, ARG(0));
    arr_push(vm, t, v_obj(a));
    arr_push(vm, vm->timers, v_obj(t));
    UNTEMP(vm, 1);
    return v_num(vm->next_timer);
}

NATIVE(timer_clear)
{
    f64 id = to_num(vm, ARG(0));
    u32 i;
    UNUSED;
    if (!vm->timers) return v_undef();
    for (i = 0; i < vm->timers->u.arr.len; i++) {
        Val t = arr_get(vm->timers, i);
        if (t.t == T_OBJ && arr_get(t.u.o, 0).u.n == id) {
            u32 n = vm->timers->u.arr.len;
            memmove(&vm->timers->u.arr.items->v[i], &vm->timers->u.arr.items->v[i + 1], (n - i - 1) * sizeof(Val));
            vm->timers->u.arr.len--;
            break;
        }
    }
    return v_undef();
}

NATIVE(queue_microtask)
{
    UNUSED;
    if (!is_callable(ARG(0))) return throw_err(vm, E_TYPE, "queueMicrotask needs a function");
    enqueue(vm, ARG(0), v_undef(), v_undef());
    return v_undef();
}

/* The earliest timer, in host_ms() time; 0 if none. */
u32 timers_next(Vm *vm)
{
    u32 i, best = 0;
    int any = 0;
    if (!vm->timers) return 0;
    for (i = 0; i < vm->timers->u.arr.len; i++) {
        Val t = arr_get(vm->timers, i);
        u32 due = (u32)arr_get(t.u.o, 1).u.n;
        if (!any || (s32)(due - best) < 0) best = due, any = 1;
    }
    return any ? (best ? best : 1) : 0;
}

/* Run what is due; each is its own little task, with microtasks after. */
int timers_run(Vm *vm, u32 slice_ms)
{
    u32 i, ran = 0, now = host_ms();
    if (!vm->timers) return 0;
    for (i = 0; i < vm->timers->u.arr.len && !vm->fatal; i++) {
        Val t = arr_get(vm->timers, i);
        u32 due = (u32)arr_get(t.u.o, 1).u.n;
        f64 every = arr_get(t.u.o, 2).u.n;
        Val fn, a;
        Obj *args;
        if ((s32)(now - due) < 0) continue;
        fn = arr_get(t.u.o, 3);
        a = arr_get(t.u.o, 4);
        args = a.u.o;
        TEMP(vm, t);
        if (every > 0) arr_set(vm, t.u.o, 1, v_num((f64)now + every));
        else {
            u32 n = vm->timers->u.arr.len;
            memmove(&vm->timers->u.arr.items->v[i], &vm->timers->u.arr.items->v[i + 1], (n - i - 1) * sizeof(Val));
            vm->timers->u.arr.len--;
            i--;
        }
        vm->deadline = host_ms() - vm->door_ms + slice_ms;
        vm_call(vm, fn, v_undef(), args->u.arr.items ? args->u.arr.items->v : 0, (int)args->u.arr.len);
        if (vm->throwing && !vm->fatal) vm->throwing = 0;
        vm_run_microtasks(vm);
        UNTEMP(vm, 1);
        ran++;
    }
    return (int)ran;
}

void lib2_init(Vm *vm)
{
    Obj *g = vm->global, *o, *c;
    static const char *gets[] = { "getTime", "getFullYear", "getMonth", "getDate", "getDay", "getHours",
        "getMinutes", "getSeconds", "getMilliseconds", "getTimezoneOffset", 0 };
    static const char *utcs[] = { "getTime", "getUTCFullYear", "getUTCMonth", "getUTCDate", "getUTCDay", "getUTCHours",
        "getUTCMinutes", "getUTCSeconds", "getUTCMilliseconds", 0 };
    static const char *sets[] = { "setTime", "setFullYear", "setMonth", "setDate", "", "setHours", "setMinutes", "setSeconds", "setMilliseconds", 0 };
    static const char *strs[] = { "toString", "toISOString", "toDateString", "toTimeString", "toLocaleDateString",
        "toLocaleTimeString", "toLocaleString", "toUTCString", 0 };
    int i;

    P_p = atom("%p"); P_h = atom("%h"); P_c = atom("%c"); P_k = atom("%k"); P_done = atom("%done");
    P_i = atom("%i"); P_list = atom("%list"); P_count = atom("%count");

    /* String */
    o = vm->string_proto;
    c = native_new(vm, "String", string_ctor, 1, 0);
    c->flags |= OF_CTOR;
    prop_put(vm, c, S_prototype, v_obj(o), P_HIDDEN);
    prop_put(vm, o, S_constructor, v_obj(c), P_HIDDEN);
    def_val(vm, g, "String", v_obj(c));
    def_native(vm, c, "fromCharCode", string_fromcharcode, 1, 0);
    def_native(vm, c, "raw", string_raw, 1, 0);
    def_native(vm, c, "fromCodePoint", string_fromcharcode, 1, 1);
    def_native(vm, o, "toString", str_tostring, 0, 0);
    def_native(vm, o, "valueOf", str_tostring, 0, 0);
    def_native(vm, o, "charAt", str_charat, 1, 0);
    def_native(vm, o, "charCodeAt", str_charat, 1, 1);
    def_native(vm, o, "codePointAt", str_charat, 1, 2);
    def_native(vm, o, "at", str_charat, 1, 3);
    def_native(vm, o, "indexOf", str_indexof, 1, 0);
    def_native(vm, o, "lastIndexOf", str_indexof, 1, 1);
    def_native(vm, o, "includes", str_indexof, 1, 2);
    def_native(vm, o, "startsWith", str_indexof, 1, 3);
    def_native(vm, o, "endsWith", str_indexof, 1, 4);
    def_native(vm, o, "slice", strp_slice, 2, 0);
    def_native(vm, o, "substring", strp_slice, 2, 1);
    def_native(vm, o, "substr", strp_slice, 2, 2);
    def_native(vm, o, "toUpperCase", str_case, 0, 0);
    def_native(vm, o, "toLowerCase", str_case, 0, 1);
    def_native(vm, o, "toLocaleUpperCase", str_case, 0, 0);
    def_native(vm, o, "toLocaleLowerCase", str_case, 0, 1);
    def_native(vm, o, "trim", str_trim, 0, 0);
    def_native(vm, o, "trimStart", str_trim, 0, 1);
    def_native(vm, o, "trimLeft", str_trim, 0, 1);
    def_native(vm, o, "trimEnd", str_trim, 0, 2);
    def_native(vm, o, "trimRight", str_trim, 0, 2);
    def_native(vm, o, "padStart", str_pad, 2, 0);
    def_native(vm, o, "padEnd", str_pad, 2, 1);
    def_native(vm, o, "repeat", str_repeat, 1, 0);
    def_native(vm, o, "concat", strp_concat, 1, 0);
    def_native(vm, o, "localeCompare", str_localecompare, 1, 0);
    def_native(vm, o, "split", str_split, 2, 0);
    def_native(vm, o, "replace", str_replace, 2, 0);
    def_native(vm, o, "replaceAll", str_replace, 2, 1);
    def_native(vm, o, "match", str_match, 1, 0);
    def_native(vm, o, "search", str_match, 1, 1);
    def_native(vm, o, "matchAll", str_match, 1, 2);
    def_native(vm, o, "normalize", str_normalize, 0, 0);

    /* Date */
    o = vm->date_proto;
    c = native_new(vm, "Date", date_ctor, 7, 0);
    c->flags |= OF_CTOR;
    prop_put(vm, c, S_prototype, v_obj(o), P_HIDDEN);
    prop_put(vm, o, S_constructor, v_obj(c), P_HIDDEN);
    def_val(vm, g, "Date", v_obj(c));
    def_native(vm, c, "now", date_now_n, 0, 0);
    def_native(vm, c, "parse", date_parse, 1, 0);
    def_native(vm, c, "UTC", date_utc, 7, 0);
    for (i = 0; gets[i]; i++) def_native(vm, o, gets[i], date_get, 0, (u32)i);
    for (i = 1; utcs[i]; i++) def_native(vm, o, utcs[i], date_get, 0, (u32)i);
    for (i = 0; sets[i]; i++) if (sets[i][0]) def_native(vm, o, sets[i], date_set, 1, (u32)i);
    for (i = 0; strs[i]; i++) def_native(vm, o, strs[i], date_str, 0, (u32)i);
    def_native(vm, o, "toJSON", date_str, 0, 1);
    def_native(vm, o, "valueOf", date_valueof, 0, 0);

    /* Promise */
    o = vm->promise_proto;
    c = native_new(vm, "Promise", promise_ctor, 1, 0);
    c->flags |= OF_CTOR;
    prop_put(vm, c, S_prototype, v_obj(o), P_HIDDEN);
    prop_put(vm, o, S_constructor, v_obj(c), P_HIDDEN);
    def_val(vm, g, "Promise", v_obj(c));
    def_native(vm, c, "resolve", promise_resolve_n, 1, 0);
    def_native(vm, c, "reject", promise_resolve_n, 1, 1);
    def_native(vm, c, "all", promise_all, 1, 0);
    def_native(vm, c, "allSettled", promise_all, 1, 1);
    def_native(vm, c, "race", promise_all, 1, 2);
    def_native(vm, o, "then", promise_then_n, 2, 0);
    def_native(vm, o, "catch", promise_then_n, 1, 1);
    def_native(vm, o, "finally", promise_then_n, 1, 2);
    s_job = native_new(vm, "job", promise_job, 1, 0);
    def_val(vm, vm->promise_proto, "%job", v_obj(s_job));   /* kept reachable */

    /* timers */
    def_native(vm, g, "setTimeout", timer_set, 2, 0);
    def_native(vm, g, "setInterval", timer_set, 2, 1);
    def_native(vm, g, "requestAnimationFrame", timer_set, 1, 2);
    def_native(vm, g, "clearTimeout", timer_clear, 1, 0);
    def_native(vm, g, "clearInterval", timer_clear, 1, 0);
    def_native(vm, g, "cancelAnimationFrame", timer_clear, 1, 0);
    def_native(vm, g, "queueMicrotask", queue_microtask, 1, 0);
    math_seed(host_ms(), (u32)vm->now_base);
}
