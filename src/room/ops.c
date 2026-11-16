/* ===========================================================================
 * ops.c -- JavaScript's rules for turning one kind of value into another,
 * comparing values, and making errors.
 * ===========================================================================
 */
#include "js.h"

int truthy(Val v)
{
    switch (v.t) {
    case T_BOOL: return v.u.b;
    case T_NUM: return v.u.n != 0 && !m_isnan(v.u.n);
    case T_STR: return v.u.s->len != 0;
    case T_OBJ: return 1;
    default: return 0;
    }
}

int is_callable(Val v) { return v.t == T_OBJ && (v.u.o->flags & OF_CALLABLE); }

Val to_prim(Vm *vm, Val v, int hint_str)
{
    Str *order[2];
    int i;
    if (v.t != T_OBJ) return v;
    if (v.u.o->cls == C_BOXED) return v.u.o->u.boxed.v;
    if (v.u.o->cls == C_DATE && !hint_str) return v_num(v.u.o->u.date.t);
    order[0] = hint_str ? S_toString : S_valueOf;
    order[1] = hint_str ? S_valueOf : S_toString;
    for (i = 0; i < 2; i++) {
        Val f = get_prop(vm, v, order[i]);
        if (vm->throwing) return v_undef();
        if (is_callable(f)) {
            Val r = vm_call(vm, f, v, 0, 0);
            if (vm->throwing) return v_undef();
            if (r.t != T_OBJ) return r;
        }
    }
    return throw_err(vm, E_TYPE, "cannot turn an object into a plain value");
}

f64 to_num(Vm *vm, Val v)
{
    int ok;
    switch (v.t) {
    case T_NUM: return v.u.n;
    case T_BOOL: return v.u.b;
    case T_NULL: return 0;
    case T_STR: return num_parse(v.u.s->c, v.u.s->len, &ok);
    case T_OBJ: {
        Val p = to_prim(vm, v, 0);
        if (vm->throwing) return m_nan();
        return to_num(vm, p);
    }
    default: return m_nan();
    }
}

Str *to_str(Vm *vm, Val v)
{
    switch (v.t) {
    case T_STR: return v.u.s;
    case T_NUM: return num_to_str(v.u.n, 10);
    case T_BOOL: return v.u.b ? S_true : S_false;
    case T_NULL: return S_null;
    case T_UNDEF: return S_undefined;
    default: {
        Val p = to_prim(vm, v, 1);
        if (vm->throwing) return S_empty;
        return to_str(vm, p);
    }
    }
}

Str *to_key(Vm *vm, Val v)
{
    Str *s = to_str(vm, v);
    if (!s) { vm->out_of_room = 1; return 0; }
    return str_intern(s);
}

Obj *to_obj(Vm *vm, Val v)
{
    Obj *o;
    if (v.t == T_OBJ) return v.u.o;
    if (IS_NULLISH(v)) { throw_err(vm, E_TYPE, "cannot use null or undefined as an object"); return 0; }
    o = obj_new(vm, v.t == T_STR ? vm->string_proto : v.t == T_NUM ? vm->number_proto : vm->boolean_proto, C_BOXED);
    if (o) o->u.boxed.v = v;
    return o;
}

int strict_eq(Val a, Val b)
{
    if (a.t != b.t) return 0;
    switch (a.t) {
    case T_UNDEF: case T_NULL: return 1;
    case T_BOOL: return a.u.b == b.u.b;
    case T_NUM: return a.u.n == b.u.n;
    case T_STR: return str_eq(a.u.s, b.u.s);
    default: return a.u.o == b.u.o;
    }
}

int loose_eq(Vm *vm, Val a, Val b)
{
    int guard;
    for (guard = 0; guard < 4; guard++) {
        if (a.t == b.t) return strict_eq(a, b);
        if (IS_NULLISH(a) && IS_NULLISH(b)) return 1;
        if (IS_NULLISH(a) || IS_NULLISH(b)) return 0;
        if (a.t == T_NUM && b.t == T_STR) return a.u.n == to_num(vm, b);
        if (a.t == T_STR && b.t == T_NUM) return to_num(vm, a) == b.u.n;
        if (a.t == T_BOOL) { a = v_num(a.u.b); continue; }
        if (b.t == T_BOOL) { b = v_num(b.u.b); continue; }
        if (a.t == T_OBJ) { a = to_prim(vm, a, 0); if (vm->throwing) return 0; continue; }
        if (b.t == T_OBJ) { b = to_prim(vm, b, 0); if (vm->throwing) return 0; continue; }
        return 0;
    }
    return 0;
}

Str *type_of(Val v)
{
    switch (v.t) {
    case T_UNDEF: return S_undefined;
    case T_NULL: return S_object;
    case T_BOOL: return S_boolean;
    case T_NUM: return S_number;
    case T_STR: return S_string;
    default: return (v.u.o->flags & OF_CALLABLE) ? S_function : S_object;
    }
}

int instance_of(Vm *vm, Val v, Val ctor)
{
    Val pv;
    Obj *o;
    int guard = 0;
    if (!is_callable(ctor)) { throw_err(vm, E_TYPE, "the right side of instanceof is not a function"); return 0; }
    if (ctor.u.o->cls == C_BOUND) ctor = v_obj(ctor.u.o->u.bound.target);
    if (v.t != T_OBJ) return 0;
    pv = get_prop(vm, ctor, S_prototype);
    if (pv.t != T_OBJ) return 0;
    for (o = v.u.o->proto; o && guard++ < 1000; o = o->proto) if (o == pv.u.o) return 1;
    return 0;
}

/* ---- errors -------------------------------------------------------------------- */
Obj *make_err(Vm *vm, int kind, Str *msg)
{
    Val pv = get_prop(vm, v_obj(vm->err_ctor[kind]), S_prototype);
    Obj *e = obj_new(vm, pv.t == T_OBJ ? pv.u.o : vm->error_proto, C_ERROR);
    if (e && msg) prop_put(vm, e, S_message, v_str(msg), P_HIDDEN);
    return e;
}

Val throw_val(Vm *vm, Val v)
{
    vm->throwing = 1;
    vm->exception = v;
    return v_undef();
}

Val throw_err(Vm *vm, int kind, const char *fmt, ...)
{
    char b[200];
    u32 n = 0;
    const char *f = fmt;
    __builtin_va_list ap;
    Obj *e;
    __builtin_va_start(ap, fmt);
    while (*f && n < sizeof b - 1) {
        if (*f == '%' && f[1] == 's') {
            const char *s = __builtin_va_arg(ap, const char *);
            while (s && *s && n < sizeof b - 1) b[n++] = *s++;
            f += 2;
        } else if (*f == '%' && f[1] == 'd') {
            int d = __builtin_va_arg(ap, int);
            char t[12]; int k = 0;
            if (d < 0) { b[n++] = '-'; d = -d; }
            do { t[k++] = (char)('0' + d % 10); d /= 10; } while (d && k < 11);
            while (k && n < sizeof b - 1) b[n++] = t[--k];
            f += 2;
        } else b[n++] = *f++;
    }
    __builtin_va_end(ap);
    b[n] = 0;
    if (vm->throwing) return v_undef();  /* the first error wins           */
    e = make_err(vm, kind, str_utf8(b, n));
    return throw_val(vm, e ? v_obj(e) : v_str(S_empty));
}
