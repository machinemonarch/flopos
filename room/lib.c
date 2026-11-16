/* ===========================================================================
 * lib.c -- the built-in objects: Object, Function, Array, Number,
 * Boolean, Math, the errors, and the global functions.
 *
 * Each built-in is a C function with the same five arguments (the
 * machine, `this`, the arguments, how many, and a number that lets one C
 * function serve a family, such as Math.floor and Math.ceil).
 *
 * C code here that calls back into a script (forEach, map, sort) keeps
 * what it has made on vm->temps first, so the sweeper sees it.
 * ===========================================================================
 */
#include "js.h"

#define ARG(i) ((i) < argc ? args[i] : v_undef())
#define NATIVE(name) static Val name(Vm *vm, Val self, Val *args, int argc, u32 magic)
#define UNUSED (void)self; (void)args; (void)argc; (void)magic

Obj *make_proto_obj(Vm *vm, Obj *proto) { return obj_new(vm, proto, C_OBJECT); }

static Obj *ctor(Vm *vm, const char *name, NativeFn f, int nargs, Obj *proto)
{
    Obj *c = native_new(vm, name, f, nargs, 0);
    if (!c) return 0;
    c->flags |= OF_CTOR;
    if (proto) {
        prop_put(vm, c, S_prototype, v_obj(proto), P_HIDDEN);
        prop_put(vm, proto, S_constructor, v_obj(c), P_HIDDEN);
    }
    def_val(vm, vm->global, name, v_obj(c));
    return c;
}

static u32 to_len(Vm *vm, Val v)
{
    f64 d = to_num(vm, v);
    if (m_isnan(d) || d <= 0) return 0;
    if (d > 4294967295.0) return 0xFFFFFFFFu;
    return (u32)d;
}

static s32 rel_index(Vm *vm, Val v, s32 len, s32 dflt)
{
    f64 d;
    if (v.t == T_UNDEF) return dflt;
    d = m_trunc(to_num(vm, v));
    if (m_isnan(d)) d = 0;
    if (d < 0) { d += len; if (d < 0) d = 0; }
    if (d > len) d = len;
    return (s32)d;
}

/* ---- Object ---------------------------------------------------------------------- */
NATIVE(object_ctor)
{
    Val v = ARG(0);
    UNUSED;
    if (IS_NULLISH(v)) return v_obj(obj_new(vm, vm->object_proto, C_OBJECT));
    return v_obj(to_obj(vm, v));
}

NATIVE(object_keys)                      /* magic 0 keys, 1 values, 2 entries */
{
    Obj *o, *keys, *r;
    u32 i;
    UNUSED;
    o = to_obj(vm, ARG(0));
    if (!o) return v_undef();
    keys = own_keys(vm, o, 0);
    if (!keys || magic == 0) return keys ? v_obj(keys) : v_undef();
    TEMP(vm, v_obj(keys));
    r = arr_new(vm, 0);
    if (!r) { UNTEMP(vm, 1); return v_undef(); }
    TEMP(vm, v_obj(r));
    for (i = 0; i < keys->u.arr.len && !vm->throwing; i++) {
        Val kv = arr_get(keys, i), v = get_elem(vm, v_obj(o), kv);
        if (magic == 1) arr_push(vm, r, v);
        else {
            Obj *pair = arr_new(vm, 0);
            if (!pair) break;
            arr_push(vm, pair, kv); arr_push(vm, pair, v);
            arr_push(vm, r, v_obj(pair));
        }
    }
    UNTEMP(vm, 2);
    return v_obj(r);
}

NATIVE(object_assign)
{
    int i;
    Obj *t = to_obj(vm, ARG(0));
    UNUSED;
    if (!t) return v_undef();
    for (i = 1; i < argc && !vm->throwing; i++) {
        Obj *keys;
        u32 j;
        if (IS_NULLISH(args[i]) || args[i].t != T_OBJ) continue;
        keys = own_keys(vm, args[i].u.o, 0);
        if (!keys) break;
        TEMP(vm, v_obj(keys));
        for (j = 0; j < keys->u.arr.len && !vm->throwing; j++) {
            Val kv = arr_get(keys, j), v = get_elem(vm, args[i], kv);
            set_elem(vm, v_obj(t), kv, v);
        }
        UNTEMP(vm, 1);
    }
    return v_obj(t);
}

NATIVE(object_create)
{
    Val p = ARG(0);
    Obj *o;
    UNUSED;
    if (p.t != T_OBJ && p.t != T_NULL) return throw_err(vm, E_TYPE, "Object.create needs an object or null");
    o = obj_new(vm, p.t == T_OBJ ? p.u.o : 0, C_OBJECT);
    if (o && ARG(1).t == T_OBJ) {
        Val define = get_prop(vm, v_obj(vm->global), atom("Object"));
        Val dp = get_prop(vm, define, atom("defineProperties"));
        Val a[2]; a[0] = v_obj(o); a[1] = ARG(1);
        TEMP(vm, v_obj(o));
        vm_call(vm, dp, define, a, 2);
        UNTEMP(vm, 1);
    }
    return o ? v_obj(o) : v_undef();
}

static void define_one(Vm *vm, Obj *o, Str *k, Val desc)
{
    Val g, s, v;
    if (desc.t != T_OBJ) { throw_err(vm, E_TYPE, "a property description must be an object"); return; }
    g = get_prop(vm, desc, S_get);
    s = get_prop(vm, desc, S_set);
    if (vm->throwing) return;
    if (is_callable(g) || is_callable(s)) {
        Prop *q;
        if (!prop_put(vm, o, k, g, P_ACCESSOR | (truthy(get_prop(vm, desc, S_enumerable)) ? 0 : P_HIDDEN))) return;
        q = prop_find(o, k);
        if (q) q->set = s;
        return;
    }
    v = get_prop(vm, desc, S_value);
    if (vm->throwing) return;
    {
        Prop *q = prop_find(o, k);
        u32 flags = q ? q->flags & ~P_ACCESSOR : P_HIDDEN;
        if (has_prop(vm, desc.u.o, S_enumerable)) flags = truthy(get_prop(vm, desc, S_enumerable)) ? 0 : P_HIDDEN;
        if (o->cls == C_ARRAY) { set_prop(vm, v_obj(o), k, v); return; }
        prop_put(vm, o, k, v, flags);
    }
}

NATIVE(object_define)                    /* defineProperty, defineProperties */
{
    Val o = ARG(0);
    UNUSED;
    if (o.t != T_OBJ) return throw_err(vm, E_TYPE, "defineProperty needs an object");
    if (magic == 0) {
        Str *k = to_key(vm, ARG(1));
        if (!k) return v_undef();
        define_one(vm, o.u.o, k, ARG(2));
    } else if (ARG(1).t == T_OBJ) {
        Obj *keys = own_keys(vm, ARG(1).u.o, 0);
        u32 i;
        if (!keys) return v_undef();
        TEMP(vm, v_obj(keys));
        for (i = 0; i < keys->u.arr.len && !vm->throwing; i++) {
            Val kv = arr_get(keys, i);
            define_one(vm, o.u.o, kv.u.s, get_elem(vm, ARG(1), kv));
        }
        UNTEMP(vm, 1);
    }
    return o;
}

NATIVE(object_getproto)
{
    Obj *o = to_obj(vm, ARG(0));
    UNUSED;
    if (!o) return v_undef();
    return o->proto ? v_obj(o->proto) : v_null();
}

NATIVE(object_setproto)
{
    UNUSED;
    if (ARG(0).t == T_OBJ && (ARG(1).t == T_OBJ || ARG(1).t == T_NULL)) {
        Obj *p;
        for (p = ARG(1).t == T_OBJ ? ARG(1).u.o : 0; p; p = p->proto) if (p == ARG(0).u.o) return throw_err(vm, E_TYPE, "a prototype loop");
        ARG(0).u.o->proto = ARG(1).t == T_OBJ ? ARG(1).u.o : 0;
    }
    return ARG(0);
}

NATIVE(object_freeze)                    /* freeze, seal, preventExtensions */
{
    UNUSED;
    if (ARG(0).t == T_OBJ) ARG(0).u.o->sealed = 1;
    return ARG(0);
}

NATIVE(object_isfrozen) { UNUSED; return v_bool(ARG(0).t != T_OBJ || ARG(0).u.o->sealed); }

NATIVE(object_getownnames)
{
    Obj *o = to_obj(vm, ARG(0));
    UNUSED;
    if (!o) return v_undef();
    return v_obj(own_keys(vm, o, 1));
}

NATIVE(object_getowndesc)
{
    Obj *o = to_obj(vm, ARG(0)), *d;
    Str *k;
    Prop *q;
    UNUSED;
    if (!o) return v_undef();
    k = to_key(vm, ARG(1));
    if (!k) return v_undef();
    q = prop_find(o, k);
    if (!q) {
        if (o->cls == C_ARRAY && (k == S_length || has_prop(vm, o, k))) {
            d = obj_new(vm, vm->object_proto, C_OBJECT);
            if (!d) return v_undef();
            TEMP(vm, v_obj(d));
            prop_put(vm, d, S_value, get_prop(vm, v_obj(o), k), 0);
            prop_put(vm, d, S_writable, v_bool(1), 0);
            prop_put(vm, d, S_enumerable, v_bool(k != S_length), 0);
            prop_put(vm, d, S_configurable, v_bool(k != S_length), 0);
            UNTEMP(vm, 1);
            return v_obj(d);
        }
        return v_undef();
    }
    d = obj_new(vm, vm->object_proto, C_OBJECT);
    if (!d) return v_undef();
    if (q->flags & P_ACCESSOR) {
        prop_put(vm, d, S_get, q->v, 0);
        prop_put(vm, d, S_set, q->set, 0);
    } else {
        prop_put(vm, d, S_value, q->v, 0);
        prop_put(vm, d, S_writable, v_bool(!o->sealed), 0);
    }
    prop_put(vm, d, S_enumerable, v_bool(!(q->flags & P_HIDDEN)), 0);
    prop_put(vm, d, S_configurable, v_bool(!o->sealed), 0);
    return v_obj(d);
}

NATIVE(object_is)
{
    Val a = ARG(0), b = ARG(1);
    UNUSED;
    if (a.t == T_NUM && b.t == T_NUM) {
        if (m_isnan(a.u.n) && m_isnan(b.u.n)) return v_bool(1);
        if (a.u.n == 0 && b.u.n == 0) return v_bool(1 / a.u.n == 1 / b.u.n);
    }
    return v_bool(strict_eq(a, b));
}

NATIVE(object_fromentries)
{
    Obj *o = obj_new(vm, vm->object_proto, C_OBJECT), *it;
    Val src = ARG(0);
    UNUSED;
    if (!o) return v_undef();
    if (src.t != T_OBJ || src.u.o->cls != C_ARRAY) return v_obj(o);
    it = src.u.o;
    TEMP(vm, v_obj(o));
    {
        u32 i;
        for (i = 0; i < it->u.arr.len && !vm->throwing; i++) {
            Val pair = arr_get(it, i);
            Val k = get_elem(vm, pair, v_num(0)), v = get_elem(vm, pair, v_num(1));
            Str *ks = to_key(vm, k);
            if (ks) prop_put(vm, o, ks, v, 0);
        }
    }
    UNTEMP(vm, 1);
    return v_obj(o);
}

NATIVE(objproto_hasown)
{
    Obj *o;
    Str *k;
    UNUSED;
    o = to_obj(vm, magic ? ARG(0) : self);
    if (!o) return v_undef();
    k = to_key(vm, magic ? ARG(1) : ARG(0));
    if (!k) return v_undef();
    if (o->cls == C_ARRAY || o->cls == C_ARGS) {
        u32 i;
        if (k == S_length || (str_is_index(k, &i) && i < o->u.arr.len)) return v_bool(1);
    }
    if (o->cls == C_FUNC && k == S_prototype) return v_bool(1);
    return v_bool(prop_find(o, k) != 0);
}

NATIVE(objproto_isproto)
{
    Obj *o;
    int guard = 0;
    UNUSED;
    if (ARG(0).t != T_OBJ || self.t != T_OBJ) return v_bool(0);
    for (o = ARG(0).u.o->proto; o && guard++ < 1000; o = o->proto) if (o == self.u.o) return v_bool(1);
    return v_bool(0);
}

NATIVE(objproto_propenum)
{
    Obj *o = to_obj(vm, self);
    Str *k = to_key(vm, ARG(0));
    Prop *q;
    UNUSED;
    if (!o || !k) return v_bool(0);
    if (o->cls == C_ARRAY) { u32 i; if (str_is_index(k, &i)) return v_bool(i < o->u.arr.len); }
    q = prop_find(o, k);
    return v_bool(q && !(q->flags & P_HIDDEN));
}

NATIVE(objproto_tostring)
{
    UNUSED;
    if (self.t == T_UNDEF) return v_str(atom("[object Undefined]"));
    if (self.t == T_NULL) return v_str(atom("[object Null]"));
    if (self.t == T_OBJ) {
        switch (self.u.o->cls) {
        case C_ARRAY: return v_str(atom("[object Array]"));
        case C_FUNC: case C_NATIVE: case C_BOUND: return v_str(atom("[object Function]"));
        case C_ERROR: return v_str(atom("[object Error]"));
        case C_DATE: return v_str(atom("[object Date]"));
        case C_REGEXP: return v_str(atom("[object RegExp]"));
        case C_ARGS: return v_str(atom("[object Arguments]"));
        case C_PROMISE: return v_str(atom("[object Promise]"));
        }
    }
    if (self.t == T_STR) return v_str(atom("[object String]"));
    if (self.t == T_NUM) return v_str(atom("[object Number]"));
    if (self.t == T_BOOL) return v_str(atom("[object Boolean]"));
    return v_str(atom("[object Object]"));
}

NATIVE(objproto_valueof) { UNUSED; if (self.t == T_OBJ && self.u.o->cls == C_BOXED) return self.u.o->u.boxed.v; return self; }

/* ---- Function --------------------------------------------------------------------- */
NATIVE(function_ctor) { UNUSED; return throw_err(vm, E_EVAL, "making functions from text is not allowed here"); }

NATIVE(fn_call)
{
    UNUSED;
    if (!is_callable(self)) return throw_err(vm, E_TYPE, "call on something that is not a function");
    return vm_call(vm, self, ARG(0), argc > 1 ? args + 1 : 0, argc > 1 ? argc - 1 : 0);
}

NATIVE(fn_apply)
{
    Val list = ARG(1);
    u32 i, n, base = vm->sp;
    Val r;
    UNUSED;
    if (!is_callable(self)) return throw_err(vm, E_TYPE, "apply on something that is not a function");
    if (IS_NULLISH(list)) return vm_call(vm, self, ARG(0), 0, 0);
    if (list.t != T_OBJ) return throw_err(vm, E_TYPE, "apply needs a list of arguments");
    n = to_len(vm, get_prop(vm, list, S_length));
    if (vm->throwing) return v_undef();
    if (n > 2000 || vm->sp + n + 8 > VM_STACK) return throw_err(vm, E_RANGE, "too many arguments");
    for (i = 0; i < n; i++) {
        Val v = (list.u.o->cls == C_ARRAY || list.u.o->cls == C_ARGS) ? arr_get(list.u.o, i) : get_elem(vm, list, v_num(i));
        vm->stack[vm->sp++] = v;
    }
    r = vm_call(vm, self, ARG(0), &vm->stack[base], (int)n);
    vm->sp = base;
    return r;
}

NATIVE(fn_bind)
{
    Obj *b;
    int i;
    UNUSED;
    if (!is_callable(self)) return throw_err(vm, E_TYPE, "bind on something that is not a function");
    b = obj_new(vm, vm->function_proto, C_BOUND);
    if (!b) return v_undef();
    b->flags = OF_CALLABLE | OF_CTOR;
    b->u.bound.target = self.u.o;
    b->u.bound.self = ARG(0);
    if (argc > 1) {
        TEMP(vm, v_obj(b));
        b->u.bound.args = gc_alloc(sizeof(Vals) + (u32)(argc - 1) * sizeof(Val), K_VALS);
        UNTEMP(vm, 1);
        if (!b->u.bound.args) { vm->out_of_room = 1; return v_undef(); }
        b->u.bound.args->n = (u32)(argc - 1);
        for (i = 1; i < argc; i++) b->u.bound.args->v[i - 1] = args[i];
    }
    return v_obj(b);
}

NATIVE(fn_tostring)
{
    UNUSED;
    if (self.t == T_OBJ && self.u.o->cls == C_NATIVE) return v_str(atom("function () { [native code] }"));
    return v_str(atom("function () { [code] }"));
}

/* ---- Array ---------------------------------------------------------------------------- */
NATIVE(array_ctor)
{
    Obj *a;
    int i;
    UNUSED;
    if (argc == 1 && args[0].t == T_NUM) {
        f64 n = args[0].u.n;
        if (n < 0 || n != (f64)(u32)n || n > 0x100000) return throw_err(vm, E_RANGE, "bad array length");
        a = arr_new(vm, (u32)n);
        if (a) { u32 k; for (k = 0; k < (u32)n; k++) a->u.arr.items->v[k] = v_undef(); }
        return a ? v_obj(a) : v_undef();
    }
    a = arr_new(vm, 0);
    for (i = 0; a && i < argc; i++) arr_push(vm, a, args[i]);
    return a ? v_obj(a) : v_undef();
}

NATIVE(array_isarray) { UNUSED; return v_bool(ARG(0).t == T_OBJ && ARG(0).u.o->cls == C_ARRAY); }

NATIVE(array_of)
{
    Obj *a = arr_new(vm, 0);
    int i;
    UNUSED;
    for (i = 0; a && i < argc; i++) arr_push(vm, a, args[i]);
    return a ? v_obj(a) : v_undef();
}

NATIVE(array_from)
{
    Val src = ARG(0), fn = ARG(1);
    Obj *a = arr_new(vm, 0);
    UNUSED;
    if (!a) return v_undef();
    TEMP(vm, v_obj(a));
    if (src.t == T_STR) {
        u32 i;
        for (i = 0; i < src.u.s->len; i++) arr_push(vm, a, v_str(str_from16(&src.u.s->c[i], 1)));
    } else if (src.t == T_OBJ) {
        u32 n = to_len(vm, get_prop(vm, src, S_length)), i;
        for (i = 0; i < n && !vm->throwing; i++) arr_push(vm, a, get_elem(vm, src, v_num(i)));
    }
    if (is_callable(fn)) {
        u32 i;
        for (i = 0; i < a->u.arr.len && !vm->throwing; i++) {
            Val x[2]; x[0] = arr_get(a, i); x[1] = v_num(i);
            a->u.arr.items->v[i] = vm_call(vm, fn, v_undef(), x, 2);
        }
    }
    UNTEMP(vm, 1);
    return v_obj(a);
}

static Obj *this_array(Vm *vm, Val self)
{
    if (self.t == T_OBJ && (self.u.o->cls == C_ARRAY || self.u.o->cls == C_ARGS)) return self.u.o;
    throw_err(vm, E_TYPE, "an Array method on something that is not an array");
    return 0;
}

NATIVE(arr_push_n)
{
    Obj *a = this_array(vm, self);
    int i;
    UNUSED;
    if (!a) return v_undef();
    for (i = 0; i < argc; i++) if (!arr_push(vm, a, args[i])) break;
    return v_num(a->u.arr.len);
}

NATIVE(arr_pop)
{
    Obj *a = this_array(vm, self);
    Val v;
    UNUSED;
    if (!a || !a->u.arr.len) return v_undef();
    v = arr_get(a, a->u.arr.len - 1);
    a->u.arr.len--;
    return v;
}

NATIVE(arr_shift)
{
    Obj *a = this_array(vm, self);
    Val v;
    UNUSED;
    if (!a || !a->u.arr.len) return v_undef();
    v = arr_get(a, 0);
    memmove(a->u.arr.items->v, a->u.arr.items->v + 1, (a->u.arr.len - 1) * sizeof(Val));
    a->u.arr.len--;
    return v;
}

NATIVE(arr_unshift)
{
    Obj *a = this_array(vm, self);
    u32 n = (u32)argc, len, i;
    UNUSED;
    if (!a) return v_undef();
    len = a->u.arr.len;
    if (!arr_set_len(vm, a, len + n)) return v_undef();
    memmove(a->u.arr.items->v + n, a->u.arr.items->v, len * sizeof(Val));
    for (i = 0; i < n; i++) a->u.arr.items->v[i] = args[i];
    return v_num(a->u.arr.len);
}

NATIVE(arr_slice)
{
    Obj *a = this_array(vm, self), *r;
    s32 len, from, to, i;
    UNUSED;
    if (!a) return v_undef();
    len = (s32)a->u.arr.len;
    from = rel_index(vm, ARG(0), len, 0);
    to = rel_index(vm, ARG(1), len, len);
    r = arr_new(vm, 0);
    for (i = from; r && i < to; i++) arr_push(vm, r, arr_get(a, (u32)i));
    return r ? v_obj(r) : v_undef();
}

NATIVE(arr_splice)
{
    Obj *a = this_array(vm, self), *r;
    s32 len, start, del, add, i;
    UNUSED;
    if (!a) return v_undef();
    len = (s32)a->u.arr.len;
    start = rel_index(vm, ARG(0), len, 0);
    if (argc < 2) del = len - start;
    else { f64 d = m_trunc(to_num(vm, ARG(1))); del = m_isnan(d) || d < 0 ? 0 : d > len - start ? len - start : (s32)d; }
    add = argc > 2 ? argc - 2 : 0;
    r = arr_new(vm, 0);
    if (!r) return v_undef();
    for (i = 0; i < del; i++) arr_push(vm, r, arr_get(a, (u32)(start + i)));
    TEMP(vm, v_obj(r));
    if (add != del) {
        if (add > del && !arr_set_len(vm, a, (u32)(len + add - del))) { UNTEMP(vm, 1); return v_undef(); }
        memmove(a->u.arr.items->v + start + add, a->u.arr.items->v + start + del, (u32)(len - start - del) * sizeof(Val));
        a->u.arr.len = (u32)(len + add - del);
    }
    for (i = 0; i < add; i++) a->u.arr.items->v[start + i] = args[2 + i];
    UNTEMP(vm, 1);
    return v_obj(r);
}

NATIVE(arr_concat)
{
    Obj *a = arr_new(vm, 0);
    int i;
    UNUSED;
    if (!a) return v_undef();
    TEMP(vm, v_obj(a));
    for (i = -1; i < argc; i++) {
        Val v = i < 0 ? self : args[i];
        if (v.t == T_OBJ && v.u.o->cls == C_ARRAY) {
            u32 k;
            for (k = 0; k < v.u.o->u.arr.len; k++) arr_push(vm, a, arr_get(v.u.o, k));
        } else arr_push(vm, a, v);
    }
    UNTEMP(vm, 1);
    return v_obj(a);
}

Str *array_join(Vm *vm, Obj *a, Str *sep, int depth)
{
    u32 i, n = a->u.arr.len, total = 0;
    Obj *parts;
    Str *r;
    if (depth > 20) return S_empty;
    parts = arr_new(vm, n);
    if (!parts) return S_empty;
    TEMP(vm, v_obj(parts));
    for (i = 0; i < n && !vm->throwing; i++) {
        Val v = arr_get(a, i);
        Str *s;
        if (IS_NULLISH(v)) s = S_empty;
        else if (v.t == T_OBJ && v.u.o->cls == C_ARRAY) s = array_join(vm, v.u.o, atom(","), depth + 1);
        else s = to_str(vm, v);
        parts->u.arr.items->v[i] = v_str(s);
        total += s->len;
    }
    total += n ? (n - 1) * sep->len : 0;
    r = str_new(total);
    if (r) {
        u32 at = 0;
        for (i = 0; i < n; i++) {
            Str *s = parts->u.arr.items->v[i].u.s;
            if (i) { memcpy(r->c + at, sep->c, sep->len * 2); at += sep->len; }
            if (s) { memcpy(r->c + at, s->c, s->len * 2); at += s->len; }
        }
    } else vm->out_of_room = 1;
    UNTEMP(vm, 1);
    return r ? r : S_empty;
}

NATIVE(arr_join)
{
    Obj *a;
    Str *sep;
    UNUSED;
    if (self.t != T_OBJ) return v_str(S_empty);
    a = self.u.o;
    if (a->cls != C_ARRAY && a->cls != C_ARGS) return v_str(atom("[object Object]"));
    sep = ARG(0).t == T_UNDEF ? atom(",") : to_str(vm, ARG(0));
    return v_str(array_join(vm, a, sep, 0));
}

NATIVE(arr_reverse)
{
    Obj *a = this_array(vm, self);
    u32 i, n;
    UNUSED;
    if (!a) return v_undef();
    n = a->u.arr.len;
    for (i = 0; i < n / 2; i++) {
        Val t = a->u.arr.items->v[i];
        a->u.arr.items->v[i] = a->u.arr.items->v[n - 1 - i];
        a->u.arr.items->v[n - 1 - i] = t;
    }
    return self;
}

NATIVE(arr_indexof)                       /* 0 indexOf, 1 lastIndexOf, 2 includes */
{
    Obj *a = this_array(vm, self);
    s32 len, i;
    Val x = ARG(0);
    UNUSED;
    if (!a) return v_undef();
    len = (s32)a->u.arr.len;
    if (magic == 1) {
        for (i = argc > 1 ? rel_index(vm, ARG(1), len, len - 1) : len - 1; i >= 0; i--)
            if (i < len && strict_eq(arr_get(a, (u32)i), x)) return v_num(i);
        return v_num(-1);
    }
    for (i = rel_index(vm, ARG(1), len, 0); i < len; i++) {
        Val v = arr_get(a, (u32)i);
        if (strict_eq(v, x) || (magic == 2 && v.t == T_NUM && x.t == T_NUM && m_isnan(v.u.n) && m_isnan(x.u.n)))
            return magic == 2 ? v_bool(1) : v_num(i);
    }
    return magic == 2 ? v_bool(0) : v_num(-1);
}

/* forEach 0, map 1, filter 2, some 3, every 4, find 5, findIndex 6,
 * findLast 7, findLastIndex 8, flatMap 9 */
NATIVE(arr_iter)
{
    Obj *a = this_array(vm, self), *r = 0;
    Val fn = ARG(0), that = ARG(1);
    u32 i, n;
    UNUSED;
    if (!a) return v_undef();
    if (!is_callable(fn)) return throw_err(vm, E_TYPE, "an Array method needs a function");
    n = a->u.arr.len;
    if (magic == 1 || magic == 2 || magic == 9) {
        r = arr_new(vm, 0);
        if (!r) return v_undef();
        TEMP(vm, v_obj(r));
    }
    for (i = 0; i < n && i < a->u.arr.len; i++) {
        u32 ix = (magic == 7 || magic == 8) ? n - 1 - i : i;
        Val x[3], v;
        x[0] = arr_get(a, ix); x[1] = v_num(ix); x[2] = self;
        v = vm_call(vm, fn, that, x, 3);
        if (vm->throwing) break;
        switch (magic) {
        case 1: arr_push(vm, r, v); break;
        case 2: if (truthy(v)) arr_push(vm, r, x[0]); break;
        case 3: if (truthy(v)) return v_bool(1); break;
        case 4: if (!truthy(v)) return v_bool(0); break;
        case 5: case 7: if (truthy(v)) return x[0]; break;
        case 6: case 8: if (truthy(v)) return v_num(ix); break;
        case 9:
            if (v.t == T_OBJ && v.u.o->cls == C_ARRAY) { u32 k; for (k = 0; k < v.u.o->u.arr.len; k++) arr_push(vm, r, arr_get(v.u.o, k)); }
            else arr_push(vm, r, v);
            break;
        }
    }
    if (r) UNTEMP(vm, 1);
    switch (magic) {
    case 1: case 2: case 9: return v_obj(r);
    case 3: return v_bool(0);
    case 4: return v_bool(1);
    case 6: case 8: return v_num(-1);
    default: return v_undef();
    }
}

NATIVE(arr_reduce)                        /* 0 reduce, 1 reduceRight */
{
    Obj *a = this_array(vm, self);
    Val fn = ARG(0), acc;
    s32 n, i, step;
    int have = argc > 1;
    UNUSED;
    if (!a) return v_undef();
    if (!is_callable(fn)) return throw_err(vm, E_TYPE, "reduce needs a function");
    n = (s32)a->u.arr.len;
    i = magic ? n - 1 : 0;
    step = magic ? -1 : 1;
    acc = ARG(1);
    if (!have) {
        if (!n) return throw_err(vm, E_TYPE, "reduce of an empty array with no first value");
        acc = arr_get(a, (u32)i); i += step;
    }
    for (; i >= 0 && i < n && !vm->throwing; i += step) {
        Val x[4];
        x[0] = acc; x[1] = arr_get(a, (u32)i); x[2] = v_num(i); x[3] = self;
        TEMP(vm, acc);
        acc = vm_call(vm, fn, v_undef(), x, 4);
        UNTEMP(vm, 1);
    }
    return acc;
}

static int sort_cmp(Vm *vm, Val fn, Val a, Val b)
{
    if (a.t == T_UNDEF) return b.t == T_UNDEF ? 0 : 1;
    if (b.t == T_UNDEF) return -1;
    if (is_callable(fn)) {
        Val x[2], r;
        f64 d;
        x[0] = a; x[1] = b;
        r = vm_call(vm, fn, v_undef(), x, 2);
        d = to_num(vm, r);
        return d < 0 ? -1 : d > 0 ? 1 : 0;
    }
    {
        Str *sa, *sb;
        if (a.t == T_STR && b.t == T_STR) return str_cmp(a.u.s, b.u.s);
        sa = to_str(vm, a);
        TEMP(vm, v_str(sa));
        sb = to_str(vm, b);
        UNTEMP(vm, 1);
        return str_cmp(sa, sb);
    }
}

NATIVE(arr_sort)
{
    Obj *a = this_array(vm, self);
    Val fn = ARG(0);
    u32 n, i, w;
    Vals *tmp;
    UNUSED;
    if (!a) return v_undef();
    n = a->u.arr.len;
    if (n < 2) return self;
    /* a merge sort: stable, as JS requires, and never worse than n log n */
    tmp = gc_alloc(sizeof(Vals) + n * sizeof(Val), K_VALS);
    if (!tmp) { vm->out_of_room = 1; return v_undef(); }
    tmp->n = n;
    {
        /* keep tmp alive: hold it in a throwaway array */
        Obj *hold = arr_new(vm, 0);
        if (!hold) return v_undef();
        hold->u.arr.items = tmp;
        hold->u.arr.len = 0;
        TEMP(vm, v_obj(hold));
    }
    for (w = 1; w < n && !vm->throwing; w *= 2) {
        for (i = 0; i < n && !vm->throwing; i += 2 * w) {
            u32 lo = i, mid = i + w < n ? i + w : n, hi = i + 2 * w < n ? i + 2 * w : n, l = lo, r = mid, o = lo;
            while (l < mid && r < hi && !vm->throwing) {
                if (sort_cmp(vm, fn, a->u.arr.items->v[r], a->u.arr.items->v[l]) < 0) tmp->v[o++] = a->u.arr.items->v[r++];
                else tmp->v[o++] = a->u.arr.items->v[l++];
            }
            while (l < mid) tmp->v[o++] = a->u.arr.items->v[l++];
            while (r < hi) tmp->v[o++] = a->u.arr.items->v[r++];
        }
        if (a->u.arr.len != n) break;     /* the comparer changed the array */
        memcpy(a->u.arr.items->v, tmp->v, n * sizeof(Val));
    }
    UNTEMP(vm, 1);
    return self;
}

NATIVE(arr_fill)
{
    Obj *a = this_array(vm, self);
    s32 len, from, to, i;
    UNUSED;
    if (!a) return v_undef();
    len = (s32)a->u.arr.len;
    from = rel_index(vm, ARG(1), len, 0);
    to = rel_index(vm, ARG(2), len, len);
    for (i = from; i < to; i++) a->u.arr.items->v[i] = ARG(0);
    return self;
}

static void flat_into(Vm *vm, Obj *r, Obj *a, int depth)
{
    u32 i;
    for (i = 0; i < a->u.arr.len && !vm->throwing; i++) {
        Val v = arr_get(a, i);
        if (depth > 0 && v.t == T_OBJ && v.u.o->cls == C_ARRAY) flat_into(vm, r, v.u.o, depth - 1);
        else arr_push(vm, r, v);
    }
}

NATIVE(arr_flat)
{
    Obj *a = this_array(vm, self), *r;
    int depth = ARG(0).t == T_UNDEF ? 1 : (int)to_num(vm, ARG(0));
    UNUSED;
    if (!a) return v_undef();
    r = arr_new(vm, 0);
    if (!r) return v_undef();
    TEMP(vm, v_obj(r));
    flat_into(vm, r, a, depth > 20 ? 20 : depth);
    UNTEMP(vm, 1);
    return v_obj(r);
}

NATIVE(arr_at)
{
    Obj *a = this_array(vm, self);
    f64 d;
    s32 len;
    UNUSED;
    if (!a) return v_undef();
    len = (s32)a->u.arr.len;
    d = m_trunc(to_num(vm, ARG(0)));
    if (m_isnan(d)) d = 0;
    if (d < 0) d += len;
    if (d < 0 || d >= len) return v_undef();
    return arr_get(a, (u32)d);
}

NATIVE(arr_keys)                          /* 0 keys, 1 values, 2 entries: as arrays */
{
    Obj *a = this_array(vm, self), *r;
    u32 i;
    UNUSED;
    if (!a) return v_undef();
    r = arr_new(vm, 0);
    if (!r) return v_undef();
    TEMP(vm, v_obj(r));
    for (i = 0; i < a->u.arr.len; i++) {
        if (magic == 0) arr_push(vm, r, v_num(i));
        else if (magic == 1) arr_push(vm, r, arr_get(a, i));
        else {
            Obj *p = arr_new(vm, 0);
            if (!p) break;
            arr_push(vm, p, v_num(i)); arr_push(vm, p, arr_get(a, i));
            arr_push(vm, r, v_obj(p));
        }
    }
    UNTEMP(vm, 1);
    return v_obj(r);
}

/* ---- Boolean, Number ------------------------------------------------------------- */
NATIVE(boolean_ctor)
{
    Val v = v_bool(truthy(ARG(0)));
    UNUSED;
    if (vm->in_new) { Obj *o = obj_new(vm, vm->boolean_proto, C_BOXED); if (o) o->u.boxed.v = v; return o ? v_obj(o) : v_undef(); }
    return v;
}

static int this_num(Vm *vm, Val self, f64 *out)
{
    if (self.t == T_NUM) { *out = self.u.n; return 1; }
    if (self.t == T_OBJ && self.u.o->cls == C_BOXED && self.u.o->u.boxed.v.t == T_NUM) { *out = self.u.o->u.boxed.v.u.n; return 1; }
    throw_err(vm, E_TYPE, "a Number method on something that is not a number");
    return 0;
}

NATIVE(bool_tostring)
{
    Val v = self;
    UNUSED;
    if (v.t == T_OBJ && v.u.o->cls == C_BOXED) v = v.u.o->u.boxed.v;
    if (magic) return v;
    return v_str(truthy(v) ? S_true : S_false);
}

NATIVE(number_ctor)
{
    Val v = v_num(argc ? to_num(vm, args[0]) : 0);
    UNUSED;
    if (vm->in_new) { Obj *o = obj_new(vm, vm->number_proto, C_BOXED); if (o) o->u.boxed.v = v; return o ? v_obj(o) : v_undef(); }
    return v;
}

NATIVE(num_tostring)
{
    f64 v;
    int radix = 10;
    UNUSED;
    if (!this_num(vm, self, &v)) return v_undef();
    if (magic) return v_num(v);           /* valueOf */
    if (ARG(0).t != T_UNDEF) radix = (int)to_num(vm, ARG(0));
    if (radix < 2 || radix > 36) return throw_err(vm, E_RANGE, "toString() radix must be 2 to 36");
    return v_str(num_to_str(v, radix));
}

NATIVE(num_tofixed)
{
    f64 v, scale = 1;
    int digits = (int)to_num(vm, ARG(0)), i, n = 0, neg;
    char b[400];
    u64 whole, frac;
    UNUSED;
    if (!this_num(vm, self, &v)) return v_undef();
    if (digits < 0 || digits > 20) return throw_err(vm, E_RANGE, "toFixed() digits must be 0 to 20");
    if (m_isnan(v) || v >= 1e21 || v <= -1e21) return v_str(num_to_str(v, 10));
    neg = v < 0;
    if (neg) v = -v;
    for (i = 0; i < digits; i++) scale *= 10;
    {
        long double x = (long double)v * scale + 0.5L;
        u64 all = (u64)x;
        u64 sc = (u64)scale;
        whole = all / sc; frac = all % sc;
    }
    if (neg && (whole || frac)) b[n++] = '-';
    {
        char t[24]; int k = 0;
        do { t[k++] = (char)('0' + whole % 10); whole /= 10; } while (whole);
        while (k) b[n++] = t[--k];
    }
    if (digits) {
        char t[24]; int k;
        b[n++] = '.';
        for (k = digits - 1; k >= 0; k--) { t[k] = (char)('0' + frac % 10); frac /= 10; }
        for (k = 0; k < digits; k++) b[n++] = t[k];
    }
    b[n] = 0;
    return v_str(str_ascii(b));
}

NATIVE(num_toprecision)
{
    f64 v;
    UNUSED;
    if (!this_num(vm, self, &v)) return v_undef();
    if (ARG(0).t == T_UNDEF) return v_str(num_to_str(v, 10));
    {
        int p = (int)to_num(vm, ARG(0));
        f64 a = v < 0 ? -v : v, s = 1;
        int e = 0;
        if (p < 1 || p > 21) return throw_err(vm, E_RANGE, "toPrecision() must be 1 to 21");
        if (a == 0) { Val d = v_num(p - 1); return num_tofixed(vm, self, &d, 1, 0); }
        while (a >= 10) { a /= 10; e++; }
        while (a < 1) { a *= 10; e--; }
        if (e < -6 || e >= p) return v_str(num_to_str(v, 10));
        {
            Val d = v_num(p - 1 - e < 0 ? 0 : p - 1 - e);
            (void)s;
            return num_tofixed(vm, self, &d, 1, 0);
        }
    }
}

NATIVE(num_tolocale)
{
    f64 v;
    char b[64], o[96];
    int n, i, j = 0, dot, start;
    UNUSED;
    if (!this_num(vm, self, &v)) return v_undef();
    n = num_fmt(v, b);
    for (dot = 0; dot < n && b[dot] != '.' && b[dot] != 'e'; dot++) { }
    start = b[0] == '-' ? 1 : 0;
    if (start) o[j++] = '-';
    for (i = start; i < dot; i++) {
        o[j++] = b[i];
        if ((dot - i - 1) % 3 == 0 && i < dot - 1) o[j++] = ',';
    }
    for (; i < n && j < 90; i++) o[j++] = b[i];
    o[j] = 0;
    return v_str(str_ascii(o));
}

NATIVE(number_is)                         /* 0 isNaN, 1 isFinite, 2 isInteger, 3 isSafeInteger */
{
    Val v = ARG(0);
    UNUSED;
    if (v.t != T_NUM) return v_bool(0);
    switch (magic) {
    case 0: return v_bool(m_isnan(v.u.n));
    case 1: return v_bool(!m_isnan(v.u.n) && !m_isinf(v.u.n));
    case 2: return v_bool(!m_isnan(v.u.n) && !m_isinf(v.u.n) && m_trunc(v.u.n) == v.u.n);
    default: return v_bool(!m_isnan(v.u.n) && m_trunc(v.u.n) == v.u.n && v.u.n <= 9007199254740991.0 && v.u.n >= -9007199254740991.0);
    }
}

/* ---- the global functions ----------------------------------------------------------- */
NATIVE(g_isnan) { f64 d = to_num(vm, ARG(0)); UNUSED; return v_bool(magic ? !m_isnan(d) && !m_isinf(d) : m_isnan(d)); }

NATIVE(g_parsefloat)
{
    Str *s = to_str(vm, ARG(0));
    u32 used;
    f64 v;
    UNUSED;
    v = num_parse_prefix(s->c, s->len, &used);
    return v_num(used ? v : m_nan());
}

NATIVE(g_parseint)
{
    Str *s = to_str(vm, ARG(0));
    int radix = ARG(1).t == T_UNDEF ? 0 : (int)to_int32(to_num(vm, ARG(1)));
    u32 i = 0, any = 0;
    int neg = 0;
    long double r = 0;
    UNUSED;
    while (i < s->len && (s->c[i] == ' ' || s->c[i] == '\t' || s->c[i] == '\n' || s->c[i] == '\r' || s->c[i] == 0xA0)) i++;
    if (i < s->len && (s->c[i] == '+' || s->c[i] == '-')) neg = s->c[i++] == '-';
    if ((radix == 0 || radix == 16) && i + 1 < s->len && s->c[i] == '0' && (s->c[i + 1] | 32) == 'x') { radix = 16; i += 2; }
    if (radix == 0) radix = 10;
    if (radix < 2 || radix > 36) return v_num(m_nan());
    for (; i < s->len; i++) {
        u32 c = s->c[i];
        int d = c >= '0' && c <= '9' ? (int)(c - '0') : (c | 32) >= 'a' && (c | 32) <= 'z' ? (int)((c | 32) - 'a' + 10) : 99;
        if (d >= radix) break;
        r = r * radix + d;
        any = 1;
    }
    if (!any) return v_num(m_nan());
    return v_num((f64)(neg ? -r : r));
}

static int uri_keep(u32 c, int magic)
{
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return 1;
    if (c == '-' || c == '_' || c == '.' || c == '!' || c == '~' || c == '*' || c == '\'' || c == '(' || c == ')') return 1;
    if (magic == 1 && (c == ';' || c == '/' || c == '?' || c == ':' || c == '@' || c == '&' || c == '=' || c == '+' || c == '$' || c == ',' || c == '#')) return 1;
    return 0;
}

NATIVE(g_encode)                          /* 0 encodeURIComponent, 1 encodeURI */
{
    Str *s = to_str(vm, ARG(0)), *r;
    char *buf;
    u32 i, n = 0, cap;
    static const char hex[] = "0123456789ABCDEF";
    UNUSED;
    cap = s->len * 12 + 1;
    if (cap > 0x200000) return throw_err(vm, E_RANGE, "too long to encode");
    buf = gc_alloc(cap + 8, K_BYTES);
    if (!buf) { vm->out_of_room = 1; return v_undef(); }
    {
        char u8b[8];
        Str one;
        (void)one;
        for (i = 0; i < s->len; i++) {
            u32 c = s->c[i], k, m;
            if (uri_keep(c, (int)magic)) { buf[8 + n++] = (char)c; continue; }
            {
                Str *t = str_from16(&s->c[i], (c >= 0xD800 && c < 0xDC00 && i + 1 < s->len) ? 2 : 1);
                if (!t) break;
                if (t->len == 2) i++;
                m = str_to_utf8(t, u8b, 8);
            }
            for (k = 0; k < m; k++) { buf[8 + n++] = '%'; buf[8 + n++] = hex[(u8)u8b[k] >> 4]; buf[8 + n++] = hex[(u8)u8b[k] & 15]; }
        }
    }
    r = str_utf8(buf + 8, n);
    return r ? v_str(r) : v_undef();
}

static int hexd(u32 c) { return c >= '0' && c <= '9' ? (int)(c - '0') : (c | 32) >= 'a' && (c | 32) <= 'f' ? (int)((c | 32) - 'a' + 10) : -1; }

NATIVE(g_decode)                          /* 0 decodeURIComponent, 1 decodeURI, 2 unescape */
{
    Str *s = to_str(vm, ARG(0)), *r;
    char *buf;
    u32 i, n = 0;
    UNUSED;
    buf = gc_alloc(s->len * 3 + 16, K_BYTES);
    if (!buf) { vm->out_of_room = 1; return v_undef(); }
    buf += 8;
    for (i = 0; i < s->len; i++) {
        u32 c = s->c[i];
        if (c == '%' && i + 2 < s->len && hexd(s->c[i + 1]) >= 0 && hexd(s->c[i + 2]) >= 0) {
            buf[n++] = (char)(hexd(s->c[i + 1]) * 16 + hexd(s->c[i + 2]));
            i += 2;
        } else if (c < 0x80) buf[n++] = (char)c;
        else {
            Str *t = str_from16(&s->c[i], 1);
            if (t) n += str_to_utf8(t, buf + n, 3);
        }
    }
    r = str_utf8(buf, n);
    return r ? v_str(r) : v_undef();
}

/* ---- Math ------------------------------------------------------------------------------ */
static u32 s_rng[4] = { 0x12345678u, 0x9ABCDEF0u, 0x0F1E2D3Cu, 0x4B5A6978u };
void math_seed(u32 a, u32 b) { s_rng[0] ^= a; s_rng[1] ^= b; s_rng[2] ^= a * 2654435761u; s_rng[3] ^= b * 40503u + 1; }

static u32 rng(void)                      /* xoshiro128** */
{
    u32 r = s_rng[1] * 5, t = s_rng[1] << 9;
    r = ((r << 7) | (r >> 25)) * 9;
    s_rng[2] ^= s_rng[0]; s_rng[3] ^= s_rng[1]; s_rng[1] ^= s_rng[2]; s_rng[0] ^= s_rng[3];
    s_rng[2] ^= t;
    s_rng[3] = (s_rng[3] << 11) | (s_rng[3] >> 21);
    return r;
}

NATIVE(math1)
{
    f64 x = to_num(vm, ARG(0));
    UNUSED;
    switch (magic) {
    case 0: return v_num(m_floor(x));
    case 1: return v_num(m_ceil(x));
    case 2: return v_num(m_round(x));
    case 3: return v_num(x < 0 ? -x : x == 0 ? 0 : x);
    case 4: return v_num(m_sqrt(x));
    case 5: return v_num(m_sin(x));
    case 6: return v_num(m_cos(x));
    case 7: return v_num(m_tan(x));
    case 8: return v_num(m_atan2(x, 1));
    case 9: return v_num(m_exp(x));
    case 10: return v_num(m_log(x));
    case 11: return v_num(m_trunc(x));
    case 12: return v_num(m_isnan(x) ? x : x > 0 ? 1 : x < 0 ? -1 : x);
    case 13: return v_num(m_log(x) / 0.69314718055994530942);   /* log2  */
    case 14: return v_num(m_log(x) / 2.30258509299404568402);   /* log10 */
    case 15: return v_num(m_atan2(x, m_sqrt(1 - x * x)));       /* asin  */
    case 16: return v_num(m_atan2(m_sqrt(1 - x * x), x));       /* acos  */
    case 17: return v_num(m_pow(x, 1.0 / 3));                   /* cbrt (positive) */
    case 18: return v_num((f64)(float)x);                       /* fround */
    case 19: { f64 e = m_exp(x); return v_num((e - 1 / e) / 2); } /* sinh */
    case 20: { f64 e = m_exp(x); return v_num((e + 1 / e) / 2); } /* cosh */
    case 21: { f64 e = m_exp(2 * x); return v_num((e - 1) / (e + 1)); } /* tanh */
    case 22: return v_num(m_log(1 + x));                        /* log1p */
    case 23: return v_num(m_exp(x) - 1);                        /* expm1 */
    }
    return v_num(m_nan());
}

NATIVE(math_minmax)                       /* 0 min, 1 max */
{
    f64 r = magic ? -m_inf() : m_inf();
    int i;
    UNUSED;
    for (i = 0; i < argc; i++) {
        f64 x = to_num(vm, args[i]);
        if (m_isnan(x)) return v_num(m_nan());
        if (magic ? x > r : x < r) r = x;
    }
    return v_num(r);
}

NATIVE(math_pow) { f64 a = to_num(vm, ARG(0)), b = to_num(vm, ARG(1)); UNUSED; return v_num(m_pow(a, b)); }
NATIVE(math_atan2) { f64 a = to_num(vm, ARG(0)), b = to_num(vm, ARG(1)); UNUSED; return v_num(m_atan2(a, b)); }
NATIVE(math_random)
{
    f64 a = (f64)(rng() >> 5), b = (f64)(rng() >> 6);
    UNUSED;
    return v_num((a * 67108864.0 + b) / 9007199254740992.0);
}
NATIVE(math_hypot)
{
    f64 s = 0;
    int i;
    UNUSED;
    for (i = 0; i < argc; i++) { f64 x = to_num(vm, args[i]); s += x * x; }
    return v_num(m_sqrt(s));
}
NATIVE(math_imul) { UNUSED; return v_num((s32)(to_uint32(to_num(vm, ARG(0))) * to_uint32(to_num(vm, ARG(1))))); }
NATIVE(math_clz32)
{
    u32 x = to_uint32(to_num(vm, ARG(0))), n = 0;
    UNUSED;
    if (!x) return v_num(32);
    while (!(x & 0x80000000u)) { x <<= 1; n++; }
    return v_num(n);
}

/* ---- errors ------------------------------------------------------------------------------- */
NATIVE(error_ctor)
{
    Obj *e;
    Str *msg = 0;
    UNUSED;
    if (ARG(0).t != T_UNDEF) msg = to_str(vm, ARG(0));
    /* class X extends Error: super(msg) calls this with the new object */
    if (!vm->in_new && self.t == T_OBJ && self.u.o != vm->global) {
        if (msg) prop_put(vm, self.u.o, S_message, v_str(msg), P_HIDDEN);
        self.u.o->cls = C_ERROR;
        return v_undef();
    }
    e = make_err(vm, (int)magic, msg);
    return e ? v_obj(e) : v_undef();
}

NATIVE(error_tostring)
{
    Val n, m;
    Str *ns, *ms;
    UNUSED;
    if (self.t != T_OBJ) return throw_err(vm, E_TYPE, "Error.prototype.toString on something that is not an object");
    n = get_prop(vm, self, S_name);
    m = get_prop(vm, self, S_message);
    ns = n.t == T_UNDEF ? atom("Error") : to_str(vm, n);
    TEMP(vm, v_str(ns));
    ms = m.t == T_UNDEF ? S_empty : to_str(vm, m);
    UNTEMP(vm, 1);
    if (!ms->len) return v_str(ns);
    if (!ns->len) return v_str(ms);
    {
        Str *r = str_concat(ns, str_ascii(": "));
        return v_str(r ? str_concat(r, ms) : ms);
    }
}

/* ---- the global object, and the order things are made in ---------------------------------- */
void lib_init(Vm *vm)
{
    Obj *g, *o, *c;
    static const char *errs[6] = { "Error", "TypeError", "RangeError", "SyntaxError", "ReferenceError", "EvalError" };
    int i;

    vm->object_proto = obj_new(vm, 0, C_OBJECT);
    vm->function_proto = obj_new(vm, vm->object_proto, C_NATIVE);
    vm->function_proto->flags = OF_CALLABLE;
    vm->function_proto->u.nat.f = objproto_valueof;   /* calling it gives undefined-ish */
    vm->function_proto->u.nat.name = S_empty;
    vm->array_proto = obj_new(vm, vm->object_proto, C_ARRAY);
    vm->array_proto->u.arr.items = 0;
    vm->string_proto = obj_new(vm, vm->object_proto, C_BOXED);
    vm->string_proto->u.boxed.v = v_str(S_empty);
    vm->number_proto = obj_new(vm, vm->object_proto, C_BOXED);
    vm->number_proto->u.boxed.v = v_num(0);
    vm->boolean_proto = obj_new(vm, vm->object_proto, C_BOXED);
    vm->boolean_proto->u.boxed.v = v_bool(0);
    vm->error_proto = obj_new(vm, vm->object_proto, C_OBJECT);
    vm->regexp_proto = obj_new(vm, vm->object_proto, C_OBJECT);
    vm->date_proto = obj_new(vm, vm->object_proto, C_OBJECT);
    vm->promise_proto = obj_new(vm, vm->object_proto, C_OBJECT);
    vm->iter_proto = obj_new(vm, vm->object_proto, C_OBJECT);
    g = vm->global = obj_new(vm, vm->object_proto, C_OBJECT);

    def_val(vm, g, "globalThis", v_obj(g));
    def_val(vm, g, "window", v_obj(g));
    def_val(vm, g, "self", v_obj(g));
    def_val(vm, g, "undefined", v_undef());
    def_val(vm, g, "NaN", v_num(m_nan()));
    def_val(vm, g, "Infinity", v_num(m_inf()));

    /* Object */
    o = vm->object_proto;
    c = ctor(vm, "Object", object_ctor, 1, o);
    def_native(vm, c, "keys", object_keys, 1, 0);
    def_native(vm, c, "values", object_keys, 1, 1);
    def_native(vm, c, "entries", object_keys, 1, 2);
    def_native(vm, c, "assign", object_assign, 2, 0);
    def_native(vm, c, "create", object_create, 2, 0);
    def_native(vm, c, "defineProperty", object_define, 3, 0);
    def_native(vm, c, "defineProperties", object_define, 2, 1);
    def_native(vm, c, "getPrototypeOf", object_getproto, 1, 0);
    def_native(vm, c, "setPrototypeOf", object_setproto, 2, 0);
    def_native(vm, c, "freeze", object_freeze, 1, 0);
    def_native(vm, c, "seal", object_freeze, 1, 0);
    def_native(vm, c, "preventExtensions", object_freeze, 1, 0);
    def_native(vm, c, "isFrozen", object_isfrozen, 1, 0);
    def_native(vm, c, "isSealed", object_isfrozen, 1, 0);
    def_native(vm, c, "getOwnPropertyNames", object_getownnames, 1, 0);
    def_native(vm, c, "getOwnPropertyDescriptor", object_getowndesc, 2, 0);
    def_native(vm, c, "is", object_is, 2, 0);
    def_native(vm, c, "fromEntries", object_fromentries, 1, 0);
    def_native(vm, c, "hasOwn", objproto_hasown, 2, 1);
    def_native(vm, o, "hasOwnProperty", objproto_hasown, 1, 0);
    def_native(vm, o, "isPrototypeOf", objproto_isproto, 1, 0);
    def_native(vm, o, "propertyIsEnumerable", objproto_propenum, 1, 0);
    def_native(vm, o, "toString", objproto_tostring, 0, 0);
    def_native(vm, o, "toLocaleString", objproto_tostring, 0, 0);
    def_native(vm, o, "valueOf", objproto_valueof, 0, 0);

    /* Function */
    o = vm->function_proto;
    ctor(vm, "Function", function_ctor, 1, o);
    def_native(vm, o, "call", fn_call, 1, 0);
    def_native(vm, o, "apply", fn_apply, 2, 0);
    def_native(vm, o, "bind", fn_bind, 1, 0);
    def_native(vm, o, "toString", fn_tostring, 0, 0);

    /* Array */
    o = vm->array_proto;
    c = ctor(vm, "Array", array_ctor, 1, o);
    def_native(vm, c, "isArray", array_isarray, 1, 0);
    def_native(vm, c, "of", array_of, 0, 0);
    def_native(vm, c, "from", array_from, 1, 0);
    def_native(vm, o, "push", arr_push_n, 1, 0);
    def_native(vm, o, "pop", arr_pop, 0, 0);
    def_native(vm, o, "shift", arr_shift, 0, 0);
    def_native(vm, o, "unshift", arr_unshift, 1, 0);
    def_native(vm, o, "slice", arr_slice, 2, 0);
    def_native(vm, o, "splice", arr_splice, 2, 0);
    def_native(vm, o, "concat", arr_concat, 1, 0);
    def_native(vm, o, "join", arr_join, 1, 0);
    def_native(vm, o, "toString", arr_join, 0, 0);
    def_native(vm, o, "reverse", arr_reverse, 0, 0);
    def_native(vm, o, "indexOf", arr_indexof, 1, 0);
    def_native(vm, o, "lastIndexOf", arr_indexof, 1, 1);
    def_native(vm, o, "includes", arr_indexof, 1, 2);
    def_native(vm, o, "forEach", arr_iter, 1, 0);
    def_native(vm, o, "map", arr_iter, 1, 1);
    def_native(vm, o, "filter", arr_iter, 1, 2);
    def_native(vm, o, "some", arr_iter, 1, 3);
    def_native(vm, o, "every", arr_iter, 1, 4);
    def_native(vm, o, "find", arr_iter, 1, 5);
    def_native(vm, o, "findIndex", arr_iter, 1, 6);
    def_native(vm, o, "findLast", arr_iter, 1, 7);
    def_native(vm, o, "findLastIndex", arr_iter, 1, 8);
    def_native(vm, o, "flatMap", arr_iter, 1, 9);
    def_native(vm, o, "reduce", arr_reduce, 1, 0);
    def_native(vm, o, "reduceRight", arr_reduce, 1, 1);
    def_native(vm, o, "sort", arr_sort, 1, 0);
    def_native(vm, o, "fill", arr_fill, 1, 0);
    def_native(vm, o, "flat", arr_flat, 0, 0);
    def_native(vm, o, "at", arr_at, 1, 0);
    def_native(vm, o, "keys", arr_keys, 0, 0);
    def_native(vm, o, "values", arr_keys, 0, 1);
    def_native(vm, o, "entries", arr_keys, 0, 2);

    /* Boolean, Number */
    o = vm->boolean_proto;
    ctor(vm, "Boolean", boolean_ctor, 1, o);
    def_native(vm, o, "toString", bool_tostring, 0, 0);
    def_native(vm, o, "valueOf", bool_tostring, 0, 1);
    o = vm->number_proto;
    c = ctor(vm, "Number", number_ctor, 1, o);
    def_native(vm, o, "toString", num_tostring, 1, 0);
    def_native(vm, o, "valueOf", num_tostring, 0, 1);
    def_native(vm, o, "toFixed", num_tofixed, 1, 0);
    def_native(vm, o, "toPrecision", num_toprecision, 1, 0);
    def_native(vm, o, "toLocaleString", num_tolocale, 0, 0);
    def_native(vm, c, "isNaN", number_is, 1, 0);
    def_native(vm, c, "isFinite", number_is, 1, 1);
    def_native(vm, c, "isInteger", number_is, 1, 2);
    def_native(vm, c, "isSafeInteger", number_is, 1, 3);
    def_native(vm, c, "parseFloat", g_parsefloat, 1, 0);
    def_native(vm, c, "parseInt", g_parseint, 2, 0);
    def_val(vm, c, "MAX_SAFE_INTEGER", v_num(9007199254740991.0));
    def_val(vm, c, "MIN_SAFE_INTEGER", v_num(-9007199254740991.0));
    def_val(vm, c, "EPSILON", v_num(2.220446049250313e-16));
    def_val(vm, c, "MAX_VALUE", v_num(1.7976931348623157e308));
    def_val(vm, c, "MIN_VALUE", v_num(5e-324));
    def_val(vm, c, "POSITIVE_INFINITY", v_num(m_inf()));
    def_val(vm, c, "NEGATIVE_INFINITY", v_num(-m_inf()));
    def_val(vm, c, "NaN", v_num(m_nan()));

    /* the global functions */
    def_native(vm, g, "isNaN", g_isnan, 1, 0);
    def_native(vm, g, "isFinite", g_isnan, 1, 1);
    def_native(vm, g, "parseFloat", g_parsefloat, 1, 0);
    def_native(vm, g, "parseInt", g_parseint, 2, 0);
    def_native(vm, g, "encodeURIComponent", g_encode, 1, 0);
    def_native(vm, g, "encodeURI", g_encode, 1, 1);
    def_native(vm, g, "escape", g_encode, 1, 0);
    def_native(vm, g, "decodeURIComponent", g_decode, 1, 0);
    def_native(vm, g, "decodeURI", g_decode, 1, 1);
    def_native(vm, g, "unescape", g_decode, 1, 2);

    /* Math */
    o = obj_new(vm, vm->object_proto, C_OBJECT);
    def_val(vm, g, "Math", v_obj(o));
    {
        static const char *one[] = { "floor", "ceil", "round", "abs", "sqrt", "sin", "cos", "tan", "atan",
            "exp", "log", "trunc", "sign", "log2", "log10", "asin", "acos", "cbrt", "fround", "sinh", "cosh", "tanh", "log1p", "expm1", 0 };
        for (i = 0; one[i]; i++) def_native(vm, o, one[i], math1, 1, (u32)i);
    }
    def_native(vm, o, "min", math_minmax, 2, 0);
    def_native(vm, o, "max", math_minmax, 2, 1);
    def_native(vm, o, "pow", math_pow, 2, 0);
    def_native(vm, o, "atan2", math_atan2, 2, 0);
    def_native(vm, o, "random", math_random, 0, 0);
    def_native(vm, o, "hypot", math_hypot, 2, 0);
    def_native(vm, o, "imul", math_imul, 2, 0);
    def_native(vm, o, "clz32", math_clz32, 1, 0);
    def_val(vm, o, "PI", v_num(3.141592653589793));
    def_val(vm, o, "E", v_num(2.718281828459045));
    def_val(vm, o, "LN2", v_num(0.6931471805599453));
    def_val(vm, o, "LN10", v_num(2.302585092994046));
    def_val(vm, o, "LOG2E", v_num(1.4426950408889634));
    def_val(vm, o, "LOG10E", v_num(0.4342944819032518));
    def_val(vm, o, "SQRT2", v_num(1.4142135623730951));
    def_val(vm, o, "SQRT1_2", v_num(0.7071067811865476));

    /* the errors */
    for (i = 0; i < 6; i++) {
        Obj *p = i ? obj_new(vm, vm->error_proto, C_OBJECT) : vm->error_proto;
        Obj *e = native_new(vm, errs[i], error_ctor, 1, (u32)i);
        e->flags |= OF_CTOR;
        if (i) e->proto = vm->err_ctor[0];
        prop_put(vm, e, S_prototype, v_obj(p), P_HIDDEN);
        prop_put(vm, p, S_constructor, v_obj(e), P_HIDDEN);
        prop_put(vm, p, S_name, v_str(atom(errs[i])), P_HIDDEN);
        prop_put(vm, p, S_message, v_str(S_empty), P_HIDDEN);
        def_val(vm, g, errs[i], v_obj(e));
        vm->err_ctor[i] = e;
    }
    def_native(vm, vm->error_proto, "toString", error_tostring, 0, 0);
}
