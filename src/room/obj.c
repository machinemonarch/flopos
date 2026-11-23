/* ===========================================================================
 * obj.c -- objects and arrays, and getting and setting their properties.
 *
 * An object's properties are kept in the order they were made (scripts
 * notice the order: for-in, Object.keys, JSON).  Small objects are
 * searched straight through; bigger ones get an index by name.  Arrays
 * keep their elements in a plain list.
 *
 * Getting a property walks the prototype chain: the object, then the
 * object it was made from, and so on, and a getter found on the way is
 * called.  That is also how a page's elements work: element.textContent
 * is a getter on the elements' prototype that asks browzer (dom.c).
 * ===========================================================================
 */
#include "js.h"

/* A page's style and classList objects answer for themselves (dom.c);
 * without a page, nothing does. */
__attribute__((weak)) Val dom_get_hook(Vm *vm, Obj *o, Str *k, int *done) { (void)vm; (void)o; (void)k; *done = 0; return v_undef(); }
__attribute__((weak)) int dom_set_hook(Vm *vm, Obj *o, Str *k, Val v) { (void)vm; (void)o; (void)k; (void)v; return 1; }

/* ---- property tables --------------------------------------------------------- */
static u16 *index_of(Props *p) { return (u16 *)&p->p[p->cap]; }

static Props *props_new(u32 cap)
{
    Props *p = gc_alloc(sizeof(Props) + cap * sizeof(Prop) + cap * 2 * 2, K_PROPS);
    if (p) p->cap = cap;
    return p;
}

static void index_add(Props *p, u32 i)
{
    u16 *ix = index_of(p);
    u32 m = p->cap * 2 - 1, h = str_hash(p->p[i].k) & m;
    while (ix[h]) h = (h + 1) & m;
    ix[h] = (u16)(i + 1);
}

Prop *prop_find(Obj *o, Str *k)
{
    Props *p = o->props;
    u32 i;
    if (!p) return 0;
    if (p->cap <= 8) {
        for (i = 0; i < p->n; i++) if (p->p[i].k == k) return &p->p[i];
        return 0;
    }
    {
        u16 *ix = index_of(p);
        u32 m = p->cap * 2 - 1, h = str_hash(k) & m;
        while (ix[h]) {
            Prop *q = &p->p[ix[h] - 1];
            if (q->k == k) return q;
            h = (h + 1) & m;
        }
    }
    return 0;
}

int prop_put(Vm *vm, Obj *o, Str *k, Val v, u32 flags)
{
    Props *p = o->props;
    Prop *q = prop_find(o, k);
    if (q) { q->v = v; q->flags = flags; return 1; }
    if (!p || p->n == p->cap) {
        u32 cap = p ? (p->live * 2 > 4 ? p->live * 2 : 4) : 4, i, n = 0;
        Props *np;
        if (cap > 30000) { throw_err(vm, E_RANGE, "too many properties"); return 0; }
        if (p && cap < p->live + 1) cap = p->live + 1;
        np = props_new(cap);
        if (!np) { vm->out_of_room = 1; return 0; }
        if (p) for (i = 0; i < p->n; i++) if (p->p[i].k) np->p[n++] = p->p[i];
        np->n = np->live = n;
        if (np->cap > 8) for (i = 0; i < n; i++) index_add(np, i);
        o->props = p = np;
    }
    q = &p->p[p->n];
    q->k = k; q->v = v; q->set = v_undef(); q->flags = flags;
    if (p->cap > 8) index_add(p, p->n);
    p->n++; p->live++;
    return 1;
}

int prop_del(Obj *o, Str *k)
{
    Prop *q = prop_find(o, k);
    if (!q) return 0;
    q->k = 0;                            /* the index skips it; it is gone at the next growth */
    q->v = v_undef();
    o->props->live--;
    /* An emptied small table: compact now, so for-in order stays simple. */
    if (o->props->cap <= 8) {
        Props *p = o->props;
        u32 i, n = 0;
        for (i = 0; i < p->n; i++) if (p->p[i].k) p->p[n++] = p->p[i];
        p->n = n;
    }
    return 1;
}

/* ---- objects ------------------------------------------------------------------ */
Obj *obj_new(Vm *vm, Obj *proto, int cls)
{
    Obj *o = gc_alloc(sizeof(Obj), K_OBJ);
    if (!o) { vm->out_of_room = 1; return 0; }
    o->proto = proto;
    o->cls = (u8)cls;
    return o;
}

Obj *arr_new(Vm *vm, u32 len)
{
    Obj *a = obj_new(vm, vm->array_proto, C_ARRAY);
    u32 cap = len < 4 ? 4 : len;
    if (!a) return 0;
    a->u.arr.items = gc_alloc(sizeof(Vals) + cap * sizeof(Val), K_VALS);
    if (!a->u.arr.items) { vm->out_of_room = 1; return 0; }
    a->u.arr.items->n = cap;
    a->u.arr.len = len;
    return a;
}

static int arr_room(Vm *vm, Obj *a, u32 n)
{
    Vals *it = a->u.arr.items, *ni;
    u32 cap;
    if (it && n <= it->n) return 1;
    cap = it ? it->n * 2 : 4;
    if (cap < n) cap = n;
    if (cap > 0x100000) { if (n > 0x100000) { throw_err(vm, E_RANGE, "array too long"); return 0; } cap = 0x100000; }
    ni = gc_alloc(sizeof(Vals) + cap * sizeof(Val), K_VALS);
    if (!ni) { vm->out_of_room = 1; return 0; }
    ni->n = cap;
    if (it) memcpy(ni->v, it->v, a->u.arr.len * sizeof(Val));
    a->u.arr.items = ni;
    return 1;
}

int arr_push(Vm *vm, Obj *a, Val v)
{
    if (!arr_room(vm, a, a->u.arr.len + 1)) return 0;
    a->u.arr.items->v[a->u.arr.len++] = v;
    return 1;
}

Val arr_get(Obj *a, u32 i)
{
    if (i < a->u.arr.len && a->u.arr.items) return a->u.arr.items->v[i];
    return v_undef();
}

int arr_set(Vm *vm, Obj *a, u32 i, Val v)
{
    if (i >= a->u.arr.len) {
        u32 k;
        if (!arr_room(vm, a, i + 1)) return 0;
        for (k = a->u.arr.len; k < i; k++) a->u.arr.items->v[k] = v_undef();
        a->u.arr.len = i + 1;
    }
    a->u.arr.items->v[i] = v;
    return 1;
}

int arr_set_len(Vm *vm, Obj *a, u32 n)
{
    u32 k;
    if (n > a->u.arr.len) {
        if (!arr_room(vm, a, n)) return 0;
        for (k = a->u.arr.len; k < n; k++) a->u.arr.items->v[k] = v_undef();
    }
    a->u.arr.len = n;
    return 1;
}

Obj *native_new(Vm *vm, const char *name, NativeFn f, int nargs, u32 magic)
{
    Obj *o = obj_new(vm, vm->function_proto, C_NATIVE);
    if (!o) return 0;
    o->flags = OF_CALLABLE;
    o->u.nat.f = f;
    o->u.nat.magic = magic;
    o->u.nat.name = atom(name);
    o->u.nat.nargs = (u32)nargs;
    return o;
}

void def_native(Vm *vm, Obj *o, const char *name, NativeFn f, int nargs, u32 magic)
{
    Obj *fn = native_new(vm, name, f, nargs, magic);
    if (fn) prop_put(vm, o, atom(name), v_obj(fn), P_HIDDEN);
}

void def_accessor(Vm *vm, Obj *o, const char *name, NativeFn get, NativeFn set, u32 magic)
{
    Obj *g = get ? native_new(vm, name, get, 0, magic) : 0;
    Obj *s = set ? native_new(vm, name, set, 1, magic) : 0;
    Str *k = atom(name);
    Prop *q;
    prop_put(vm, o, k, g ? v_obj(g) : v_undef(), P_ACCESSOR | P_HIDDEN);
    q = prop_find(o, k);
    if (q) q->set = s ? v_obj(s) : v_undef();
}

void def_val(Vm *vm, Obj *o, const char *name, Val v) { prop_put(vm, o, atom(name), v, P_HIDDEN); }

/* ---- getting -------------------------------------------------------------------- */
static Val fn_own(Vm *vm, Obj *o, Str *k, int *done)
{
    *done = 0;
    if (o->cls == C_FUNC && k == S_prototype && !(o->flags & OF_ARROW)) {
        Obj *p = obj_new(vm, vm->object_proto, C_OBJECT);
        if (!p) return v_undef();
        prop_put(vm, p, S_constructor, v_obj(o), P_HIDDEN);
        prop_put(vm, o, S_prototype, v_obj(p), P_HIDDEN);
        *done = 1;
        return v_obj(p);
    }
    if (k == S_length) {
        *done = 1;
        if (o->cls == C_FUNC) return v_num(o->u.fn.fn->nparams);
        if (o->cls == C_NATIVE) return v_num(o->u.nat.nargs);
        if (o->cls == C_BOUND) return v_num(0);
        *done = 0;
    }
    if (k == S_name) {
        if (o->cls == C_FUNC) { *done = 1; return v_str(o->u.fn.fn->name ? o->u.fn.fn->name : S_empty); }
        if (o->cls == C_NATIVE) { *done = 1; return v_str(o->u.nat.name); }
    }
    return v_undef();
}

static Val get_from(Vm *vm, Obj *o, Str *k, Val self)
{
    int guard = 0;
    while (o && guard++ < 1000) {
        Prop *q;
        int done;
        Val v;
        if (o->cls == C_ARRAY || o->cls == C_ARGS) {
            u32 i;
            if (k == S_length) return v_num(o->u.arr.len);
            if (str_is_index(k, &i)) {
                if (i < o->u.arr.len) return arr_get(o, i);
            }
        } else if (o->cls == C_STYLE || o->cls == C_CLASSLIST) {
            v = dom_get_hook(vm, o, k, &done);
            if (done) return v;
        } else if (o->cls == C_BOXED && IS_STR(o->u.boxed.v)) {
            u32 i;
            Str *s = o->u.boxed.v.u.s;
            if (k == S_length) return v_num(s->len);
            if (str_is_index(k, &i) && i < s->len) return v_str(str_from16(&s->c[i], 1));
        }
        q = prop_find(o, k);
        if (q) {
            if (q->flags & P_ACCESSOR) {
                if (!IS_OBJ(q->v)) return v_undef();
                return vm_call(vm, q->v, self, 0, 0);
            }
            return q->v;
        }
        if (o->cls == C_FUNC || o->cls == C_NATIVE || o->cls == C_BOUND) {
            v = fn_own(vm, o, k, &done);
            if (done) return v;
        }
        o = o->proto;
    }
    return v_undef();
}

Val get_prop(Vm *vm, Val o, Str *k)
{
    switch (o.t) {
    case T_OBJ: {
        u32 i;
        if (o.u.o->cls == C_PROXY) return proxy_get(vm, o.u.o, k, o);
        if (o.u.o->cls == C_TARRAY && o.u.o->pad != TA_VIEW) {
            if (k == S_length) return v_num(o.u.o->u.ta.len);      /* the loop's test, quickly */
            if (str_is_index(k, &i)) return i < o.u.o->u.ta.len ? ta_get(o.u.o, i) : v_undef();
        }
        return get_from(vm, o.u.o, k, o);
    }
    case T_STR: {
        u32 i;
        if (k == S_length) return v_num(o.u.s->len);
        if (str_is_index(k, &i)) return i < o.u.s->len ? v_str(str_from16(&o.u.s->c[i], 1)) : v_undef();
        return get_from(vm, vm->string_proto, k, o);
    }
    case T_NUM: return get_from(vm, vm->number_proto, k, o);
    case T_BOOL: return get_from(vm, vm->boolean_proto, k, o);
    default: {
        char b[64];
        u32 n = str_to_utf8(k, b, 60);
        b[n] = 0;
        return throw_err(vm, E_TYPE, "cannot read '%s' of %s", b, o.t == T_NULL ? "null" : "undefined");
    }
    }
}

Val get_elem(Vm *vm, Val o, Val k)
{
    if (o.t == T_OBJ && o.u.o->cls == C_TARRAY && o.u.o->pad != TA_VIEW && k.t == T_NUM) {
        f64 d = k.u.n;
        if (d >= 0 && d == (f64)(u32)d) return (u32)d < o.u.o->u.ta.len ? ta_get(o.u.o, (u32)d) : v_undef();
    }
    if (o.t == T_OBJ && (o.u.o->cls == C_ARRAY || o.u.o->cls == C_ARGS) && k.t == T_NUM) {
        f64 d = k.u.n;
        if (d >= 0 && d < 4294967295.0 && d == (f64)(u32)d) {
            u32 i = (u32)d;
            if (i < o.u.o->u.arr.len) return arr_get(o.u.o, i);
            return get_from(vm, o.u.o->proto, to_key(vm, k), o);
        }
    }
    if (o.t == T_STR && k.t == T_NUM) {
        f64 d = k.u.n;
        if (d >= 0 && d == (f64)(u32)d) {
            u32 i = (u32)d;
            return i < o.u.s->len ? v_str(str_from16(&o.u.s->c[i], 1)) : v_undef();
        }
    }
    if (IS_NULLISH(o)) {
        Str *ks = to_key(vm, k);
        return ks ? get_prop(vm, o, ks) : v_undef();
    }
    {
        Str *ks = to_key(vm, k);
        if (!ks) return v_undef();
        return get_prop(vm, o, ks);
    }
}

/* ---- setting --------------------------------------------------------------------- */
int set_prop(Vm *vm, Val ov, Str *k, Val v)
{
    Obj *o, *p;
    int guard = 0;
    if (ov.t != T_OBJ) {
        if (IS_NULLISH(ov)) {
            char b[64];
            u32 n = str_to_utf8(k, b, 60);
            b[n] = 0;
            throw_err(vm, E_TYPE, "cannot set '%s' of %s", b, ov.t == T_NULL ? "null" : "undefined");
            return 0;
        }
        return 1;                        /* on a plain string or number: nothing */
    }
    o = ov.u.o;
    if (o->cls == C_PROXY) return proxy_set(vm, o, k, v, ov);
    if (o->cls == C_ARRAY || o->cls == C_ARGS) {
        u32 i;
        if (k == S_length) {
            f64 d = to_num(vm, v);
            if (d < 0 || d != (f64)(u32)d) { throw_err(vm, E_RANGE, "bad array length"); return 0; }
            return arr_set_len(vm, o, (u32)d);
        }
        if (str_is_index(k, &i) && i < o->u.arr.len + 65536) return o->sealed ? 1 : arr_set(vm, o, i, v);
    }
    if (o->cls == C_TARRAY && o->pad != TA_VIEW) {
        u32 i;
        if (str_is_index(k, &i)) return ta_put(vm, o, i, v);
    }
    if (o->cls == C_STYLE || o->cls == C_CLASSLIST) return dom_set_hook(vm, o, k, v);
    /* A setter anywhere up the chain is called; an own value is replaced. */
    for (p = o; p && guard++ < 1000; p = p->proto) {
        Prop *q = prop_find(p, k);
        if (!q) continue;
        if (q->flags & P_ACCESSOR) {
            if (IS_OBJ(q->set)) { vm_call(vm, q->set, ov, &v, 1); return !vm->throwing; }
            return 1;                    /* a getter with no setter: ignored */
        }
        if (p == o) { if (!o->sealed) q->v = v; return 1; }
        break;
    }
    if (o->sealed) return 1;
    return prop_put(vm, o, k, v, 0);
}

int set_elem(Vm *vm, Val o, Val k, Val v)
{
    Str *ks;
    if (o.t == T_OBJ && o.u.o->cls == C_TARRAY && o.u.o->pad != TA_VIEW && k.t == T_NUM) {
        f64 d = k.u.n;
        if (d >= 0 && d == (f64)(u32)d) return ta_put(vm, o.u.o, (u32)d, v);
    }
    if (o.t == T_OBJ && o.u.o->cls == C_ARRAY && k.t == T_NUM) {
        f64 d = k.u.n;
        if (d >= 0 && d == (f64)(u32)d && d < o.u.o->u.arr.len + 65536.0) {
            if (o.u.o->sealed) return 1;
            return arr_set(vm, o.u.o, (u32)d, v);
        }
    }
    ks = to_key(vm, k);
    if (!ks) return 0;
    return set_prop(vm, o, ks, v);
}

int has_prop(Vm *vm, Obj *o, Str *k)
{
    int guard = 0;
    for (; o && guard++ < 1000; o = o->proto) {
        u32 i;
        if (o->cls == C_PROXY) return proxy_has(vm, o, k);
        if ((o->cls == C_ARRAY || o->cls == C_ARGS) && (k == S_length || (str_is_index(k, &i) && i < o->u.arr.len))) return 1;
        if (o->cls == C_TARRAY && o->pad != TA_VIEW && str_is_index(k, &i)) return i < o->u.ta.len;
        if (prop_find(o, k)) return 1;
        if ((o->cls == C_FUNC && k == S_prototype && !(o->flags & OF_ARROW))) return 1;
    }
    return 0;
}

int del_prop(Vm *vm, Val ov, Val k)
{
    Str *ks;
    Obj *o;
    if (ov.t != T_OBJ) return 1;
    o = ov.u.o;
    ks = to_key(vm, k);
    if (ks && o->cls == C_PROXY) return proxy_del(vm, o, ks);
    if (!ks || o->sealed) return 0;
    if (o->cls == C_ARRAY) {
        u32 i;
        if (str_is_index(ks, &i)) { if (i < o->u.arr.len) o->u.arr.items->v[i] = v_undef(); return 1; }
    }
    if (o->cls == C_CLASSLIST || o->cls == C_STYLE) { dom_set_hook(vm, o, ks, v_str(S_empty)); return 1; }
    prop_del(o, ks);
    return 1;
}

/* The names for for-in and Object.keys: array indexes first, then the
 * properties in the order they were made.  all: the hidden ones too. */
Obj *own_keys(Vm *vm, Obj *o, int all)
{
    if (o->cls == C_PROXY) return proxy_keys(vm, o, all);
    Obj *r = arr_new(vm, 0);
    u32 i;
    if (!r) return 0;
    TEMP(vm, v_obj(r));
    if (o->cls == C_ARRAY || o->cls == C_ARGS) {
        for (i = 0; i < o->u.arr.len; i++) if (!arr_push(vm, r, v_str(num_to_str((f64)i, 10)))) break;
    } else if (o->cls == C_BOXED && IS_STR(o->u.boxed.v)) {
        for (i = 0; i < o->u.boxed.v.u.s->len; i++) if (!arr_push(vm, r, v_str(num_to_str((f64)i, 10)))) break;
    }
    if (o->props)
        for (i = 0; i < o->props->n; i++) {
            Prop *q = &o->props->p[i];
            if (!q->k) continue;
            if (!all && (q->flags & P_HIDDEN)) continue;
            if (!arr_push(vm, r, v_str(q->k))) break;
        }
    UNTEMP(vm, 1);
    return r;
}
