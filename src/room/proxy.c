/* proxy.c -- Proxy: an object whose reads, writes, `in`, delete, key
 * listing and calls go to a handler's traps (get, set, has, deleteProperty,
 * ownKeys, apply, construct), and to its target where it has none.  It is
 * a C_PROXY: u.bound.target the target (0 once revoked), u.bound.self the
 * handler.  obj.c and vm.c ask here when they meet one.
 */
#include "js.h"

#define ARG(i) ((i) < argc ? args[i] : v_undef())
#define NATIVE(name) static Val name(Vm *vm, Val self, Val *args, int argc, u32 magic)
#define UNUSED (void)self; (void)args; (void)argc; (void)magic

static Val trap(Vm *vm, Obj *p, const char *name)
{
    if (!p->u.bound.target) { throw_err(vm, E_TYPE, "a revoked Proxy was used"); return v_undef(); }
    return get_prop(vm, p->u.bound.self, atom(name));
}

Val proxy_get(Vm *vm, Obj *p, Str *k, Val recv)
{
    Val t = trap(vm, p, "get"), a[3];
    if (vm->throwing) return v_undef();
    if (!is_callable(t)) return get_prop(vm, v_obj(p->u.bound.target), k);
    a[0] = v_obj(p->u.bound.target); a[1] = v_str(k); a[2] = recv;
    return vm_call(vm, t, p->u.bound.self, a, 3);
}

int proxy_set(Vm *vm, Obj *p, Str *k, Val v, Val recv)
{
    Val t = trap(vm, p, "set"), a[4];
    if (vm->throwing) return 0;
    if (!is_callable(t)) return set_prop(vm, v_obj(p->u.bound.target), k, v);
    a[0] = v_obj(p->u.bound.target); a[1] = v_str(k); a[2] = v; a[3] = recv;
    vm_call(vm, t, p->u.bound.self, a, 4);
    return !vm->throwing;
}

int proxy_has(Vm *vm, Obj *p, Str *k)
{
    Val t = trap(vm, p, "has"), a[2];
    if (vm->throwing) return 0;
    if (!is_callable(t)) return has_prop(vm, p->u.bound.target, k);
    a[0] = v_obj(p->u.bound.target); a[1] = v_str(k);
    return truthy(vm_call(vm, t, p->u.bound.self, a, 2));
}

int proxy_del(Vm *vm, Obj *p, Str *k)
{
    Val t = trap(vm, p, "deleteProperty"), a[2];
    if (vm->throwing) return 0;
    if (!is_callable(t)) return del_prop(vm, v_obj(p->u.bound.target), v_str(k));
    a[0] = v_obj(p->u.bound.target); a[1] = v_str(k);
    return truthy(vm_call(vm, t, p->u.bound.self, a, 2));
}

Obj *proxy_keys(Vm *vm, Obj *p, int all)
{
    Val t = trap(vm, p, "ownKeys"), a[1], r;
    if (vm->throwing) return 0;
    if (!is_callable(t)) return own_keys(vm, p->u.bound.target, all);
    a[0] = v_obj(p->u.bound.target);
    r = vm_call(vm, t, p->u.bound.self, a, 1);
    if (vm->throwing) return 0;
    if (r.t != T_OBJ || r.u.o->cls != C_ARRAY) { throw_err(vm, E_TYPE, "ownKeys must give a list"); return 0; }
    {   /* the engine expects names: each one made a string, never trusted as one */
        Obj *out = arr_new(vm, 0);
        u32 i;
        if (!out) return 0;
        TEMP(vm, r); TEMP(vm, v_obj(out));
        for (i = 0; i < r.u.o->u.arr.len && !vm->throwing; i++) {
            Val k = arr_get(r.u.o, i);
            Str *ks;
            if (k.t != T_STR) { throw_err(vm, E_TYPE, "ownKeys gave something that is not a name"); break; }
            ks = to_key(vm, k);
            if (ks) arr_push(vm, out, v_str(ks));
        }
        UNTEMP(vm, 2);
        return vm->throwing ? 0 : out;
    }
}

/* apply or construct: the trap with (target, this, list) or (target, list, newTarget). */
Val proxy_call(Vm *vm, Obj *p, Val self, Val *args, int argc, int is_new)
{
    Val t = trap(vm, p, is_new ? "construct" : "apply"), a[3];
    Obj *list;
    int i;
    if (vm->throwing) return v_undef();
    if (!is_callable(t)) return is_new ? vm_construct(vm, v_obj(p->u.bound.target), args, argc) : vm_call(vm, v_obj(p->u.bound.target), self, args, argc);
    list = arr_new(vm, 0);
    if (!list) return v_undef();
    TEMP(vm, v_obj(list));
    for (i = 0; i < argc; i++) arr_push(vm, list, args[i]);
    a[0] = v_obj(p->u.bound.target);
    if (is_new) { a[1] = v_obj(list); a[2] = v_obj(p); } else { a[1] = self; a[2] = v_obj(list); }
    t = vm_call(vm, t, p->u.bound.self, a, 3);
    UNTEMP(vm, 1);
    return t;
}

static Obj *make(Vm *vm, Val target, Val handler)
{
    Obj *p;
    if (target.t != T_OBJ || handler.t != T_OBJ) { throw_err(vm, E_TYPE, "a Proxy needs an object and a handler"); return 0; }
    p = obj_new(vm, target.u.o->proto, C_PROXY);
    if (!p) return 0;
    p->u.bound.target = target.u.o; p->u.bound.self = handler; p->u.bound.args = 0;
    p->flags = target.u.o->flags & (OF_CALLABLE | OF_CTOR);
    return p;
}

NATIVE(proxy_ctor)
{
    Obj *p;
    UNUSED;
    if (!vm->in_new) return throw_err(vm, E_TYPE, "Proxy needs new");
    p = make(vm, ARG(0), ARG(1));
    return p ? v_obj(p) : v_undef();
}

NATIVE(proxy_revoke)
{
    Obj *f = vm->callee;
    Val pv;
    UNUSED;
    pv = f ? get_prop(vm, v_obj(f), atom("@@proxy")) : v_undef();
    if (pv.t == T_OBJ && pv.u.o->cls == C_PROXY) pv.u.o->u.bound.target = 0;
    return v_undef();
}

NATIVE(proxy_revocable)
{
    Obj *p = make(vm, ARG(0), ARG(1)), *r, *f;
    UNUSED;
    if (!p) return v_undef();
    TEMP(vm, v_obj(p));
    r = obj_new(vm, vm->object_proto, C_OBJECT);
    f = native_new(vm, "revoke", proxy_revoke, 0, 0);
    if (r && f) {
        prop_put(vm, f, atom("@@proxy"), v_obj(p), P_HIDDEN);
        prop_put(vm, r, atom("proxy"), v_obj(p), 0);
        prop_put(vm, r, atom("revoke"), v_obj(f), 0);
    }
    UNTEMP(vm, 1);
    return r ? v_obj(r) : v_undef();
}

void proxy_init(Vm *vm)
{
    Obj *c = native_new(vm, "Proxy", proxy_ctor, 2, 0);
    if (!c) return;
    c->flags |= OF_CTOR;
    def_native(vm, c, "revocable", proxy_revocable, 2, 0);
    def_val(vm, vm->global, "Proxy", v_obj(c));
}
