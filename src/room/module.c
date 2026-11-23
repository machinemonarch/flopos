/* ===========================================================================
 * module.c -- JavaScript modules: what was compiled, and in what order it runs.
 *
 * browzer loads a page's modules (script.c): it fetches one, has it
 * compiled here (engine_module_scan, which answers the addresses it
 * imports from), fetches those, and so on; then has them run, each after
 * those it imports from (engine_module_run, given where each of its
 * addresses led).  Each module is kept, by its address, in a hidden
 * table on the global object: its function, its namespace (what it
 * exports, as getters) and import.meta.  While a module starts, the few
 * built-ins its first lines call (compile.c, module_start) find it as
 * "the current one".
 * ===========================================================================
 */
#include "js.h"
#include "engine.h"

#define ARG(i) ((i) < argc ? args[i] : v_undef())
static Val meta_resolve(Vm *vm, Val self, Val *args, int argc, u32 magic);
#define NATIVE(name) static Val name(Vm *vm, Val self, Val *args, int argc, u32 magic)
#define UNUSED (void)self; (void)args; (void)argc; (void)magic

extern Str *g_mod_spec[];
extern int  g_mod_nspec;
Proto *js_compile(Vm *vm, const char *src, u32 len, const char *name, char *why, u32 whycap, int as_function);

static void say(char *err, u32 cap, const char *t)
{
    u32 n = 0;
    if (!err || !cap) return;
    while (t[n] && n + 1 < cap) { err[n] = t[n]; n++; }
    err[n] = 0;
}

static Obj *hidden_obj(Vm *vm, const char *name)
{
    Val r = get_prop(vm, v_obj(vm->global), atom(name));
    Obj *o;
    if (r.t == T_OBJ) return r.u.o;
    o = obj_new(vm, 0, C_OBJECT);
    if (!o || !prop_put(vm, vm->global, atom(name), v_obj(o), P_HIDDEN)) return 0;
    return o;
}

static Val record_of(Vm *vm, const char *url)
{
    Obj *reg = hidden_obj(vm, "__flopos_modules");
    Str *k = str_utf8(url, js_strlen(url));
    if (!reg || !k) return v_undef();
    k = str_intern(k);
    return get_prop(vm, v_obj(reg), k);
}

static Val current(Vm *vm) { return get_prop(vm, v_obj(vm->global), atom("__flopos_current")); }

/* Compile the module at url; its addresses go into list, one a line.
 * A module already kept is not compiled again (ENGINE_OK, and an empty
 * list: its imports were seen already). */
int engine_module_scan(Vm *vm, const char *src, u32 len, const char *url, char *list, u32 cap, char *err, u32 errcap)
{
    Proto *p;
    Obj *reg, *rec, *fn, *ns, *meta;
    Str *k;
    u32 n = 0;
    int i;
    if (err && errcap) err[0] = 0;
    if (cap) list[0] = 0;
    engine_begin(vm, 1000);
    if (record_of(vm, url).t == T_OBJ) return ENGINE_OK;
    p = js_compile(vm, src, len, url, err, errcap, 2);
    if (!p) return vm->out_of_room ? ENGINE_ROOM : ENGINE_SYNTAX;
    for (i = 0; i < g_mod_nspec; i++) {
        u32 m = str_to_utf8(g_mod_spec[i], list + n, cap > n + 2 ? cap - n - 2 : 0);
        if (n + m + 2 >= cap) { say(err, errcap, "it imports from too many places"); return ENGINE_ROOM; }
        n += m;
        list[n++] = '\n';
    }
    if (cap) list[n] = 0;
    reg = hidden_obj(vm, "__flopos_modules");
    k = str_utf8(url, js_strlen(url));
    if (k) k = str_intern(k);
    fn = obj_new(vm, vm->function_proto, C_FUNC);
    if (!reg || !k || !fn) return ENGINE_ROOM;
    fn->flags = OF_CALLABLE;
    fn->u.fn.fn = p;
    rec = obj_new(vm, 0, C_OBJECT);
    if (!rec || !prop_put(vm, reg, k, v_obj(rec), 0)) return ENGINE_ROOM;
    prop_put(vm, rec, atom("fn"), v_obj(fn), 0);
    ns = obj_new(vm, vm->object_proto, C_OBJECT);
    meta = obj_new(vm, vm->object_proto, C_OBJECT);
    if (!ns || !meta) return ENGINE_ROOM;
    prop_put(vm, rec, atom("ns"), v_obj(ns), 0);
    prop_put(vm, meta, atom("url"), v_str(k), 0);
    def_native(vm, meta, "resolve", meta_resolve, 1, 0);
    prop_put(vm, rec, atom("meta"), v_obj(meta), 0);
    return ENGINE_OK;
}

/* Run the module at url, its addresses having led to the modules at
 * dep[0..n-1] (each run already, or running: a circle of imports). */
int engine_module_run(Vm *vm, const char *url, const char **dep, int n, u32 ms, char *err, u32 errcap)
{
    Val rec = record_of(vm, url), fn, done;
    Obj *deps;
    int i, r;
    if (err && errcap) err[0] = 0;
    if (rec.t != T_OBJ) { say(err, errcap, "a module that was never read"); return ENGINE_THREW; }
    done = get_prop(vm, rec, atom("ran"));
    if (done.t != T_UNDEF) return ENGINE_OK;          /* each module runs once */
    deps = arr_new(vm, 0);
    if (!deps) return ENGINE_ROOM;
    prop_put(vm, rec.u.o, atom("deps"), v_obj(deps), 0);
    for (i = 0; i < n; i++) {
        Val d = record_of(vm, dep[i]);
        if (d.t != T_OBJ) { say(err, errcap, "a module it imports from did not load"); return ENGINE_THREW; }
        arr_push(vm, deps, get_prop(vm, d, atom("ns")));
    }
    prop_put(vm, rec.u.o, atom("ran"), v_bool(1), 0);
    prop_put(vm, vm->global, atom("__flopos_current"), rec, P_HIDDEN);
    fn = get_prop(vm, rec, atom("fn"));
    r = engine_call(vm, fn, v_undef(), 0, 0, ms, err, errcap);
    prop_put(vm, vm->global, atom("__flopos_current"), v_undef(), P_HIDDEN);
    return r;
}

/* The namespace of the module at url, or undefined (import()). */
Val module_ns(Vm *vm, const char *url)
{
    Val rec = record_of(vm, url);
    return rec.t == T_OBJ ? get_prop(vm, rec, atom("ns")) : v_undef();
}

NATIVE(mod_ns)                               /* __flopos_modns(i) */
{
    Val cur = current(vm), deps;
    UNUSED;
    if (cur.t != T_OBJ) return throw_err(vm, E_ERROR, "an import outside a module's start");
    deps = get_prop(vm, cur, atom("deps"));
    if (deps.t != T_OBJ) return v_undef();
    return get_elem(vm, deps, ARG(0));
}

/* import.meta.resolve(name): the full address, against the module's own
 * (dom.c's __flopos_mresolve, which knows addresses; the name as it is without it). */
static Val meta_resolve(Vm *vm, Val self, Val *args, int argc, u32 magic)
{
    Val f = get_prop(vm, v_obj(vm->global), atom("__flopos_mresolve")), a[2];
    UNUSED;
    a[0] = ARG(0); a[1] = self.t == T_OBJ ? get_prop(vm, self, atom("url")) : v_undef();
    if (!is_callable(f)) return v_str(to_str(vm, ARG(0)));
    return vm_call(vm, f, v_undef(), a, 2);
}

NATIVE(mod_self)                             /* 0 its namespace, 1 import.meta */
{
    Val cur = current(vm);
    UNUSED;
    if (cur.t != T_OBJ) return throw_err(vm, E_ERROR, "a module's start outside a module");
    return get_prop(vm, cur, atom(magic ? "meta" : "ns"));
}

NATIVE(mod_star)                             /* export * from ...: its getters, shared */
{
    Val cur = current(vm), deps, from, to;
    Obj *keys;
    u32 j;
    UNUSED;
    if (cur.t != T_OBJ) return v_undef();
    deps = get_prop(vm, cur, atom("deps"));
    from = deps.t == T_OBJ ? get_elem(vm, deps, ARG(0)) : v_undef();
    to = get_prop(vm, cur, atom("ns"));
    if (from.t != T_OBJ || to.t != T_OBJ) return v_undef();
    keys = own_keys(vm, from.u.o, 0);
    if (!keys) return v_undef();
    TEMP(vm, v_obj(keys));
    for (j = 0; j < keys->u.arr.len; j++) {
        Val kv = arr_get(keys, j);
        Prop *q, *d;
        if (kv.t != T_STR || str_eq_ascii(kv.u.s, "default") || prop_find(to.u.o, kv.u.s)) continue;
        q = prop_find(from.u.o, kv.u.s);
        if (!q) continue;
        if (!prop_put(vm, to.u.o, kv.u.s, q->flags & P_ACCESSOR ? v_undef() : q->v, q->flags & P_ACCESSOR)) break;
        if (q->flags & P_ACCESSOR) { d = prop_find(to.u.o, kv.u.s); d->v = q->v; d->set = v_undef(); }
    }
    UNTEMP(vm, 1);
    return v_undef();
}

void module_init(Vm *vm)
{
    Obj *g = vm->global;
    def_native(vm, g, "__flopos_modns", mod_ns, 1, 0);
    def_native(vm, g, "__flopos_modself", mod_self, 0, 0);
    def_native(vm, g, "__flopos_modmeta", mod_self, 0, 1);
    def_native(vm, g, "__flopos_star", mod_star, 1, 0);
}
