/* mapset.c -- Map, Set, WeakMap and WeakSet.
 *
 * One table serves all four: the entries in the order they were added (a
 * key and a value each, in a Vals block), and a hash index over them (a
 * K_BYTES block of entry numbers, 0 for an empty slot).  A deleted entry
 * keeps its place with its key marked gone, so an iterator part way
 * through carries on; the gone ones are dropped when the table grows.
 * Keys are compared the way the standard says (SameValueZero: NaN equals
 * NaN, -0 equals 0).
 *
 * WeakMap and WeakSet take objects only, as the standard says, but they
 * hold what they are given: nothing here is forgotten while the table lives.
 */
#include "js.h"

#define ARG(i) ((i) < argc ? args[i] : v_undef())
#define NATIVE(name) static Val name(Vm *vm, Val self, Val *args, int argc, u32 magic)
#define UNUSED (void)self; (void)args; (void)argc; (void)magic

#define T_GONE 7                         /* a deleted entry's key           */
enum { K_MAP, K_SET, K_WEAKMAP, K_WEAKSET };   /* o->pad on a C_MAP          */
enum { I_KEYS, I_VALUES, I_ENTRIES };         /* o->pad on a C_MAPITER      */

#define KV(o)  ((o)->u.map.kv->v)

/* ---- the table ---------------------------------------------------------------------------- */
static u32 hash_val(Val v)
{
    switch (v.t) {
    case T_STR: return str_hash(v.u.s);
    case T_OBJ: return (u32)(__SIZE_TYPE__)v.u.o * 2654435761u;
    case T_NUM: {
        union { f64 f; u32 w[2]; } u;
        u.f = v.u.n;
        if (v.u.n == 0) return 0;            /* -0 and 0 alike                 */
        if (v.u.n != v.u.n) return 0x7FF80000u;
        return (u.w[0] ^ (u.w[1] * 31u)) * 2654435761u;
    }
    case T_BOOL: return v.u.b ? 0x51u : 0x50u;
    default: return v.t * 0x9E3779B9u;
    }
}

static int same_zero(Val a, Val b)
{
    if (a.t != b.t) return 0;
    if (a.t == T_NUM) return a.u.n == b.u.n || (a.u.n != a.u.n && b.u.n != b.u.n);
    return strict_eq(a, b);
}

static u32 *index_of(Obj *m) { return m->u.map.idx; }

static s32 find(Obj *m, Val k, u32 h)
{
    u32 *idx = index_of(m), mask, i, e;
    if (!idx) return -1;
    mask = m->u.map.icap - 1;
    for (i = h & mask; (e = idx[i]) != 0; i = (i + 1) & mask)
        if (same_zero(KV(m)[2 * (e - 1)], k)) return (s32)(e - 1);
    return -1;
}

/* Room for one more: a bigger table, the gone entries left behind. */
static int grow(Vm *vm, Obj *m)
{
    u32 live = m->u.map.live, cap = live < 4 ? 8 : live * 2, icap = 16, i, j = 0;
    Vals *kv;
    GcHdr *raw;
    u32 *idx;
    while (icap < cap * 2) icap <<= 1;
    kv = gc_alloc(sizeof(Vals) + 2 * cap * sizeof(Val), K_VALS);
    if (!kv) { vm->out_of_room = 1; return 0; }
    kv->n = 2 * cap;
    raw = gc_alloc(sizeof(GcHdr) + icap * sizeof(u32), K_BYTES);   /* no sweep comes between: see gc.c */
    if (!raw) { vm->out_of_room = 1; return 0; }
    idx = (u32 *)(raw + 1);
    for (i = 0; m->u.map.kv && i < m->u.map.n; i++) {
        Val k = KV(m)[2 * i];
        if (k.t == T_GONE) continue;
        kv->v[2 * j] = k;
        kv->v[2 * j + 1] = KV(m)[2 * i + 1];
        j++;
    }
    for (i = j; i < cap; i++) kv->v[2 * i].t = T_UNDEF;
    m->u.map.kv = kv;
    m->u.map.idx = idx;
    m->u.map.icap = icap;
    m->u.map.n = j;
    for (i = 0; i < j; i++) {
        u32 s = hash_val(kv->v[2 * i]) & (icap - 1);
        while (idx[s]) s = (s + 1) & (icap - 1);
        idx[s] = i + 1;
    }
    return 1;
}

static int put(Vm *vm, Obj *m, Val k, Val v)
{
    u32 h, s;
    s32 at;
    if (k.t == T_NUM && k.u.n == 0) k.u.n = 0;   /* -0 is kept as 0         */
    h = hash_val(k);
    at = find(m, k, h);
    if (at >= 0) { KV(m)[2 * at + 1] = v; return 1; }
    if (!m->u.map.kv || m->u.map.n * 2 >= m->u.map.kv->n) {
        TEMP(vm, k); TEMP(vm, v);
        s = (u32)grow(vm, m);
        UNTEMP(vm, 2);
        if (!s) return 0;
    }
    at = (s32)m->u.map.n++;
    KV(m)[2 * at] = k;
    KV(m)[2 * at + 1] = v;
    m->u.map.live++;
    for (s = h & (m->u.map.icap - 1); m->u.map.idx[s]; s = (s + 1) & (m->u.map.icap - 1)) {}
    m->u.map.idx[s] = (u32)at + 1;
    return 1;
}

static int del(Obj *m, Val k)
{
    s32 at = find(m, k, hash_val(k));
    if (at < 0) return 0;
    KV(m)[2 * at].t = T_GONE;                /* the index still points here: it never matches */
    KV(m)[2 * at + 1] = v_undef();
    m->u.map.live--;
    return 1;
}

/* ---- checking what a method was called on ------------------------------------------------ */
static Obj *this_table(Vm *vm, Val self, int kind, const char *what)
{
    if (self.t == T_OBJ && self.u.o->cls == C_MAP && self.u.o->pad == kind) return self.u.o;
    throw_err(vm, E_TYPE, "%s called on something that is not one", what);
    return 0;
}

static Obj *table_new(Vm *vm, Obj *proto, int kind)
{
    Obj *m = obj_new(vm, proto, C_MAP);
    if (m) m->pad = (u8)kind;
    return m;
}

static Obj *s_proto[4], *s_iter_proto;

/* ---- constructors -------------------------------------------------------------------------- */
NATIVE(table_ctor)                      /* magic: the kind                  */
{
    Obj *m;
    Val src = ARG(0);
    static const char *names[4] = { "Map", "Set", "WeakMap", "WeakSet" };
    UNUSED;
    if (!vm->in_new && self.t == T_OBJ && self.u.o != vm->global && self.u.o->cls == C_OBJECT) {
        m = self.u.o;                        /* class X extends Map: super()    */
        m->cls = C_MAP;
        m->pad = (u8)magic;
    } else if (!vm->in_new) {
        return throw_err(vm, E_TYPE, "%s needs new", names[magic]);
    } else {
        m = table_new(vm, s_proto[magic], (int)magic);
        if (!m) return v_undef();
    }
    if (IS_NULLISH(src)) return vm->in_new ? v_obj(m) : v_undef();
    TEMP(vm, v_obj(m));
    {
        Obj *it = vm_iter_new(vm, src);
        Val item;
        if (it) {
            TEMP(vm, v_obj(it));
            while (!vm->throwing && vm_iter_next(vm, it, &item)) {
                Val k = item, v = item;
                if (magic == K_MAP || magic == K_WEAKMAP) {
                    if (item.t != T_OBJ) { throw_err(vm, E_TYPE, "a %s is made from [key, value] pairs", names[magic]); break; }
                    TEMP(vm, item);
                    k = get_elem(vm, item, v_num(0));
                    TEMP(vm, k);
                    v = get_elem(vm, item, v_num(1));
                    UNTEMP(vm, 2);
                    if (vm->throwing) break;
                }
                if ((magic == K_WEAKMAP || magic == K_WEAKSET) && k.t != T_OBJ) { throw_err(vm, E_TYPE, "a %s takes objects only", names[magic]); break; }
                if (!put(vm, m, k, v)) break;
            }
            UNTEMP(vm, 1);
        }
    }
    UNTEMP(vm, 1);
    return vm->in_new ? v_obj(m) : v_undef();
}

/* ---- methods: magic is the kind they belong to ------------------------------------------- */
NATIVE(t_get)
{
    Obj *m = this_table(vm, self, (int)magic, "get");
    s32 at;
    UNUSED;
    if (!m) return v_undef();
    at = find(m, ARG(0), hash_val(ARG(0)));
    return at < 0 ? v_undef() : KV(m)[2 * at + 1];
}

NATIVE(t_has)
{
    Obj *m = this_table(vm, self, (int)magic, "has");
    UNUSED;
    if (!m) return v_undef();
    return v_bool(find(m, ARG(0), hash_val(ARG(0))) >= 0);
}

NATIVE(t_set)                            /* Map/WeakMap set(k, v); Set/WeakSet add(v) */
{
    Obj *m = this_table(vm, self, (int)magic, magic == K_SET || magic == K_WEAKSET ? "add" : "set");
    Val k = ARG(0);
    UNUSED;
    if (!m) return v_undef();
    if (k.t == T_NUM && k.u.n == 0) k.u.n = 0;   /* -0 is kept as 0 */
    if ((magic == K_WEAKMAP || magic == K_WEAKSET) && k.t != T_OBJ)
        return throw_err(vm, E_TYPE, "a weak table takes objects only");
    put(vm, m, k, magic == K_MAP || magic == K_WEAKMAP ? ARG(1) : k);
    return self;
}

NATIVE(t_delete)
{
    Obj *m = this_table(vm, self, (int)magic, "delete");
    UNUSED;
    if (!m) return v_undef();
    return v_bool(del(m, ARG(0)));
}

NATIVE(t_clear)
{
    Obj *m = this_table(vm, self, (int)magic, "clear");
    u32 i;
    UNUSED;
    if (!m) return v_undef();
    /* every entry gone, the table kept: an iterator part way through ends */
    for (i = 0; i < m->u.map.n; i++) { KV(m)[2 * i].t = T_GONE; KV(m)[2 * i + 1] = v_undef(); }
    m->u.map.live = 0;
    return v_undef();
}

NATIVE(t_size)
{
    Obj *m = this_table(vm, self, (int)magic, "size");
    UNUSED;
    return m ? v_num(m->u.map.live) : v_undef();
}

NATIVE(t_foreach)
{
    Obj *m = this_table(vm, self, (int)magic, "forEach");
    Val fn = ARG(0), a[3];
    u32 i;
    UNUSED;
    if (!m) return v_undef();
    if (!is_callable(fn)) return throw_err(vm, E_TYPE, "forEach needs a function");
    for (i = 0; m->u.map.kv && i < m->u.map.n && !vm->throwing; i++) {   /* n read again: it may grow */
        Val k = KV(m)[2 * i];
        if (k.t == T_GONE) continue;
        a[0] = KV(m)[2 * i + 1]; a[1] = k; a[2] = self;
        vm_call(vm, fn, ARG(1), a, 3);
    }
    return v_undef();
}

/* ---- iterators ----------------------------------------------------------------------------- */
static Obj *iter_of(Vm *vm, Obj *m, int what)
{
    Obj *it = obj_new(vm, s_iter_proto, C_MAPITER);
    if (!it) return 0;
    it->u.iter.of = v_obj(m);
    it->u.iter.i = 0;
    it->pad = (u8)what;
    return it;
}

NATIVE(t_iter)                           /* magic: kind << 2 | what         */
{
    Obj *m = this_table(vm, self, (int)(magic >> 2), "an iterator");
    Obj *it;
    UNUSED;
    if (!m) return v_undef();
    it = iter_of(vm, m, (int)(magic & 3));
    return it ? v_obj(it) : v_undef();
}

/* The next entry for for-of (vm.c) and for next(): 0 when it is done. */
int mapiter_step(Vm *vm, Obj *it, Val *out)
{
    Obj *m;
    if (it->u.iter.of.t != T_OBJ) return 0;
    m = it->u.iter.of.u.o;
    while (m->u.map.kv && it->u.iter.i < m->u.map.n) {
        u32 i = it->u.iter.i++;
        Val k = KV(m)[2 * i];
        if (k.t == T_GONE) continue;
        if (it->pad == I_KEYS) *out = k;
        else if (it->pad == I_VALUES) *out = KV(m)[2 * i + 1];
        else {
            Obj *pair = arr_new(vm, 0);
            if (!pair) return 0;
            arr_push(vm, pair, k);
            arr_push(vm, pair, KV(m)[2 * i + 1]);
            *out = v_obj(pair);
        }
        return 1;
    }
    it->u.iter.of = v_undef();               /* once done, done for good        */
    return 0;
}

/* for-of straight over a Map (its entries) or a Set (its values) */
Obj *mapiter_for(Vm *vm, Obj *m)
{
    return iter_of(vm, m, m->pad == K_MAP ? I_ENTRIES : I_VALUES);
}

NATIVE(it_next)
{
    Val v = v_undef();
    Obj *r;
    int more;
    UNUSED;
    if (self.t != T_OBJ || self.u.o->cls != C_MAPITER) return throw_err(vm, E_TYPE, "next called on something that is not a Map or Set iterator");
    more = mapiter_step(vm, self.u.o, &v);
    if (vm->throwing) return v_undef();
    TEMP(vm, v);
    r = obj_new(vm, vm->object_proto, C_OBJECT);
    UNTEMP(vm, 1);
    if (!r) return v_undef();
    prop_put(vm, r, S_value, more ? v : v_undef(), 0);
    prop_put(vm, r, S_done, v_bool(!more), 0);
    return v_obj(r);
}

NATIVE(it_self) { UNUSED; (void)vm; return self; }

/* ---- Map.groupBy and Object.groupBy -------------------------------------------------------- */
NATIVE(group_by)                         /* magic 0: Object.groupBy, 1: Map.groupBy */
{
    Val fn = ARG(1), item, key, a[2];
    Obj *out, *it;
    u32 n = 0;
    UNUSED;
    if (!is_callable(fn)) return throw_err(vm, E_TYPE, "groupBy needs a function");
    out = magic ? table_new(vm, s_proto[K_MAP], K_MAP) : obj_new(vm, 0, C_OBJECT);
    if (!out) return v_undef();
    TEMP(vm, v_obj(out));
    it = vm_iter_new(vm, ARG(0));
    if (it) {
        TEMP(vm, v_obj(it));
        while (!vm->throwing && vm_iter_next(vm, it, &item)) {
            Val list;
            TEMP(vm, item);
            a[0] = item; a[1] = v_num(n++);
            key = vm_call(vm, fn, v_undef(), a, 2);
            if (vm->throwing) { UNTEMP(vm, 1); break; }
            TEMP(vm, key);
            if (magic) {
                s32 at = find(out, key, hash_val(key));
                list = at >= 0 ? KV(out)[2 * at + 1] : v_undef();
            } else {
                Str *ks = to_key(vm, key);
                Prop *q = ks ? prop_find(out, ks) : 0;
                list = q ? q->v : v_undef();
                if (ks) key = v_str(ks);
            }
            if (!vm->throwing && list.t != T_OBJ) {
                Obj *l = arr_new(vm, 0);
                if (l) {
                    list = v_obj(l);
                    if (magic) put(vm, out, key, list);
                    else prop_put(vm, out, key.u.s, list, 0);
                }
            }
            if (list.t == T_OBJ) arr_push(vm, list.u.o, item);
            UNTEMP(vm, 2);
        }
        UNTEMP(vm, 1);
    }
    UNTEMP(vm, 1);
    return v_obj(out);
}

/* ---- making them -------------------------------------------------------------------------- */
static Obj *make_ctor(Vm *vm, const char *name, int kind)
{
    Obj *p = obj_new(vm, vm->object_proto, C_OBJECT), *c;
    if (!p) return 0;
    s_proto[kind] = p;
    c = native_new(vm, name, table_ctor, 0, (u32)kind);
    if (!c) return 0;
    c->flags |= OF_CTOR;
    prop_put(vm, c, S_prototype, v_obj(p), P_HIDDEN);
    prop_put(vm, p, S_constructor, v_obj(c), P_HIDDEN);
    prop_put(vm, p, atom("@@toStringTag"), v_str(atom(name)), P_HIDDEN);
    def_val(vm, vm->global, name, v_obj(c));
    return c;
}

static void link_iter(Vm *vm, Obj *p, const char *name, int kind, int what, int also_iterator)
{
    Obj *f = native_new(vm, name, t_iter, 0, (u32)(kind << 2 | what));
    if (!f) return;
    prop_put(vm, p, atom(name), v_obj(f), P_HIDDEN);
    if (also_iterator) prop_put(vm, p, atom("@@iterator"), v_obj(f), P_HIDDEN);
}

void mapset_init(Vm *vm)
{
    Obj *c, *p;
    int k;
    for (k = 0; k < 4; k++) {
        static const char *names[4] = { "Map", "Set", "WeakMap", "WeakSet" };
        c = make_ctor(vm, names[k], k);
        if (!c) return;
        p = s_proto[k];
        if (k == K_MAP || k == K_WEAKMAP) { def_native(vm, p, "get", t_get, 1, (u32)k); def_native(vm, p, "set", t_set, 2, (u32)k); }
        else def_native(vm, p, "add", t_set, 1, (u32)k);
        def_native(vm, p, "has", t_has, 1, (u32)k);
        def_native(vm, p, "delete", t_delete, 1, (u32)k);
        if (k == K_MAP || k == K_SET) {
            def_native(vm, p, "clear", t_clear, 0, (u32)k);
            def_native(vm, p, "forEach", t_foreach, 1, (u32)k);
            def_accessor(vm, p, "size", t_size, 0, (u32)k);
            link_iter(vm, p, "keys", k, k == K_MAP ? I_KEYS : I_VALUES, 0);
            link_iter(vm, p, "values", k, I_VALUES, k == K_SET);
            link_iter(vm, p, "entries", k, I_ENTRIES, k == K_MAP);
        }
        if (k == K_MAP) def_native(vm, c, "groupBy", group_by, 2, 1);
    }
    s_iter_proto = obj_new(vm, vm->iter_proto, C_OBJECT);
    if (!s_iter_proto) return;
    def_native(vm, s_iter_proto, "next", it_next, 0, 0);
    def_native(vm, s_iter_proto, "@@iterator", it_self, 0, 0);
    {
        Val o = get_prop(vm, v_obj(vm->global), atom("Object"));
        if (o.t == T_OBJ) def_native(vm, o.u.o, "groupBy", group_by, 2, 0);
    }
    for (k = 0; k < 4; k++) vm->coll_proto[k] = s_proto[k];
    vm->coll_proto[4] = s_iter_proto;
}
