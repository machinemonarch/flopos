/* typed.c -- ArrayBuffer, the typed arrays (Int8Array ... Float64Array),
 * DataView, and TextEncoder / TextDecoder.
 *
 * An ArrayBuffer (C_ABUF) owns its bytes: a K_BYTES block that never moves
 * (the room's collector does not move anything).  A typed array or a
 * DataView (C_TARRAY; pad: the kind, TA_VIEW for a DataView) points into
 * one: u.ta.buf the buffer, u.ta.data its first byte, u.ta.len its length
 * in elements (bytes for a DataView).  Elements are read and written in
 * obj.c's element paths (ta_get / ta_put).
 *
 * The many methods that only go through the elements (forEach, map, join,
 * sort ...) are the engine's Array ones, run on a plain copy and, for
 * those that change it or make a new one, written back as this kind.
 * Nothing of BigInt64Array: the room has no BigInt.
 */
#include "js.h"

#define ARG(i) ((i) < argc ? args[i] : v_undef())
#define NATIVE(name) static Val name(Vm *vm, Val self, Val *args, int argc, u32 magic)
#define UNUSED (void)self; (void)args; (void)argc; (void)magic
#define MAX_BYTES (1536u << 10)          /* a buffer at most: the scripts have 2 MB in all */

static const char *const NAMES[TA_VIEW] = { "Int8Array", "Uint8Array", "Uint8ClampedArray", "Int16Array", "Uint16Array",
                                           "Int32Array", "Uint32Array", "Float32Array", "Float64Array" };
static const u8 SIZE[TA_VIEW + 1] = { 1, 1, 1, 2, 2, 4, 4, 4, 8, 1 };
static Obj *s_proto[TA_VIEW + 1], *s_ctor[TA_VIEW], *s_buf_proto, *s_cmp;

static void fmt_name(char *o, const char *a, const char *b)
{
    while (*a) *o++ = *a++;
    while (*b) *o++ = *b++;
    *o = 0;
}

static u8 *bytes_new(Vm *vm, u32 n)
{
    GcHdr *h = gc_alloc(sizeof(GcHdr) + (n ? n : 1), K_BYTES);
    u8 *p;
    if (!h) { vm->out_of_room = 1; return 0; }
    p = (u8 *)(h + 1);
    memset(p, 0, n);
    return p;
}

static Obj *abuf_new(Vm *vm, u32 n)
{
    Obj *o;
    u8 *d;
    if (n > MAX_BYTES) { throw_err(vm, E_RANGE, "an ArrayBuffer of %u bytes is more than the room holds", n); return 0; }
    o = obj_new(vm, s_buf_proto, C_ABUF);
    if (!o) return 0;
    TEMP(vm, v_obj(o));
    d = bytes_new(vm, n);
    UNTEMP(vm, 1);
    if (!d) return 0;
    o->u.ta.buf = 0; o->u.ta.data = d; o->u.ta.len = n;
    return o;
}

static Obj *view_new(Vm *vm, int kind, Obj *buf, u32 off, u32 len)
{
    Obj *o = obj_new(vm, s_proto[kind], C_TARRAY);
    if (!o) return 0;
    o->pad = (u8)kind;
    o->u.ta.buf = buf; o->u.ta.data = buf->u.ta.data + off; o->u.ta.len = len;
    return o;
}

static Obj *ta_make(Vm *vm, int kind, u32 len)
{
    Obj *b, *o;
    if ((u64)len * SIZE[kind] > MAX_BYTES) { throw_err(vm, E_RANGE, "a %s of %u is more than the room holds", NAMES[kind], len); return 0; }
    b = abuf_new(vm, len * SIZE[kind]);
    if (!b) return 0;
    TEMP(vm, v_obj(b));
    o = view_new(vm, kind, b, 0, len);
    UNTEMP(vm, 1);
    return o;
}

/* A K_BYTES block's bytes (data just past its GcHdr) as a Uint8Array that
 * owns them: no copy (dom.c's long fetches). */
Obj *ta_adopt(Vm *vm, u8 *data, u32 len)
{
    Obj *b = obj_new(vm, s_buf_proto, C_ABUF), *v;
    if (!b) return 0;
    b->u.ta.buf = 0; b->u.ta.data = data; b->u.ta.len = len;
    TEMP(vm, v_obj(b));
    v = view_new(vm, 1, b, 0, len);
    UNTEMP(vm, 1);
    return v;
}

/* Bytes as a new Uint8Array (dom.c: what the door brings). */
Obj *ta_bytes(Vm *vm, const u8 *p, u32 n)
{
    Obj *o = ta_make(vm, 1, n);
    if (o && n) memcpy(o->u.ta.data, p, n);
    return o;
}

/* ---- elements ------------------------------------------------------------------------ */
Val ta_get(Obj *o, u32 i)
{
    u8 *p = o->u.ta.data + i * SIZE[o->pad];
    switch (o->pad) {
    case 0: return v_num((signed char)p[0]);
    case 1: case 2: return v_num(p[0]);
    case 3: { short v; memcpy(&v, p, 2); return v_num(v); }
    case 4: { u16 v; memcpy(&v, p, 2); return v_num(v); }
    case 5: { s32 v; memcpy(&v, p, 4); return v_num(v); }
    case 6: { u32 v; memcpy(&v, p, 4); return v_num(v); }
    case 7: { float v; memcpy(&v, p, 4); return v_num(v); }
    default: { f64 v; memcpy(&v, p, 8); return v_num(v); }
    }
}

static void put_num(u8 *p, int kind, f64 d)
{
    switch (kind) {
    case 0: case 1: p[0] = (u8)to_uint32(d); break;
    case 2: p[0] = d != d || d <= 0 ? 0 : d >= 255 ? 255 : (u8)(d - m_floor(d) == 0.5 ? ((u32)m_floor(d) + ((u32)m_floor(d) & 1)) : m_round(d)); break;
    case 3: case 4: { u16 v = (u16)to_uint32(d); memcpy(p, &v, 2); break; }
    case 5: case 6: { u32 v = to_uint32(d); memcpy(p, &v, 4); break; }
    case 7: { float v = (float)d; memcpy(p, &v, 4); break; }
    default: memcpy(p, &d, 8);
    }
}

int ta_put(Vm *vm, Obj *o, u32 i, Val v)
{
    f64 d = to_num(vm, v);
    if (vm->throwing) return 0;
    if (i < o->u.ta.len) put_num(o->u.ta.data + i * SIZE[o->pad], o->pad, d);
    return 1;                            /* past the end: nothing, as the standard says */
}

static Obj *this_ta(Vm *vm, Val self)
{
    if (self.t != T_OBJ || self.u.o->cls != C_TARRAY || self.u.o->pad == TA_VIEW) { throw_err(vm, E_TYPE, "not a typed array"); return 0; }
    return self.u.o;
}

static u32 rel(Vm *vm, Val v, u32 len, u32 dflt)       /* a relative index, clamped */
{
    f64 d;
    if (v.t == T_UNDEF) return dflt;
    d = m_trunc(to_num(vm, v));
    if (d != d) d = 0;
    if (d < 0) d = d + len < 0 ? 0 : d + len;
    return d > len ? len : (u32)d;
}

/* ---- constructors ------------------------------------------------------------------- */
NATIVE(abuf_ctor)
{
    f64 d = to_num(vm, ARG(0));
    Obj *b;
    UNUSED;
    if (!vm->in_new) return throw_err(vm, E_TYPE, "ArrayBuffer needs new");
    if (d != d) d = 0;
    if (d < 0 || d > 4294967295.0) return throw_err(vm, E_RANGE, "a bad length for an ArrayBuffer");
    b = abuf_new(vm, (u32)d);
    return b ? v_obj(b) : v_undef();
}

static Obj *ta_from_list(Vm *vm, int kind, Val src)       /* an array-like or iterable */
{
    Obj *list = 0, *o;
    u32 i, n;
    if (src.t == T_OBJ && src.u.o->cls == C_ARRAY) list = src.u.o;
    else if (src.t == T_OBJ && !is_callable(get_prop(vm, src, atom("@@iterator")))) {   /* array-like */
        f64 d = to_num(vm, get_prop(vm, src, S_length));
        if (vm->throwing) return 0;
        if (d != d || d < 0) d = 0;
        if (d * SIZE[kind] > MAX_BYTES) { throw_err(vm, E_RANGE, "a %s that long is more than the room holds", NAMES[kind]); return 0; }
        n = (u32)d;
        o = ta_make(vm, kind, n);
        if (!o) return 0;
        TEMP(vm, v_obj(o));
        for (i = 0; i < n && !vm->throwing; i++) ta_put(vm, o, i, get_elem(vm, src, v_num(i)));
        UNTEMP(vm, 1);
        return o;
    } else {
        Obj *it = vm_iter_new(vm, src);
        Val x;
        if (vm->throwing) return 0;
        if (it) {
            TEMP(vm, v_obj(it));
            list = arr_new(vm, 0);
            if (!list) { UNTEMP(vm, 1); return 0; }
            TEMP(vm, v_obj(list));
            while (vm_iter_next(vm, it, &x) > 0) arr_push(vm, list, x);
            UNTEMP(vm, 2);
            if (vm->throwing) return 0;
        } else if (src.t == T_OBJ) {
            Val l = get_prop(vm, src, S_length);
            n = to_uint32(to_num(vm, l));
            o = ta_make(vm, kind, n);
            if (!o) return 0;
            TEMP(vm, v_obj(o));
            for (i = 0; i < n && !vm->throwing; i++) ta_put(vm, o, i, get_elem(vm, src, v_num(i)));
            UNTEMP(vm, 1);
            return o;
        } else return ta_make(vm, kind, 0);
    }
    TEMP(vm, v_obj(list));
    n = list->u.arr.len;
    o = ta_make(vm, kind, n);
    if (o) {
        TEMP(vm, v_obj(o));
        for (i = 0; i < n && !vm->throwing; i++) ta_put(vm, o, i, arr_get(list, i));
        UNTEMP(vm, 1);
    }
    UNTEMP(vm, 1);
    return o;
}

NATIVE(ta_ctor)                          /* magic: the kind */
{
    int k = (int)magic;
    Val a = ARG(0);
    Obj *o;
    UNUSED;
    if (!vm->in_new) return throw_err(vm, E_TYPE, "%s needs new", NAMES[k]);
    if (a.t != T_OBJ) {
        f64 d = a.t == T_UNDEF ? 0 : to_num(vm, a);
        if (d != d) d = 0;
        if (d < 0 || d != m_trunc(d)) return throw_err(vm, E_RANGE, "a bad length for a %s", NAMES[k]);
        o = ta_make(vm, k, (u32)d);
        return o ? v_obj(o) : v_undef();
    }
    if (a.u.o->cls == C_ABUF) {
        Obj *b = a.u.o;
        u32 off = ARG(1).t == T_UNDEF ? 0 : to_uint32(to_num(vm, ARG(1))), len;
        if (off % SIZE[k] || off > b->u.ta.len) return throw_err(vm, E_RANGE, "a bad offset for a %s", NAMES[k]);
        if (ARG(2).t == T_UNDEF) {
            if ((b->u.ta.len - off) % SIZE[k]) return throw_err(vm, E_RANGE, "the buffer is not a whole number of elements");
            len = (b->u.ta.len - off) / SIZE[k];
        } else len = to_uint32(to_num(vm, ARG(2)));
        if ((u64)off + (u64)len * SIZE[k] > b->u.ta.len) return throw_err(vm, E_RANGE, "past the end of the buffer");
        o = view_new(vm, k, b, off, len);
        return o ? v_obj(o) : v_undef();
    }
    if (a.u.o->cls == C_TARRAY && a.u.o->pad != TA_VIEW) {
        u32 i, n = a.u.o->u.ta.len;
        o = ta_make(vm, k, n);
        if (!o) return v_undef();
        for (i = 0; i < n; i++) put_num(o->u.ta.data + i * SIZE[k], k, ta_get(a.u.o, i).u.n);
        return v_obj(o);
    }
    o = ta_from_list(vm, k, a);
    return o ? v_obj(o) : v_undef();
}

NATIVE(ta_from_of)                       /* magic: kind << 1 | of */
{
    int k = (int)(magic >> 1);
    Obj *o;
    UNUSED;
    if (magic & 1) {                     /* of(...items) */
        int i;
        o = ta_make(vm, k, (u32)argc);
        if (!o) return v_undef();
        for (i = 0; i < argc; i++) ta_put(vm, o, (u32)i, args[i]);
        return v_obj(o);
    }
    o = ta_from_list(vm, k, ARG(0));
    if (o && is_callable(ARG(1))) {
        u32 i;
        TEMP(vm, v_obj(o));
        for (i = 0; i < o->u.ta.len && !vm->throwing; i++) {
            Val in[2];
            in[0] = ta_get(o, i); in[1] = v_num(i);
            ta_put(vm, o, i, vm_call(vm, ARG(1), ARG(2), in, 2));
        }
        UNTEMP(vm, 1);
    }
    return o ? v_obj(o) : v_undef();
}

/* ---- the buffer --------------------------------------------------------------------- */
NATIVE(abuf_len)
{
    UNUSED;
    if (self.t != T_OBJ || self.u.o->cls != C_ABUF) return throw_err(vm, E_TYPE, "not an ArrayBuffer");
    return v_num(self.u.o->u.ta.len);
}

NATIVE(abuf_slice)
{
    Obj *b, *n;
    u32 from, to;
    UNUSED;
    if (self.t != T_OBJ || self.u.o->cls != C_ABUF) return throw_err(vm, E_TYPE, "not an ArrayBuffer");
    b = self.u.o;
    from = rel(vm, ARG(0), b->u.ta.len, 0);
    to = rel(vm, ARG(1), b->u.ta.len, b->u.ta.len);
    n = abuf_new(vm, to > from ? to - from : 0);
    if (n && to > from) memcpy(n->u.ta.data, b->u.ta.data + from, to - from);
    return n ? v_obj(n) : v_undef();
}

NATIVE(abuf_isview)
{
    UNUSED;
    return v_bool(ARG(0).t == T_OBJ && ARG(0).u.o->cls == C_TARRAY);
}

/* ---- typed arrays' own ---------------------------------------------------------------- */
NATIVE(ta_info)                          /* magic: 0 length, 1 byteLength, 2 byteOffset, 3 buffer */
{
    Obj *o;
    UNUSED;
    if (self.t != T_OBJ || self.u.o->cls != C_TARRAY) return throw_err(vm, E_TYPE, "not a typed array");
    o = self.u.o;
    switch (magic) {
    case 0: return v_num(o->u.ta.len);
    case 1: return v_num(o->u.ta.len * SIZE[o->pad]);
    case 2: return v_num((f64)(o->u.ta.data - o->u.ta.buf->u.ta.data));
    default: return v_obj(o->u.ta.buf);
    }
}

NATIVE(ta_set)                           /* set(source, offset) */
{
    Obj *o = this_ta(vm, self);
    Val src = ARG(0);
    u32 off, i, n;
    UNUSED;
    if (!o) return v_undef();
    off = ARG(1).t == T_UNDEF ? 0 : to_uint32(to_num(vm, ARG(1)));
    if (src.t == T_OBJ && src.u.o->cls == C_TARRAY && src.u.o->pad != TA_VIEW) {
        Obj *s = src.u.o;
        n = s->u.ta.len;
        if ((u64)off + n > o->u.ta.len) return throw_err(vm, E_RANGE, "set: past the end");
        if (s->pad == o->pad) memmove(o->u.ta.data + off * SIZE[o->pad], s->u.ta.data, n * SIZE[o->pad]);
        else {
            f64 tmp[64];                 /* through a small copy: the two may overlap */
            u32 at = 0;
            while (at < n) {
                u32 k, m = n - at < 64 ? n - at : 64;
                for (k = 0; k < m; k++) tmp[k] = ta_get(s, at + k).u.n;
                for (k = 0; k < m; k++) put_num(o->u.ta.data + (off + at + k) * SIZE[o->pad], o->pad, tmp[k]);
                at += m;
            }
        }
        return v_undef();
    }
    if (src.t != T_OBJ) return throw_err(vm, E_TYPE, "set: not a list");
    n = to_uint32(to_num(vm, get_prop(vm, src, S_length)));
    if ((u64)off + n > o->u.ta.len) return throw_err(vm, E_RANGE, "set: past the end");
    for (i = 0; i < n && !vm->throwing; i++) ta_put(vm, o, off + i, get_elem(vm, src, v_num(i)));
    return v_undef();
}

NATIVE(ta_sub)                           /* magic: 0 subarray (shares), 1 slice (copies) */
{
    Obj *o = this_ta(vm, self), *n;
    u32 from, to, len;
    UNUSED;
    if (!o) return v_undef();
    from = rel(vm, ARG(0), o->u.ta.len, 0);
    to = rel(vm, ARG(1), o->u.ta.len, o->u.ta.len);
    len = to > from ? to - from : 0;
    if (!magic) return (n = view_new(vm, o->pad, o->u.ta.buf, (u32)(o->u.ta.data - o->u.ta.buf->u.ta.data) + from * SIZE[o->pad], len)) ? v_obj(n) : v_undef();
    n = ta_make(vm, o->pad, len);
    if (n && len) memcpy(n->u.ta.data, o->u.ta.data + from * SIZE[o->pad], len * SIZE[o->pad]);
    return n ? v_obj(n) : v_undef();
}

NATIVE(ta_fill)
{
    Obj *o = this_ta(vm, self);
    u32 from, to, i;
    f64 d;
    UNUSED;
    if (!o) return v_undef();
    d = to_num(vm, ARG(0));
    from = rel(vm, ARG(1), o->u.ta.len, 0);
    to = rel(vm, ARG(2), o->u.ta.len, o->u.ta.len);
    for (i = from; i < to; i++) put_num(o->u.ta.data + i * SIZE[o->pad], o->pad, d);
    return self;
}

NATIVE(ta_at)
{
    Obj *o = this_ta(vm, self);
    f64 d;
    UNUSED;
    if (!o) return v_undef();
    d = m_trunc(to_num(vm, ARG(0)));
    if (d != d) d = 0;
    if (d < 0) d += o->u.ta.len;
    return d < 0 || d >= o->u.ta.len ? v_undef() : ta_get(o, (u32)d);
}

/* The rest: the Array method of that name on a plain copy.  magic: the
 * name's place in BRIDGE; those marked back write the copy back (sort,
 * reverse), those marked make return a new one of this kind (map, filter). */
static const char *const BRIDGE[] = { "forEach", "map", "filter", "reduce", "reduceRight", "every", "some", "find", "findIndex",
                                      "findLast", "findLastIndex", "indexOf", "lastIndexOf", "includes", "join", "toString",
                                      "keys", "values", "entries", "sort", "reverse", "toLocaleString", 0 };
enum { B_MAP = 1, B_FILTER = 2, B_SORT = 19, B_REVERSE = 20 };

NATIVE(ta_cmp)                           /* sort's default for numbers: NaN last */
{
    f64 a = to_num(vm, ARG(0)), b = to_num(vm, ARG(1));
    UNUSED;
    if (a != a) return v_num(b != b ? 0 : 1);
    if (b != b) return v_num(-1);
    return v_num(a < b ? -1 : a > b ? 1 : 0);
}

NATIVE(ta_bridge)
{
    Obj *o = this_ta(vm, self), *copy;
    Val fn, r, own[2];
    u32 i;
    UNUSED;
    if (!o) return v_undef();
    copy = arr_new(vm, 0);
    if (!copy) return v_undef();
    TEMP(vm, v_obj(copy));
    for (i = 0; i < o->u.ta.len; i++) arr_push(vm, copy, ta_get(o, i));
    fn = get_prop(vm, v_obj(vm->array_proto), atom(BRIDGE[magic]));
    if (magic == B_SORT && !is_callable(ARG(0))) { own[0] = v_obj(s_cmp); args = own; argc = 1; }
    r = vm_call(vm, fn, v_obj(copy), args, argc);
    if (!vm->throwing && (magic == B_SORT || magic == B_REVERSE)) {
        for (i = 0; i < o->u.ta.len; i++) ta_put(vm, o, i, arr_get(copy, i));
        r = self;
    } else if (!vm->throwing && (magic == B_MAP || magic == B_FILTER) && r.t == T_OBJ) {
        TEMP(vm, r);
        { Obj *n = ta_from_list(vm, o->pad, r); r = n ? v_obj(n) : v_undef(); }
        UNTEMP(vm, 1);
    }
    UNTEMP(vm, 1);
    return r;
}

/* ---- DataView -------------------------------------------------------------------------- */
NATIVE(dv_ctor)
{
    Obj *b, *o;
    u32 off, len;
    UNUSED;
    if (!vm->in_new) return throw_err(vm, E_TYPE, "DataView needs new");
    if (ARG(0).t != T_OBJ || ARG(0).u.o->cls != C_ABUF) return throw_err(vm, E_TYPE, "DataView needs an ArrayBuffer");
    b = ARG(0).u.o;
    off = ARG(1).t == T_UNDEF ? 0 : to_uint32(to_num(vm, ARG(1)));
    if (off > b->u.ta.len) return throw_err(vm, E_RANGE, "a bad offset for a DataView");
    len = ARG(2).t == T_UNDEF ? b->u.ta.len - off : to_uint32(to_num(vm, ARG(2)));
    if ((u64)off + len > b->u.ta.len) return throw_err(vm, E_RANGE, "past the end of the buffer");
    o = view_new(vm, TA_VIEW, b, off, len);
    return o ? v_obj(o) : v_undef();
}

NATIVE(dv_rw)                            /* magic: kind << 1 | set */
{
    int k = (int)(magic >> 1), set = (int)(magic & 1), little, i;
    u8 tmp[8], *p;
    u32 at;
    Obj *o;
    UNUSED;
    if (self.t != T_OBJ || self.u.o->cls != C_TARRAY || self.u.o->pad != TA_VIEW) return throw_err(vm, E_TYPE, "not a DataView");
    o = self.u.o;
    at = to_uint32(to_num(vm, ARG(0)));
    if ((u64)at + SIZE[k] > o->u.ta.len) return throw_err(vm, E_RANGE, "past the end of the DataView");
    little = truthy(ARG(set ? 2 : 1));
    p = o->u.ta.data + at;
    if (set) {
        put_num(tmp, k, to_num(vm, ARG(1)));
        for (i = 0; i < SIZE[k]; i++) p[i] = little ? tmp[i] : tmp[SIZE[k] - 1 - i];
        return v_undef();
    } else {
        Obj fake;
        for (i = 0; i < SIZE[k]; i++) tmp[i] = little ? p[i] : p[SIZE[k] - 1 - i];
        fake.pad = (u8)k; fake.u.ta.data = tmp;
        return ta_get(&fake, 0);
    }
}

/* ---- TextEncoder / TextDecoder (UTF-8 only, as the standard has it) -------------------- */
NATIVE(te_encode)
{
    Str *s = ARG(0).t == T_UNDEF ? S_empty : to_str(vm, ARG(0));
    u32 n;
    Obj *o;
    char *tmp;
    UNUSED;
    if (!s) return v_undef();
    {
        u32 i;                               /* its UTF-8 length first: no room wasted */
        for (n = 0, i = 0; i < s->len; i++) {
            u32 c = s->c[i];
            if (c < 0x80) n += 1;
            else if (c < 0x800) n += 2;
            else if (c >= 0xD800 && c < 0xDC00 && i + 1 < s->len && s->c[i + 1] >= 0xDC00 && s->c[i + 1] < 0xE000) { n += 4; i++; }
            else n += 3;
        }
    }
    TEMP(vm, v_str(s));
    o = ta_make(vm, 1, n);
    if (o) { tmp = (char *)o->u.ta.data; str_to_utf8(s, tmp, n); }
    UNTEMP(vm, 1);
    return o ? v_obj(o) : v_undef();
}

NATIVE(td_decode)
{
    Val a = ARG(0);
    Str *s;
    UNUSED;
    if (a.t == T_UNDEF) return v_str(S_empty);
    if (a.t != T_OBJ || (a.u.o->cls != C_ABUF && a.u.o->cls != C_TARRAY)) return throw_err(vm, E_TYPE, "decode needs bytes");
    if (a.u.o->cls == C_ABUF) s = str_utf8((const char *)a.u.o->u.ta.data, a.u.o->u.ta.len);
    else s = str_utf8((const char *)a.u.o->u.ta.data, a.u.o->u.ta.len * SIZE[a.u.o->pad]);
    if (!s) { vm->out_of_room = 1; return v_undef(); }
    return v_str(s);
}

NATIVE(text_ctor)                        /* both: nothing to keep, UTF-8 always; magic: its prototype's place */
{
    Obj *o;
    UNUSED;
    if (!vm->in_new) return throw_err(vm, E_TYPE, "needs new");
    o = obj_new(vm, vm->typed_proto[magic], C_OBJECT);
    return o ? v_obj(o) : v_undef();
}

static Obj *plain_ctor(Vm *vm, const char *name, NativeFn f, int nargs, u32 magic, Obj *proto)
{
    Obj *c = native_new(vm, name, f, nargs, magic);
    if (!c) return 0;
    c->flags |= OF_CTOR;
    prop_put(vm, c, S_prototype, v_obj(proto), P_HIDDEN);
    prop_put(vm, proto, S_constructor, v_obj(c), P_HIDDEN);
    prop_put(vm, proto, atom("@@toStringTag"), v_str(atom(name)), P_HIDDEN);
    def_val(vm, vm->global, name, v_obj(c));
    return c;
}

void typed_init(Vm *vm)
{
    static const char *const DVK[] = { "Int8", "Uint8", 0, "Int16", "Uint16", "Int32", "Uint32", "Float32", "Float64" };
    Obj *base, *c, *p;
    int k, i;
    s_buf_proto = obj_new(vm, vm->object_proto, C_OBJECT);
    base = obj_new(vm, vm->object_proto, C_OBJECT);       /* %TypedArray%.prototype */
    if (!s_buf_proto || !base) return;
    vm->typed_proto[0] = s_buf_proto; vm->typed_proto[1] = base;
    c = plain_ctor(vm, "ArrayBuffer", abuf_ctor, 1, 0, s_buf_proto);
    if (!c) return;
    def_native(vm, c, "isView", abuf_isview, 1, 0);
    def_accessor(vm, s_buf_proto, "byteLength", abuf_len, 0, 0);
    def_native(vm, s_buf_proto, "slice", abuf_slice, 2, 0);
    def_accessor(vm, base, "length", ta_info, 0, 0);
    def_accessor(vm, base, "byteLength", ta_info, 0, 1);
    def_accessor(vm, base, "byteOffset", ta_info, 0, 2);
    def_accessor(vm, base, "buffer", ta_info, 0, 3);
    def_native(vm, base, "set", ta_set, 1, 0);
    def_native(vm, base, "subarray", ta_sub, 2, 0);
    def_native(vm, base, "slice", ta_sub, 2, 1);
    def_native(vm, base, "fill", ta_fill, 1, 0);
    def_native(vm, base, "at", ta_at, 1, 0);
    for (i = 0; BRIDGE[i]; i++) def_native(vm, base, BRIDGE[i], ta_bridge, 1, (u32)i);
    prop_put(vm, base, atom("@@iterator"), get_prop(vm, v_obj(base), atom("values")), P_HIDDEN);
    s_cmp = native_new(vm, "compare", ta_cmp, 2, 0);
    vm->typed_proto[2] = s_cmp;
    for (k = 0; k < TA_VIEW; k++) {
        p = obj_new(vm, base, C_OBJECT);
        if (!p) return;
        s_proto[k] = p;
        vm->typed_proto[3 + k] = p;
        c = plain_ctor(vm, NAMES[k], ta_ctor, 3, (u32)k, p);
        if (!c) return;
        s_ctor[k] = c;
        def_val(vm, c, "BYTES_PER_ELEMENT", v_num(SIZE[k]));
        def_val(vm, p, "BYTES_PER_ELEMENT", v_num(SIZE[k]));
        def_native(vm, c, "from", ta_from_of, 1, (u32)(k << 1));
        def_native(vm, c, "of", ta_from_of, 0, (u32)(k << 1 | 1));
    }
    p = obj_new(vm, vm->object_proto, C_OBJECT);
    if (!p) return;
    s_proto[TA_VIEW] = p;
    vm->typed_proto[3 + TA_VIEW] = p;
    plain_ctor(vm, "DataView", dv_ctor, 1, 0, p);
    def_accessor(vm, p, "byteLength", ta_info, 0, 1);
    def_accessor(vm, p, "byteOffset", ta_info, 0, 2);
    def_accessor(vm, p, "buffer", ta_info, 0, 3);
    for (k = 0; k < TA_VIEW; k++) {
        char name[16];
        if (!DVK[k]) continue;
        fmt_name(name, "get", DVK[k]); def_native(vm, p, name, dv_rw, 1, (u32)(k << 1));
        fmt_name(name, "set", DVK[k]); def_native(vm, p, name, dv_rw, 2, (u32)(k << 1 | 1));
    }
    p = obj_new(vm, vm->object_proto, C_OBJECT);
    if (!p) return;
    vm->typed_proto[4 + TA_VIEW] = p;
    plain_ctor(vm, "TextEncoder", text_ctor, 0, 4 + TA_VIEW, p);
    def_native(vm, p, "encode", te_encode, 1, 0);
    def_val(vm, p, "encoding", v_str(atom("utf-8")));
    p = obj_new(vm, vm->object_proto, C_OBJECT);
    if (!p) return;
    vm->typed_proto[5 + TA_VIEW] = p;
    plain_ctor(vm, "TextDecoder", text_ctor, 0, 5 + TA_VIEW, p);
    def_native(vm, p, "decode", td_decode, 1, 0);
    def_val(vm, p, "encoding", v_str(atom("utf-8")));
}
