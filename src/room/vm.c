/* ===========================================================================
 * vm.c -- carrying out the instructions.
 *
 * A stack machine: instructions take their values from the top of a
 * stack and put their results back there.  A call to a script's function
 * opens a frame (where it is, its variables, its `this`) and carries on in
 * the same loop, so deep scripts do not use up the C stack; only C code
 * calling back into a script (Array map, a getter) starts a loop of its
 * own, inside the first.
 *
 * Every instruction is counted.  Every few thousand, the clock is read:
 * a script that has had its time is stopped, wherever it is, and nothing
 * it does can catch that.  The same goes for a script that fills the
 * memory.
 *
 * Values an instruction works on stay on the stack until it is done with
 * them, because working them out can run more script (a valueOf, a
 * getter), and the sweeper only looks after what is on the stack.
 * ===========================================================================
 */
#include "js.h"

Vm *g_vm;
extern int g_gc_stress;

#define PUSH(v) do { Val push_ = (v); vm->stack[vm->sp] = push_; vm->sp++; } while (0)
#define TOP     (vm->stack[vm->sp - 1])
#define AT(n)   (vm->stack[vm->sp - 1 - (n)])

/* ---- time ------------------------------------------------------------------------ */
int vm_check_time(Vm *vm)
{
    u32 now = host_ms() - vm->door_ms;
    if (vm->deadline && (s32)(now - vm->deadline) >= 0) {
        vm->out_of_time = 1;
        vm->fatal = 1;
        if (!vm->stop_why) vm->stop_why = "it ran out of time";
        throw_val(vm, v_str(S_empty));
        return 0;
    }
    if (vm->out_of_room) {
        vm->fatal = 1;
        if (!vm->stop_why) vm->stop_why = "it ran out of room";
        throw_val(vm, v_str(S_empty));
        return 0;
    }
    return 1;
}

/* ---- calls ------------------------------------------------------------------------ */
static Val call_other(Vm *vm, Obj *fn, Val self, Val *args, int argc, int is_new);

/* A frame for one of the script's own functions.  The arguments are on
 * the stack at base+1+has_self ... */
static int enter(Vm *vm, Obj *fn, Val self, Val *args, int argc, u32 base, int is_new, int stop)
{
    Proto *p = fn->u.fn.fn;
    Env *env;
    Frame *f;
    u32 i, n = p->nslots;
    if (vm->nframes >= VM_FRAMES - 1) { throw_err(vm, E_RANGE, "too much recursion"); return 0; }
    if (vm->sp + 64 > VM_STACK) { throw_err(vm, E_RANGE, "too much recursion"); return 0; }
    env = gc_alloc(sizeof(Env) + n * sizeof(Val), K_ENV);
    if (!env) { vm->out_of_room = 1; vm_check_time(vm); return 0; }
    env->parent = fn->u.fn.env;
    env->n = n;
    for (i = 0; i < n; i++) env->v[i] = v_undef();
    if (p->pad[0] && p->nparams) {               /* ...rest              */
        u32 fixed = p->nparams - 1u;
        for (i = 0; i < fixed && (int)i < argc; i++) env->v[i] = args[i];
        {
            Obj *rest = arr_new(vm, 0);
            if (!rest) { vm_check_time(vm); return 0; }
            for (i = fixed; (int)i < argc; i++) arr_push(vm, rest, args[i]);
            env->v[fixed] = v_obj(rest);
        }
    } else for (i = 0; i < p->nparams && (int)i < argc; i++) env->v[i] = args[i];
    if (p->args_slot != 0xFFFF && p->args_slot < n) {
        Obj *a = arr_new(vm, 0);
        if (!a) { vm_check_time(vm); return 0; }
        a->cls = C_ARGS;
        for (i = 0; (int)i < argc; i++) arr_push(vm, a, args[i]);
        env->v[p->args_slot] = v_obj(a);
    }
    if (p->self_slot != 0xFFFF && p->self_slot < n) env->v[p->self_slot] = v_obj(fn);
    vm->sp = base;
    f = &vm->frames[vm->nframes++];
    f->p = p;
    f->fn = fn;
    f->env = env;
    if (fn->flags & OF_ARROW) f->self = fn->u.fn.self;
    else f->self = IS_NULLISH(self) ? v_obj(vm->global) : self;
    f->pc = 0;
    f->base = base;
    f->ntry = 0;
    f->is_new = (u8)is_new;
    f->stop = (u8)stop;
    f->co = 0;
    vm->steps++;
    if (p->pad[1]) {                     /* async: its promise, made now     */
        Obj *pr = promise_new(vm), *co;
        if (pr) TEMP(vm, v_obj(pr));
        co = pr ? obj_new(vm, vm->object_proto, C_ASYNC) : 0;
        if (pr) UNTEMP(vm, 1);
        if (!co) { vm->nframes--; vm_check_time(vm); return 0; }
        co->u.co.prom = v_obj(pr);
        co->u.co.stack = 0;
        co->u.co.fr = 0;
        vm->frames[vm->nframes - 1].co = co;
    }
    return 1;
}

static Val run(Vm *vm);

/* ---- async functions -----------------------------------------------------------
 * An async function runs like any other until its first await.  There its
 * frame -- where it had got to, its try blocks, the few values it had on
 * the stack mid-expression -- is packed into its C_ASYNC object, the frame
 * is left, and whoever called it gets its promise.  Its locals live in its
 * Env on the heap already, so nothing else needs keeping.  When what it
 * awaited settles, a microtask calls async_step, which unpacks the frame
 * on top of the stack and runs it on: to the next await, or to its end,
 * where return settles the promise and an uncaught throw rejects it. */
static Val async_step(Vm *vm, Val self, Val *args, int argc, u32 magic)
{
    Obj *me = vm->callee, *co;
    Val cv = get_prop(vm, v_obj(me), atom("%co")), arg = argc ? args[0] : v_undef();
    Frame *f, *saved;
    Vals *st;
    u32 base, i, n;
    (void)self;
    if (cv.t != T_OBJ || cv.u.o->cls != C_ASYNC || !cv.u.o->u.co.fr) return v_undef();
    co = cv.u.o;
    saved = (Frame *)co->u.co.fr;
    st = co->u.co.stack;
    n = st ? st->n : 0;
    if (vm->nframes >= VM_FRAMES - 1 || vm->sp + n + 64 > VM_STACK) {
        Obj *e = make_err(vm, E_RANGE, str_ascii("too much recursion"));
        co->u.co.fr = 0;
        promise_resolve_fn(vm, co->u.co.prom.u.o, e ? v_obj(e) : v_undef(), 1);
        return v_undef();
    }
    base = vm->sp;
    for (i = 0; i < n; i++) vm->stack[base + i] = st->v[i];
    vm->sp = base + n;
    f = &vm->frames[vm->nframes++];
    *f = *saved;
    f->base = base;
    f->stop = 1;
    f->co = co;
    for (i = 0; i < f->ntry; i++) f->tries[i].sp += base;
    co->u.co.fr = 0;
    co->u.co.stack = 0;
    if (magic == 0) vm->stack[vm->sp++] = arg;           /* await's answer   */
    else { vm->throwing = 1; vm->exception = arg; }      /* it threw, at the await */
    run(vm);
    return v_undef();
}

/* ---- generators -----------------------------------------------------------------
 * Calling a generator function runs only its parameters; then (OP_GENSTART)
 * its frame is packed into a C_GEN object, the way await packs an async
 * function's, and the caller gets that object.  next(v) unpacks the frame
 * and runs it until a yield packs it again ({value, done: false}) or it
 * returns ({value, done: true}).  u.co.prom holds where it is: 0 paused,
 * 1 running, 2 finished. */
static int gen_pack(Vm *vm, Obj *g, u32 above)
{
    Frame *f = &vm->frames[vm->nframes - 1], *saved;
    u32 n = vm->sp - above - f->base, i;
    Vals *st;
    GcHdr *raw;
    st = gc_alloc(sizeof(Vals) + n * sizeof(Val), K_VALS);
    if (!st) { vm->out_of_room = 1; return 0; }
    st->n = n;
    for (i = 0; i < n; i++) st->v[i] = vm->stack[f->base + i];
    g->u.co.stack = st;
    raw = gc_alloc(sizeof(GcHdr) + sizeof(Frame), K_BYTES);
    if (!raw) { vm->out_of_room = 1; return 0; }
    f = &vm->frames[vm->nframes - 1];
    saved = (Frame *)(raw + 1);
    *saved = *f;
    for (i = 0; i < saved->ntry; i++) saved->tries[i].sp -= f->base;
    saved->co = 0;
    g->u.co.fr = saved;
    return 1;
}

static Val iter_result(Vm *vm, Val v, int done)
{
    Obj *r;
    TEMP(vm, v);
    r = obj_new(vm, vm->object_proto, C_OBJECT);
    UNTEMP(vm, 1);
    if (!r) return v_undef();
    prop_put(vm, r, S_value, v, 0);
    prop_put(vm, r, S_done, v_bool(done), 0);
    return v_obj(r);
}

static Val gen_step(Vm *vm, Val self, Val *args, int argc, u32 magic)   /* 0 next, 1 throw, 2 return */
{
    Obj *g;
    Frame *f, *saved;
    Vals *st;
    u32 base, i, n;
    Val arg = argc ? args[0] : v_undef(), r;
    if (self.t != T_OBJ || self.u.o->cls != C_GEN) return throw_err(vm, E_TYPE, "next() on something that is not a generator");
    g = self.u.o;
    if (g->u.co.prom.u.n == 1) return throw_err(vm, E_TYPE, "a generator asked for more while it runs");
    if (g->u.co.prom.u.n == 2 || magic == 2) {
        g->u.co.prom = v_num(2); g->u.co.fr = 0; g->u.co.stack = 0;
        if (magic == 1) { throw_val(vm, arg); return v_undef(); }
        return iter_result(vm, magic == 2 ? arg : v_undef(), 1);
    }
    saved = (Frame *)g->u.co.fr;
    st = g->u.co.stack;
    n = st ? st->n : 0;
    if (!saved) return iter_result(vm, v_undef(), 1);
    if (vm->nframes >= VM_FRAMES - 1 || vm->sp + n + 64 > VM_STACK) return throw_err(vm, E_RANGE, "too much recursion");
    base = vm->sp;
    for (i = 0; i < n; i++) vm->stack[base + i] = st->v[i];
    vm->sp = base + n;
    f = &vm->frames[vm->nframes++];
    *f = *saved;
    f->base = base;
    f->stop = 1;
    f->co = g;
    for (i = 0; i < f->ntry; i++) f->tries[i].sp += base;
    g->u.co.fr = 0;
    g->u.co.stack = 0;
    g->u.co.prom = v_num(1);
    if (magic == 0) vm->stack[vm->sp++] = arg;           /* what yield gives back */
    else { vm->throwing = 1; vm->exception = arg; }      /* thrown at the yield   */
    r = run(vm);
    if (vm->throwing || vm->fatal) { g->u.co.prom = v_num(2); g->u.co.fr = 0; g->u.co.stack = 0; return v_undef(); }
    return r;
}

static Val gen_self(Vm *vm, Val self, Val *args, int argc, u32 magic) { (void)vm; (void)args; (void)argc; (void)magic; return self; }

/* ---- async generators ------------------------------------------------------------
 * An async generator is a generator inside (its awaits are OP_AWAITY: a
 * yield that says "wait on this"), wrapped in an object whose next(),
 * throw() and return() give promises.  Requests queue ("%q": [kind,
 * value, promise] each) and are served in turn: the generator runs to its
 * next yield (the head request's promise settles with it), its end, or an
 * await (the driver waits on the value, then runs it on). */
static Val agen_wake(Vm *vm, Val self, Val *args, int argc, u32 magic);

static Val agen_new(Vm *vm, Obj *g)
{
    Obj *ag, *q;
    TEMP(vm, v_obj(g));
    ag = obj_new(vm, vm->agen_proto, C_OBJECT);
    if (ag) { TEMP(vm, v_obj(ag)); q = arr_new(vm, 0); if (q) prop_put(vm, ag, atom("%q"), v_obj(q), P_HIDDEN); prop_put(vm, ag, atom("%g"), v_obj(g), P_HIDDEN); UNTEMP(vm, 1); }
    UNTEMP(vm, 1);
    return ag ? v_obj(ag) : v_undef();
}

/* Settle the head request and take it off the queue. */
static void agen_settle(Vm *vm, Obj *q, Val v, int rejected)
{
    Val pr = arr_get(arr_get(q, 0).u.o, 2);
    u32 i;
    for (i = 1; i < q->u.arr.len; i++) q->u.arr.items->v[i - 1] = q->u.arr.items->v[i];
    if (q->u.arr.len) q->u.arr.len--;
    if (pr.t == T_OBJ) promise_resolve_fn(vm, pr.u.o, v, rejected);
}

/* Run the generator on for the head request: 1 if it now waits on an await. */
static int agen_step(Vm *vm, Obj *ag, Obj *q, u32 kind, Val arg)
{
    Val g = get_prop(vm, v_obj(ag), atom("%g")), r;
    vm->gen_awaited = 0;
    TEMP(vm, arg);
    r = gen_step(vm, g, &arg, 1, kind);
    UNTEMP(vm, 1);
    if (vm->fatal) return 1;
    if (vm->throwing) {
        Val e = vm->exception;
        vm->throwing = 0; vm->exception = v_undef();
        agen_settle(vm, q, e, 1);
        return 0;
    }
    if (vm->gen_awaited) {
        Obj *pr, *ok, *bad;
        Val v = r.t == T_OBJ ? get_prop(vm, r, S_value) : v_undef();
        vm->gen_awaited = 0;
        TEMP(vm, v);
        pr = promise_new(vm);
        if (pr) TEMP(vm, v_obj(pr));
        ok = pr ? native_new(vm, "await", agen_wake, 1, 0) : 0;
        if (ok) { TEMP(vm, v_obj(ok)); prop_put(vm, ok, atom("%ag"), v_obj(ag), P_HIDDEN); }
        bad = ok ? native_new(vm, "await", agen_wake, 1, 1) : 0;
        if (bad) { prop_put(vm, bad, atom("%ag"), v_obj(ag), P_HIDDEN); promise_then(vm, pr, v_obj(ok), v_obj(bad)); promise_resolve_fn(vm, pr, v, 0); }
        UNTEMP(vm, 1u + (pr ? 1u : 0u) + (ok ? 1u : 0u));
        if (!bad) { vm_check_time(vm); return 1; }
        return 1;
    }
    agen_settle(vm, q, r, 0);
    return 0;
}

/* Serve the queue until it is empty or waiting. */
static void agen_run(Vm *vm, Obj *ag)
{
    Val qv = get_prop(vm, v_obj(ag), atom("%q"));
    Obj *q = qv.t == T_OBJ ? qv.u.o : 0;
    int guard = 0;
    if (!q || truthy(get_prop(vm, v_obj(ag), atom("%busy")))) return;
    while (q->u.arr.len && !vm->fatal && guard++ < 100000) {
        Obj *req = arr_get(q, 0).u.o;
        prop_put(vm, ag, atom("%busy"), v_bool(1), P_HIDDEN);
        if (agen_step(vm, ag, q, (u32)arr_get(req, 0).u.n, arr_get(req, 1))) return;   /* waiting: stays busy */
        prop_put(vm, ag, atom("%busy"), v_bool(0), P_HIDDEN);
    }
}

/* An await in it settled: run it on with the answer (or the error thrown there). */
static Val agen_wake(Vm *vm, Val self, Val *args, int argc, u32 magic)
{
    Val agv = get_prop(vm, v_obj(vm->callee), atom("%ag")), qv;
    (void)self;
    if (agv.t != T_OBJ) return v_undef();
    qv = get_prop(vm, agv, atom("%q"));
    if (qv.t != T_OBJ || !qv.u.o->u.arr.len) return v_undef();
    if (agen_step(vm, agv.u.o, qv.u.o, magic, argc ? args[0] : v_undef())) return v_undef();
    prop_put(vm, agv.u.o, atom("%busy"), v_bool(0), P_HIDDEN);
    agen_run(vm, agv.u.o);
    return v_undef();
}

static Val agen_request(Vm *vm, Val self, Val *args, int argc, u32 magic)   /* 0 next, 1 throw, 2 return */
{
    Obj *pr = promise_new(vm), *req;
    Val qv;
    if (!pr) return v_undef();
    TEMP(vm, v_obj(pr));
    qv = self.t == T_OBJ ? get_prop(vm, self, atom("%q")) : v_undef();
    if (qv.t != T_OBJ) {
        Obj *e = make_err(vm, E_TYPE, str_ascii("next() on something that is not an async generator"));
        promise_resolve_fn(vm, pr, e ? v_obj(e) : v_undef(), 1);
        UNTEMP(vm, 1);
        return v_obj(pr);
    }
    req = arr_new(vm, 0);
    if (req) {
        arr_push(vm, req, v_num(magic));
        arr_push(vm, req, argc ? args[0] : v_undef());
        arr_push(vm, req, v_obj(pr));
        arr_push(vm, qv.u.o, v_obj(req));
        agen_run(vm, self.u.o);
    }
    UNTEMP(vm, 1);
    return v_obj(pr);
}

void gen_init(Vm *vm)
{
    vm->gen_proto = obj_new(vm, vm->iter_proto ? vm->iter_proto : vm->object_proto, C_OBJECT);
    if (!vm->gen_proto) return;
    def_native(vm, vm->gen_proto, "next", gen_step, 1, 0);
    def_native(vm, vm->gen_proto, "throw", gen_step, 1, 1);
    def_native(vm, vm->gen_proto, "return", gen_step, 1, 2);
    def_native(vm, vm->gen_proto, "@@iterator", gen_self, 0, 0);
    vm->agen_proto = obj_new(vm, vm->object_proto, C_OBJECT);
    if (!vm->agen_proto) return;
    def_native(vm, vm->agen_proto, "next", agen_request, 1, 0);
    def_native(vm, vm->agen_proto, "throw", agen_request, 1, 1);
    def_native(vm, vm->agen_proto, "return", agen_request, 1, 2);
    def_native(vm, vm->agen_proto, "@@asyncIterator", gen_self, 0, 0);
}

/* From C: call fn with these arguments. */
Val vm_call(Vm *vm, Val fnv, Val self, Val *args, int argc)
{
    u32 base = vm->sp, i;
    Obj *fn;
    Val r;
    if (vm->throwing) return v_undef();
    if (!is_callable(fnv)) return throw_err(vm, E_TYPE, "something that is not a function was called");
    if (vm->sp + (u32)argc + 4 > VM_STACK) return throw_err(vm, E_RANGE, "too much recursion");
    fn = fnv.u.o;
    PUSH(self); PUSH(fnv);
    for (i = 0; (int)i < argc; i++) PUSH(args[i]);
    if (g_gc_stress) gc_collect(vm);     /* what C holds here must be on the stack or in temps */
    if (fn->cls == C_FUNC) {
        if (!enter(vm, fn, self, &vm->stack[base + 2], argc, base, 0, 1)) { vm->sp = base; return v_undef(); }
        return run(vm);
    }
    r = call_other(vm, fn, self, &vm->stack[base + 2], argc, 0);
    vm->sp = base;
    return r;
}

Val vm_construct(Vm *vm, Val fnv, Val *args, int argc)
{
    u32 base = vm->sp, i;
    Obj *fn, *self;
    Val pv, r;
    if (vm->throwing) return v_undef();
    if (!is_callable(fnv) || (fnv.u.o->flags & OF_ARROW)) return throw_err(vm, E_TYPE, "something that is not a constructor was used with new");
    fn = fnv.u.o;
    if (vm->sp + (u32)argc + 4 > VM_STACK) return throw_err(vm, E_RANGE, "too much recursion");
    PUSH(v_undef()); PUSH(fnv);
    for (i = 0; (int)i < argc; i++) PUSH(args[i]);
    if (fn->cls == C_FUNC) {
        pv = get_prop(vm, fnv, S_prototype);
        self = obj_new(vm, pv.t == T_OBJ ? pv.u.o : vm->object_proto, C_OBJECT);
        if (!self) { vm->sp = base; vm_check_time(vm); return v_undef(); }
        vm->stack[base] = v_obj(self);
        if (!enter(vm, fn, v_obj(self), &vm->stack[base + 2], argc, base, 1, 1)) { vm->sp = base; return v_undef(); }
        return run(vm);
    }
    r = call_other(vm, fn, v_undef(), &vm->stack[base + 2], argc, 1);
    vm->sp = base;
    return r;
}

/* Natives and bound functions. */
static Val call_other(Vm *vm, Obj *fn, Val self, Val *args, int argc, int is_new)
{
    if (fn->cls == C_NATIVE) {
        Val r;
        Obj *keep = vm->callee;
        vm->in_new = is_new;
        vm->callee = fn;
        vm->steps++;
        r = fn->u.nat.f(vm, self, args, argc, fn->u.nat.magic);
        vm->in_new = 0;
        vm->callee = keep;
        return r;
    }
    if (fn->cls == C_BOUND) {
        Vals *b = fn->u.bound.args;
        u32 nb = b ? b->n : 0, i, base = vm->sp;
        Val r;
        if (vm->sp + nb + (u32)argc + 4 > VM_STACK) return throw_err(vm, E_RANGE, "too much recursion");
        for (i = 0; i < nb; i++) PUSH(b->v[i]);
        for (i = 0; (int)i < argc; i++) PUSH(args[i]);
        if (is_new) r = vm_construct(vm, v_obj(fn->u.bound.target), &vm->stack[base], (int)(nb + (u32)argc));
        else r = vm_call(vm, v_obj(fn->u.bound.target), fn->u.bound.self, &vm->stack[base], (int)(nb + (u32)argc));
        vm->sp = base;
        return r;
    }
    if (fn->cls == C_FUNC) {
        if (is_new) return vm_construct(vm, v_obj(fn), args, argc);
        return vm_call(vm, v_obj(fn), self, args, argc);
    }
    return throw_err(vm, E_TYPE, "something that is not a function was called");
}

/* ---- arithmetic --------------------------------------------------------------- */
static int add(Vm *vm, Val *a, Val *b, Val *out)
{
    if (a->t == T_NUM && b->t == T_NUM) { *out = v_num(a->u.n + b->u.n); return 1; }
    if (a->t == T_STR && b->t == T_STR) {
        Str *s = str_concat(a->u.s, b->u.s);
        if (!s) { vm->out_of_room = 1; return 0; }
        *out = v_str(s);
        return 1;
    }
    *a = to_prim(vm, *a, 0);             /* kept where it was: still on the stack */
    if (vm->throwing) return 0;
    *b = to_prim(vm, *b, 0);
    if (vm->throwing) return 0;
    if (a->t == T_STR || b->t == T_STR) {
        Str *x = to_str(vm, *a), *y;
        *a = v_str(x);
        y = to_str(vm, *b);
        x = str_concat(x, y);
        if (!x) { vm->out_of_room = 1; return 0; }
        *out = v_str(x);
        return 1;
    }
    *out = v_num(to_num(vm, *a) + to_num(vm, *b));
    return !vm->throwing;
}

static int compare(Vm *vm, Val *a, Val *b, int op)   /* -1 if it threw */
{
    f64 x, y;
    if (a->t == T_NUM && b->t == T_NUM) { x = a->u.n; y = b->u.n; }
    else {
        *a = to_prim(vm, *a, 0); if (vm->throwing) return -1;
        *b = to_prim(vm, *b, 0); if (vm->throwing) return -1;
        if (a->t == T_STR && b->t == T_STR) {
            int c = str_cmp(a->u.s, b->u.s);
            switch (op) { case OP_LT: return c < 0; case OP_LE: return c <= 0; case OP_GT: return c > 0; default: return c >= 0; }
        }
        x = to_num(vm, *a); y = to_num(vm, *b);
        if (vm->throwing) return -1;
    }
    switch (op) { case OP_LT: return x < y; case OP_LE: return x <= y; case OP_GT: return x > y; default: return x >= y; }
}

static Val get_global(Vm *vm, Str *k, int *found)
{
    Obj *o;
    int guard = 0;
    for (o = vm->global; o && guard++ < 100; o = o->proto) {
        Prop *q = prop_find(o, k);
        if (q) {
            *found = 1;
            if (q->flags & P_ACCESSOR) return IS_OBJ(q->v) ? vm_call(vm, q->v, v_obj(vm->global), 0, 0) : v_undef();
            return q->v;
        }
    }
    *found = 0;
    return v_undef();
}

/* An iterator for for-in and for-of. */
static Obj *iter_new(Vm *vm, Val of, int keys)
{
    Obj *it;
    /* for-of over an object that says how ([Symbol.iterator]): its iterator */
    if (!keys && of.t == T_OBJ && of.u.o->cls == C_MAP && of.u.o->pad < 2) {
        /* a Map or Set as it is (not one with its own [Symbol.iterator]) */
        Val mk = get_prop(vm, of, atom("@@iterator"));
        if (vm->throwing) return 0;
        if (mk.t == T_OBJ && mk.u.o->cls == C_NATIVE && of.u.o->proto == vm->coll_proto[of.u.o->pad]) return mapiter_for(vm, of.u.o);
    }
    if (!keys && of.t == T_OBJ && of.u.o->cls == C_MAPITER) return of.u.o;   /* an iterator already */
    if (!keys && of.t == T_OBJ && of.u.o->cls != C_ARRAY && of.u.o->cls != C_ARGS && of.u.o->cls != C_GEN) {
        Val mk = get_prop(vm, of, atom("@@iterator"));
        if (vm->throwing) return 0;
        if (is_callable(mk) && !is_callable(get_prop(vm, of, S_next))) {
            of = vm_call(vm, mk, of, 0, 0);
            if (vm->throwing) return 0;
            if (of.t == T_OBJ && of.u.o->cls == C_MAPITER) return of.u.o;
        }
    }
    it = obj_new(vm, vm->iter_proto, C_ITER);
    if (!it) return 0;
    it->u.iter.of = of;
    it->u.iter.i = 0;
    TEMP(vm, v_obj(it));
    if (keys) {
        Obj *list = 0;
        if (of.t == T_OBJ) {
            Obj *o = of.u.o;
            int guard = 0;
            list = own_keys(vm, o, 0);
            /* and what it gets from user-made prototypes */
            for (o = o->proto; o && list && guard++ < 50; o = o->proto) {
                u32 i;
                if (!o->props) continue;
                for (i = 0; i < o->props->n; i++) {
                    Prop *q = &o->props->p[i];
                    if (q->k && !(q->flags & P_HIDDEN)) arr_push(vm, list, v_str(q->k));
                }
            }
        } else if (of.t == T_STR) {
            u32 i;
            list = arr_new(vm, 0);
            for (i = 0; list && i < of.u.s->len; i++) arr_push(vm, list, v_str(num_to_str((f64)i, 10)));
        } else list = arr_new(vm, 0);
        if (list) it->u.iter.keys = list->u.arr.items, it->u.iter.of = v_obj(list);
        it->flags = 1;                    /* a list of keys: walked like an array */
    }
    UNTEMP(vm, 1);
    return it;
}

static int iter_next(Vm *vm, Obj *it, Val *out)
{
    Val of = it->u.iter.of;
    if (it->cls == C_MAPITER) return mapiter_step(vm, it, out);
    if (of.t == T_OBJ && (of.u.o->cls == C_ARRAY || of.u.o->cls == C_ARGS)) {
        if (it->u.iter.i >= of.u.o->u.arr.len) return 0;
        *out = arr_get(of.u.o, it->u.iter.i++);
        return 1;
    }
    if (of.t == T_STR) {
        Str *s = of.u.s;
        u32 i = it->u.iter.i;
        if (i >= s->len) return 0;
        if (s->c[i] >= 0xD800 && s->c[i] < 0xDC00 && i + 1 < s->len) { *out = v_str(str_from16(&s->c[i], 2)); it->u.iter.i += 2; }
        else { *out = v_str(str_from16(&s->c[i], 1)); it->u.iter.i++; }
        return 1;
    }
    if (of.t == T_OBJ) {
        /* an object with a next(): an iterator made by a script or a library */
        Val nx = get_prop(vm, of, S_next), r;
        if (vm->throwing) return 0;
        if (!is_callable(nx)) {
            /* array-like: a length and numbered items */
            Val len = get_prop(vm, of, S_length);
            f64 n = to_num(vm, len);
            if (vm->throwing || !(it->u.iter.i < n)) return 0;
            *out = get_elem(vm, of, v_num(it->u.iter.i++));
            return !vm->throwing;
        }
        r = vm_call(vm, nx, of, 0, 0);
        if (vm->throwing || r.t != T_OBJ) return 0;
        if (r.u.o->cls == C_PROMISE) { throw_err(vm, E_TYPE, "that gives promises: it is gone through with for await"); return 0; }
        TEMP(vm, r);
        if (truthy(get_prop(vm, r, S_done))) { UNTEMP(vm, 1); return 0; }
        *out = get_prop(vm, r, S_value);
        UNTEMP(vm, 1);
        return !vm->throwing;
    }
    if (!IS_NULLISH(of)) throw_err(vm, E_TYPE, "that cannot be gone through with for-of");
    return 0;
}

Obj *vm_iter_new(Vm *vm, Val of)
{
    if (IS_NULLISH(of)) { throw_err(vm, E_TYPE, "cannot go through null or undefined"); return 0; }
    return iter_new(vm, of, 0);
}
int vm_iter_next(Vm *vm, Obj *it, Val *out) { return iter_next(vm, it, out); }

static Obj *make_closure(Vm *vm, Proto *p, Frame *f)
{
    Obj *fn = obj_new(vm, vm->function_proto, C_FUNC);
    if (!fn) return 0;
    fn->flags = OF_CALLABLE | (p->arrow ? OF_ARROW : OF_CTOR);
    fn->u.fn.fn = p;
    fn->u.fn.env = f->env;
    fn->u.fn.self = p->arrow ? f->self : v_undef();
    return fn;
}

static void spread_into(Vm *vm, Val target, Val src)
{
    Obj *t = target.u.o;
    if (t->cls == C_ARRAY) {
        Obj *it;
        Val v;
        if (src.t == T_OBJ && src.u.o->cls == C_ARRAY) {
            u32 i, n = src.u.o->u.arr.len;
            for (i = 0; i < n; i++) if (!arr_push(vm, t, arr_get(src.u.o, i))) return;
            return;
        }
        if (IS_NULLISH(src)) { throw_err(vm, E_TYPE, "cannot spread null or undefined"); return; }
        it = iter_new(vm, src, 0);
        if (!it) return;
        TEMP(vm, v_obj(it));
        while (iter_next(vm, it, &v)) arr_push(vm, t, v);
        UNTEMP(vm, 1);
        return;
    }
    if (src.t == T_OBJ) {                     /* { ...o }                 */
        Obj *keys = own_keys(vm, src.u.o, 0);
        u32 i;
        if (!keys) return;
        TEMP(vm, v_obj(keys));
        for (i = 0; i < keys->u.arr.len && !vm->throwing; i++) {
            Val k = arr_get(keys, i);
            Val v = get_elem(vm, src, k);
            if (vm->throwing) break;
            prop_put(vm, t, to_key(vm, k), v, 0);
        }
        UNTEMP(vm, 1);
    } else if (src.t == T_STR) {
        u32 i;
        for (i = 0; i < src.u.s->len; i++) prop_put(vm, t, str_intern(num_to_str((f64)i, 10)), v_str(str_from16(&src.u.s->c[i], 1)), 0);
    }
}

/* ---- the loop ----------------------------------------------------------------------- */
#define RD8()   (code[f->pc++])
#define RD16()  (f->pc += 2, (u32)code[f->pc - 2] | ((u32)code[f->pc - 1] << 8))
#define RD32()  (f->pc += 4, (s32)((u32)code[f->pc - 4] | ((u32)code[f->pc - 3] << 8) | ((u32)code[f->pc - 2] << 16) | ((u32)code[f->pc - 1] << 24)))
#define LOAD()  do { f = &vm->frames[vm->nframes - 1]; code = f->p->code; k = f->p->consts ? f->p->consts->v : 0; } while (0)
#define CHECK() do { if (vm->throwing) goto thrown; } while (0)

static Val run(Vm *vm)
{
    Frame *f;
    u8 *code;
    Val *k;
    LOAD();
    if (vm->throwing) goto thrown;        /* an async function, resumed with a rejection */
    for (;;) {
        u32 op;
        if ((++vm->steps & 1023) == 0) {
            if (!vm_check_time(vm)) goto thrown;
            gc_maybe(vm);
        } else if (g_gc_stress && (vm->steps & 15) == 0) gc_collect(vm);
        if (vm->sp + 32 > VM_STACK) { throw_err(vm, E_RANGE, "too much recursion"); goto thrown; }
        op = code[f->pc++];
        switch (op) {
        case OP_UNDEF: PUSH(v_undef()); break;
        case OP_NULL: PUSH(v_null()); break;
        case OP_TRUE: PUSH(v_bool(1)); break;
        case OP_FALSE: PUSH(v_bool(0)); break;
        case OP_INT8: PUSH(v_num((f64)(signed char)RD8())); break;
        case OP_CONST: { u32 i = RD16(); PUSH(k[i]); break; }
        case OP_POP: vm->sp--; break;
        case OP_DUP: PUSH(TOP); break;
        case OP_DUP2: { Val a = AT(1), b = AT(0); PUSH(a); PUSH(b); break; }
        case OP_SWAP: { Val t = AT(0); AT(0) = AT(1); AT(1) = t; break; }
        case OP_ROT3: { Val c = AT(0); AT(0) = AT(1); AT(1) = AT(2); AT(2) = c; break; }
        case OP_ROT4: { Val d = AT(0); AT(0) = AT(1); AT(1) = AT(2); AT(2) = AT(3); AT(3) = d; break; }

        case OP_GETVAR: {
            u32 d = RD8(), s = RD16();
            Env *e = f->env;
            while (d--) e = e->parent;
            PUSH(e->v[s]);
            break;
        }
        case OP_SETVAR: {
            u32 d = RD8(), s = RD16();
            Env *e = f->env;
            while (d--) e = e->parent;
            e->v[s] = TOP;
            break;
        }
        case OP_GETGLOBAL: {
            u32 i = RD16(); int found;
            Val v;
            f->pc++;
            v = get_global(vm, k[i].u.s, &found);
            CHECK();
            if (!found) {
                char b[80]; u32 n = str_to_utf8(k[i].u.s, b, 60); b[n] = 0;
                throw_err(vm, E_REFERENCE, "%s is not defined", b);
                goto thrown;
            }
            PUSH(v);
            break;
        }
        case OP_TYPEOF_GLOBAL: {
            u32 i = RD16(); int found;
            Val v;
            f->pc++;
            v = get_global(vm, k[i].u.s, &found);
            CHECK();
            PUSH(v);
            break;
        }
        case OP_SETGLOBAL: {
            u32 i = RD16();
            f->pc++;
            set_prop(vm, v_obj(vm->global), k[i].u.s, TOP);
            CHECK();
            break;
        }
        case OP_DECLGLOBAL: {
            u32 i = RD16();
            if (!prop_find(vm->global, k[i].u.s)) prop_put(vm, vm->global, k[i].u.s, v_undef(), 0);
            break;
        }
        case OP_GETPROP: {
            u32 i = RD16();
            Val r = get_prop(vm, TOP, k[i].u.s);
            CHECK();
            TOP = r;
            if (r.t == T_UNDEF) { vm->miss_name = k[i].u.s; vm->miss_at = vm->sp - 1; }
            break;
        }
        case OP_SETPROP: {
            u32 i = RD16();
            set_prop(vm, AT(1), k[i].u.s, AT(0));
            CHECK();
            AT(1) = AT(0); vm->sp--;
            break;
        }
        case OP_GETELEM: {
            Val r = get_elem(vm, AT(1), AT(0));
            CHECK();
            AT(1) = r; vm->sp--;
            break;
        }
        case OP_SETELEM: {
            set_elem(vm, AT(2), AT(1), AT(0));
            CHECK();
            AT(2) = AT(0); vm->sp -= 2;
            break;
        }
        case OP_DELPROP: { u32 i = RD16(); int r = del_prop(vm, TOP, v_str(k[i].u.s)); CHECK(); TOP = v_bool(r); break; }
        case OP_DELELEM: { int r = del_prop(vm, AT(1), AT(0)); CHECK(); AT(1) = v_bool(r); vm->sp--; break; }

        case OP_ADD: {
            Val r;
            if (!add(vm, &AT(1), &AT(0), &r)) { if (!vm->throwing) vm_check_time(vm); goto thrown; }
            AT(1) = r; vm->sp--;
            break;
        }
        case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD: case OP_EXP: {
            f64 a, b, r;
            if (AT(1).t == T_NUM && AT(0).t == T_NUM) { a = AT(1).u.n; b = AT(0).u.n; }
            else { a = to_num(vm, AT(1)); CHECK(); b = to_num(vm, AT(0)); CHECK(); }
            switch (op) {
            case OP_SUB: r = a - b; break;
            case OP_MUL: r = a * b; break;
            case OP_DIV: r = a / b; break;
            case OP_MOD: r = m_fmod(a, b); break;
            default: r = m_pow(a, b);
            }
            AT(1) = v_num(r); vm->sp--;
            break;
        }
        case OP_AND: case OP_OR: case OP_XOR: case OP_SHL: case OP_SHR: case OP_USHR: {
            f64 a = to_num(vm, AT(1)), b;
            u32 x, y;
            CHECK();
            b = to_num(vm, AT(0));
            CHECK();
            x = to_uint32(a); y = to_uint32(b);
            switch (op) {
            case OP_AND: AT(1) = v_num((s32)(x & y)); break;
            case OP_OR: AT(1) = v_num((s32)(x | y)); break;
            case OP_XOR: AT(1) = v_num((s32)(x ^ y)); break;
            case OP_SHL: AT(1) = v_num((s32)(x << (y & 31))); break;
            case OP_SHR: AT(1) = v_num((s32)x >> (y & 31)); break;
            default: AT(1) = v_num((f64)(x >> (y & 31)));
            }
            vm->sp--;
            break;
        }
        case OP_NEG: { f64 a = to_num(vm, TOP); CHECK(); TOP = v_num(-a); break; }
        case OP_PLUS: { f64 a = to_num(vm, TOP); CHECK(); TOP = v_num(a); break; }
        case OP_INC: { f64 a = to_num(vm, TOP); CHECK(); TOP = v_num(a + 1); break; }
        case OP_DEC: { f64 a = to_num(vm, TOP); CHECK(); TOP = v_num(a - 1); break; }
        case OP_NOT: TOP = v_bool(!truthy(TOP)); break;
        case OP_BITNOT: { f64 a = to_num(vm, TOP); CHECK(); TOP = v_num((s32)~to_uint32(a)); break; }
        case OP_EQ: case OP_NE: { int r = loose_eq(vm, AT(1), AT(0)); CHECK(); AT(1) = v_bool(op == OP_EQ ? r : !r); vm->sp--; break; }
        case OP_SEQ: { int r = strict_eq(AT(1), AT(0)); AT(1) = v_bool(r); vm->sp--; break; }
        case OP_SNE: { int r = strict_eq(AT(1), AT(0)); AT(1) = v_bool(!r); vm->sp--; break; }
        case OP_LT: case OP_LE: case OP_GT: case OP_GE: {
            int r = compare(vm, &AT(1), &AT(0), (int)op);
            if (r < 0) goto thrown;
            AT(1) = v_bool(r); vm->sp--;
            break;
        }
        case OP_INSTANCEOF: { int r = instance_of(vm, AT(1), AT(0)); CHECK(); AT(1) = v_bool(r); vm->sp--; break; }
        case OP_IN: {
            Str *key;
            if (AT(0).t != T_OBJ) { throw_err(vm, E_TYPE, "'in' needs an object on its right"); goto thrown; }
            key = to_key(vm, AT(1));
            CHECK();
            if (!key) { vm_check_time(vm); goto thrown; }
            AT(1) = v_bool(has_prop(vm, AT(0).u.o, key)); vm->sp--;
            break;
        }
        case OP_TYPEOF: TOP = v_str(type_of(TOP)); break;
        case OP_TOSTR: { Str *s = to_str(vm, TOP); CHECK(); TOP = v_str(s); break; }
        case OP_VOID: TOP = v_undef(); break;

        case OP_JMP: { s32 off = RD32(); if (off < 0) { if (!vm_check_time(vm)) goto thrown; } f->pc += (u32)off; break; }
        case OP_JF: { s32 off = RD32(); if (!truthy(TOP)) f->pc += (u32)off; vm->sp--; break; }
        case OP_JT: { s32 off = RD32(); if (truthy(TOP)) f->pc += (u32)off; vm->sp--; break; }
        case OP_JFKEEP: { s32 off = RD32(); if (!truthy(TOP)) f->pc += (u32)off; break; }
        case OP_JTKEEP: { s32 off = RD32(); if (truthy(TOP)) f->pc += (u32)off; break; }
        case OP_JNNKEEP: { s32 off = RD32(); if (!IS_NULLISH(TOP)) f->pc += (u32)off; break; }
        case OP_OPTCHAIN: { s32 off = RD32(); if (IS_NULLISH(TOP)) { TOP = v_undef(); f->pc += (u32)off; } break; }

        case OP_CLOSURE: {
            u32 i = RD16();
            Obj *fn = make_closure(vm, f->p->kids[i], f);
            if (!fn) { vm_check_time(vm); goto thrown; }
            PUSH(v_obj(fn));
            break;
        }
        case OP_CALL: case OP_CALLM: {
            int argc = (int)RD8();
            u32 base = vm->sp - (u32)argc - (op == OP_CALLM ? 2u : 1u);
            Val fnv = AT(argc), self = op == OP_CALLM ? vm->stack[base] : v_undef();
            if (!is_callable(fnv)) {
                if (fnv.t == T_UNDEF && vm->miss_name && vm->miss_at == vm->sp - (u32)argc - 1u && vm->miss_name->len < 60) {
                    char nm[64]; u32 j;                /* the method read just for this call: named */
                    for (j = 0; j < vm->miss_name->len; j++) nm[j] = vm->miss_name->c[j] < 128 ? (char)vm->miss_name->c[j] : '?';
                    nm[j] = 0;
                    throw_err(vm, E_TYPE, "%s is not a function: it does not exist", nm);
                } else throw_err(vm, E_TYPE, fnv.t == T_UNDEF ? "a function that does not exist was called" : "something that is not a function was called");
                goto thrown;
            }
            if (fnv.u.o->cls == C_FUNC) {
                if (!enter(vm, fnv.u.o, self, &vm->stack[vm->sp - (u32)argc], argc, base, 0, 0)) goto thrown;
                LOAD();
                if ((vm->steps & 255) == 0) gc_maybe(vm);
            } else {
                Val r = call_other(vm, fnv.u.o, self, &vm->stack[vm->sp - (u32)argc], argc, 0);
                CHECK();
                vm->stack[base] = r;
                vm->sp = base + 1;
            }
            break;
        }
        case OP_CALLSPREAD: {
            int method = (int)RD8();
            Obj *list = AT(0).u.o;
            u32 base = vm->sp - 1u - (method ? 2u : 1u), i, n = list->u.arr.len;
            Val fnv = vm->stack[base + (method ? 1u : 0u)], self = method ? vm->stack[base] : v_undef();
            if (!is_callable(fnv)) { throw_err(vm, E_TYPE, "something that is not a function was called"); goto thrown; }
            if (n > 1000 || vm->sp + n + 8 > VM_STACK) { throw_err(vm, E_RANGE, "too many arguments"); goto thrown; }
            for (i = 0; i < n; i++) PUSH(arr_get(list, i));
            if (fnv.u.o->cls == C_FUNC) {
                if (!enter(vm, fnv.u.o, self, &vm->stack[vm->sp - n], (int)n, base, 0, 0)) goto thrown;
                LOAD();
            } else {
                Val r = call_other(vm, fnv.u.o, self, &vm->stack[vm->sp - n], (int)n, 0);
                CHECK();
                vm->stack[base] = r;
                vm->sp = base + 1;
            }
            break;
        }
        case OP_NEW: {
            int argc = (int)RD8();
            u32 base = vm->sp - (u32)argc - 1u;
            Val fnv = vm->stack[base], r;
            if (!is_callable(fnv) || (fnv.u.o->flags & OF_ARROW)) { throw_err(vm, E_TYPE, "something that is not a constructor was used with new"); goto thrown; }
            if (fnv.u.o->cls == C_FUNC) {
                Val pv = get_prop(vm, fnv, S_prototype);
                Obj *self;
                CHECK();
                self = obj_new(vm, pv.t == T_OBJ ? pv.u.o : vm->object_proto, C_OBJECT);
                if (!self) { vm_check_time(vm); goto thrown; }
                vm->stack[base] = v_obj(self);  /* keeps it safe; enter moves sp to base */
                if (!enter(vm, fnv.u.o, v_obj(self), &vm->stack[vm->sp - (u32)argc], argc, base, 1, 0)) goto thrown;
                vm->frames[vm->nframes - 1].self = v_obj(self);
                LOAD();
            } else {
                r = call_other(vm, fnv.u.o, v_undef(), &vm->stack[vm->sp - (u32)argc], argc, 1);
                CHECK();
                vm->stack[base] = r;
                vm->sp = base + 1;
            }
            break;
        }
        case OP_RET: case OP_RETUNDEF: {
            Val r = op == OP_RET ? TOP : v_undef();
            int stop = f->stop;
            if (f->is_new && r.t != T_OBJ) r = f->self;
            if (f->co && f->co->cls == C_GEN) {   /* a generator, finished */
                Obj *g = f->co;
                g->u.co.prom = v_num(2);
                vm->sp = f->base;
                vm->nframes--;
                TEMP(vm, v_obj(g));
                r = iter_result(vm, r, 1);
                UNTEMP(vm, 1);
                if (vm->fatal) return v_undef();
                if (stop) return r;
                PUSH(r);
                LOAD();
                break;
            }
            if (f->co) {                     /* async: the value settles its promise */
                Obj *co = f->co;
                TEMP(vm, v_obj(co)); TEMP(vm, r);
                vm->sp = f->base;
                vm->nframes--;
                promise_resolve_fn(vm, co->u.co.prom.u.o, r, 0);
                UNTEMP(vm, 2);
                r = co->u.co.prom;
                if (vm->fatal) return v_undef();
                vm->throwing = 0;
                if (stop) return r;
                PUSH(r);
                LOAD();
                break;
            }
            vm->sp = f->base;
            vm->nframes--;
            if (stop) return r;
            PUSH(r);
            LOAD();
            break;
        }
        case OP_THIS: PUSH(f->self); break;
        case OP_ARRAY: { Obj *a = arr_new(vm, 0); if (!a) { vm_check_time(vm); goto thrown; } PUSH(v_obj(a)); break; }
        case OP_APPEND: { if (!arr_push(vm, AT(1).u.o, AT(0))) { vm_check_time(vm); goto thrown; } vm->sp--; break; }
        case OP_SPREAD: spread_into(vm, AT(1), AT(0)); CHECK(); vm->sp--; break;
        case OP_LISTOF:
            if (TOP.t != T_OBJ || (TOP.u.o->cls != C_ARRAY && TOP.u.o->cls != C_ARGS)) {
                Obj *a;
                if (IS_NULLISH(TOP)) { throw_err(vm, E_TYPE, "cannot destructure null or undefined"); goto thrown; }
                a = arr_new(vm, 0);
                if (!a) { vm_check_time(vm); goto thrown; }
                PUSH(v_obj(a));
                spread_into(vm, AT(0), AT(1));
                CHECK();
                AT(1) = AT(0); vm->sp--;
            }
            break;
        case OP_OBJECT: { Obj *o = obj_new(vm, vm->object_proto, C_OBJECT); if (!o) { vm_check_time(vm); goto thrown; } PUSH(v_obj(o)); break; }
        case OP_INITPROP: case OP_INITHIDDEN: {
            u32 i = RD16();
            if (AT(1).t != T_OBJ) { throw_err(vm, E_TYPE, "a field set on something that is not an object"); goto thrown; }
            if (!prop_put(vm, AT(1).u.o, k[i].u.s, AT(0), op == OP_INITHIDDEN ? P_HIDDEN : 0)) { vm_check_time(vm); goto thrown; }
            if (AT(0).t == T_OBJ && AT(0).u.o->cls == C_FUNC && !AT(0).u.o->u.fn.fn->name) { /* a method's name: from its key */ }
            vm->sp--;
            break;
        }
        case OP_INITELEM: {
            Str *key;
            if (AT(2).t != T_OBJ) { throw_err(vm, E_TYPE, "a field set on something that is not an object"); goto thrown; }
            key = to_key(vm, AT(1));
            CHECK();
            if (!key || !prop_put(vm, AT(2).u.o, key, AT(0), 0)) { vm_check_time(vm); goto thrown; }
            vm->sp -= 2;
            break;
        }
        case OP_INITGET: case OP_INITSET: case OP_INITGETELEM: case OP_INITSETELEM: {
            /* get x() {} -- or get [key]() {}, the key on the stack below it */
            int elem = op == OP_INITGETELEM || op == OP_INITSETELEM;
            Str *key;
            Obj *o;
            Prop *q;
            if (elem) { key = to_key(vm, AT(1)); CHECK(); if (!key) { vm_check_time(vm); goto thrown; } }
            else key = k[RD16()].u.s;
            if (AT(elem ? 2 : 1).t != T_OBJ) { throw_err(vm, E_TYPE, "a getter set on something that is not an object"); goto thrown; }
            o = AT(elem ? 2 : 1).u.o;
            q = prop_find(o, key);
            if (!q || !(q->flags & P_ACCESSOR)) {
                if (!prop_put(vm, o, key, v_undef(), P_ACCESSOR | (o->cls == C_FUNC || o->proto != vm->object_proto ? P_HIDDEN : 0))) { vm_check_time(vm); goto thrown; }
                q = prop_find(o, key);
                q->set = v_undef();
            }
            if (op == OP_INITGET || op == OP_INITGETELEM) q->v = AT(0); else q->set = AT(0);
            vm->sp -= elem ? 2 : 1;
            break;
        }
        case OP_SETPROTO: {
            if (AT(1).t == T_OBJ && (AT(0).t == T_OBJ || AT(0).t == T_NULL)) {
                Obj *p, *o = AT(1).u.o;
                int loop = 0;
                for (p = AT(0).t == T_OBJ ? AT(0).u.o : 0; p; p = p->proto) if (p == o) loop = 1;
                if (!loop) o->proto = AT(0).t == T_OBJ ? AT(0).u.o : 0;
            }
            vm->sp--;
            break;
        }
        case OP_THROW: throw_val(vm, TOP); vm->sp--; goto thrown;
        case OP_TRY: {
            s32 off = RD32();
            if (f->ntry >= VM_TRIES) { throw_err(vm, E_RANGE, "try blocks nested too deep"); goto thrown; }
            f->tries[f->ntry].pc = f->pc + (u32)off;
            f->tries[f->ntry].sp = vm->sp;
            f->ntry++;
            break;
        }
        case OP_ENDTRY: if (f->ntry) f->ntry--; break;
        case OP_FORIN: case OP_FOROF: {
            Obj *it = iter_new(vm, TOP, op == OP_FORIN);
            CHECK();
            if (!it) { vm_check_time(vm); goto thrown; }
            if (op == OP_FOROF && IS_NULLISH(TOP)) { throw_err(vm, E_TYPE, "for-of over null or undefined"); goto thrown; }
            TOP = v_obj(it);
            break;
        }
        case OP_FORAWAIT: {                  /* for await: an async iterator, or for-of's own walk */
            Val mk;
            if (IS_NULLISH(TOP)) { throw_err(vm, E_TYPE, "for await over null or undefined"); goto thrown; }
            mk = TOP.t == T_OBJ ? get_prop(vm, TOP, atom("@@asyncIterator")) : v_undef();
            CHECK();
            if (is_callable(mk)) {
                Val it = vm_call(vm, mk, TOP, 0, 0);
                CHECK();
                if (it.t != T_OBJ) { throw_err(vm, E_TYPE, "[Symbol.asyncIterator]() gave something that is not an object"); goto thrown; }
                TOP = it;
            } else {
                Obj *it = iter_new(vm, TOP, 0);
                CHECK();
                if (!it) { vm_check_time(vm); goto thrown; }
                TOP = v_obj(it);
            }
            break;
        }
        case OP_ANEXT: {                     /* the iterator stays; next()'s answer, to be awaited, goes on top */
            Obj *it = TOP.u.o;
            Val r;
            if (it->cls == C_ITER || it->cls == C_MAPITER) {
                Val v = v_undef();
                int more = iter_next(vm, it, &v);
                CHECK();
                r = iter_result(vm, v, !more);
            } else {
                Val nx = get_prop(vm, TOP, S_next);
                CHECK();
                r = vm_call(vm, nx, TOP, 0, 0);
            }
            CHECK();
            PUSH(r);
            break;
        }
        case OP_ITERDONE: {                  /* {value, done}: done, out of the loop; else its value */
            s32 off = RD32();
            Val r = TOP;
            if (r.t != T_OBJ) { throw_err(vm, E_TYPE, "an iterator's next() gave something that is not an object"); goto thrown; }
            if (truthy(get_prop(vm, r, S_done))) { CHECK(); vm->sp--; f->pc += (u32)off; break; }
            CHECK();
            TOP = get_prop(vm, r, S_value);
            CHECK();
            break;
        }
        case OP_ITERNEXT: {
            s32 off = RD32();
            Val v;
            if (!iter_next(vm, TOP.u.o, &v)) { CHECK(); f->pc += (u32)off; break; }
            CHECK();
            PUSH(v);
            break;
        }
        case OP_REGEXP: {
            u32 a = RD16(), b = RD16();
            Obj *re = re_new(vm, k[a].u.s, k[b].u.s);
            CHECK();
            if (!re) { vm_check_time(vm); goto thrown; }
            PUSH(v_obj(re));
            break;
        }
        case OP_AWAIT: {
            Obj *co = f->co, *pr, *ok, *bad;
            Vals *st;
            GcHdr *raw;
            Frame *saved;
            Val v = TOP;
            u32 n, i;
            int stop;
            if (!co || co->cls != C_ASYNC) { throw_err(vm, E_SYNTAX, "await outside an async function"); goto thrown; }
            n = vm->sp - 1 - f->base;        /* what it had on the stack, below v */
            st = gc_alloc(sizeof(Vals) + n * sizeof(Val), K_VALS);
            if (!st) { vm->out_of_room = 1; vm_check_time(vm); goto thrown; }
            st->n = n;
            for (i = 0; i < n; i++) st->v[i] = vm->stack[f->base + i];
            co->u.co.stack = st;
            raw = gc_alloc(sizeof(GcHdr) + sizeof(Frame), K_BYTES);
            if (!raw) { vm->out_of_room = 1; vm_check_time(vm); goto thrown; }
            f = &vm->frames[vm->nframes - 1];
            saved = (Frame *)(raw + 1);
            *saved = *f;
            for (i = 0; i < saved->ntry; i++) saved->tries[i].sp -= f->base;
            co->u.co.fr = saved;
            /* what it waits for, as a promise, and what to do when it settles */
            pr = promise_new(vm);
            if (!pr) { vm_check_time(vm); goto thrown; }
            TEMP(vm, v_obj(pr));
            ok = native_new(vm, "await", async_step, 1, 0);
            if (ok) { TEMP(vm, v_obj(ok)); prop_put(vm, ok, atom("%co"), v_obj(co), P_HIDDEN); }
            bad = ok ? native_new(vm, "await", async_step, 1, 1) : 0;
            if (bad) { TEMP(vm, v_obj(bad)); prop_put(vm, bad, atom("%co"), v_obj(co), P_HIDDEN); }
            if (!ok || !bad) { UNTEMP(vm, ok ? 2u : 1u); vm_check_time(vm); goto thrown; }
            promise_then(vm, pr, v_obj(ok), v_obj(bad));
            promise_resolve_fn(vm, pr, v, 0);
            UNTEMP(vm, 3);
            if (vm->fatal) goto thrown;
            vm->throwing = 0;
            /* leave the frame: the caller gets the promise */
            f = &vm->frames[vm->nframes - 1];
            stop = f->stop;
            {
                Val r = co->u.co.prom;
                vm->sp = f->base;
                vm->nframes--;
                if (stop) return r;
                PUSH(r);
            }
            LOAD();
            break;
        }
        case OP_GENSTART: case OP_YIELD: case OP_AWAITY: {
            Obj *g;
            Val r, v = op != OP_GENSTART ? TOP : v_undef();
            int stop;
            if (op == OP_GENSTART) {
                g = obj_new(vm, vm->gen_proto, C_GEN);
                if (!g) { vm_check_time(vm); goto thrown; }
                g->u.co.prom = v_num(0); g->u.co.stack = 0; g->u.co.fr = 0;
            } else {
                g = f->co;
                if (!g || g->cls != C_GEN) { throw_err(vm, E_SYNTAX, "yield outside a generator"); goto thrown; }
            }
            TEMP(vm, v_obj(g)); TEMP(vm, v);
            if (!gen_pack(vm, g, op != OP_GENSTART ? 1 : 0)) { UNTEMP(vm, 2); vm_check_time(vm); goto thrown; }
            g->u.co.prom = v_num(0);
            if (op == OP_AWAITY) vm->gen_awaited = 1;   /* for agen_step: wait on v, then go on */
            r = op != OP_GENSTART ? iter_result(vm, v, 0) : f->p->gen == 2 ? agen_new(vm, g) : v_obj(g);
            UNTEMP(vm, 2);
            if (vm->throwing || vm->fatal) goto thrown;
            f = &vm->frames[vm->nframes - 1];
            stop = f->stop;
            vm->sp = f->base;
            vm->nframes--;
            if (stop) return r;
            PUSH(r);
            LOAD();
            break;
        }
        case OP_DEBUG: case OP_TEMPLATE: break;
        default:
            throw_err(vm, E_ERROR, "a broken instruction");
            vm->fatal = 1;
            goto thrown;
        }
        continue;

    thrown:
        /* Find a catch: in this frame, else the caller's, but never past a
         * frame C is waiting on.  Out of time or room: no catch at all. */
        for (;;) {
            f = &vm->frames[vm->nframes - 1];
            if (f->ntry && !vm->fatal) {
                Try t = f->tries[--f->ntry];
                vm->sp = t.sp;
                vm->throwing = 0;
                PUSH(vm->exception);
                f->pc = t.pc;
                LOAD();
                break;
            }
            if (f->co && f->co->cls == C_ASYNC && !vm->fatal) {   /* async: the throw rejects its promise */
                Obj *co = f->co;
                Val e = vm->exception, r;
                int stop = f->stop;
                vm->throwing = 0;
                TEMP(vm, v_obj(co)); TEMP(vm, e);
                vm->sp = f->base;
                vm->nframes--;
                promise_resolve_fn(vm, co->u.co.prom.u.o, e, 1);
                UNTEMP(vm, 2);
                r = co->u.co.prom;
                if (stop) return r;
                PUSH(r);
                LOAD();
                break;
            }
            {
                int stop = f->stop;
                vm->sp = f->base;
                vm->nframes--;
                if (stop) return v_undef();
            }
        }
    }
}

/* ---- tasks ------------------------------------------------------------------------- */
Val vm_run_script(Vm *vm, Proto *p)
{
    Obj *fn = obj_new(vm, vm->function_proto, C_FUNC);
    Env *env;
    Frame *f;
    u32 i;
    if (!fn) { vm_check_time(vm); return v_undef(); }
    fn->flags = OF_CALLABLE;
    fn->u.fn.fn = p;
    env = gc_alloc(sizeof(Env) + p->nslots * sizeof(Val), K_ENV);
    if (!env) { vm->out_of_room = 1; vm_check_time(vm); return v_undef(); }
    env->n = p->nslots;
    for (i = 0; i < env->n; i++) env->v[i] = v_undef();
    if (vm->nframes >= VM_FRAMES - 1) return throw_err(vm, E_RANGE, "too much recursion");
    PUSH(v_obj(fn));
    f = &vm->frames[vm->nframes++];
    memset(f, 0, sizeof *f);
    f->p = p; f->fn = fn; f->env = env; f->self = v_obj(vm->global);
    f->base = vm->sp - 1;
    f->stop = 1;
    return run(vm);
}

/* Promise reactions waiting their turn (fn, this, arg in threes): run
 * them all, oldest first. */
void vm_run_microtasks(Vm *vm)
{
    u32 guard = 0, at = 0;
    while (vm->microq && at + 3 <= vm->microq->u.arr.len && !vm->fatal && guard++ < 100000) {
        Obj *q = vm->microq;
        Val fn = arr_get(q, at), self = arr_get(q, at + 1), arg = arr_get(q, at + 2);
        at += 3;
        vm_call(vm, fn, self, &arg, 1);
        vm->throwing = 0;                     /* a failed reaction rejects its own promise */
        if (at >= 3000) {                     /* drop what has been done      */
            u32 i, n = q->u.arr.len;
            for (i = at; i < n; i++) q->u.arr.items->v[i - at] = q->u.arr.items->v[i];
            q->u.arr.len -= at;
            at = 0;
        }
    }
    if (vm->microq) {
        Obj *q = vm->microq;
        u32 i, n = q->u.arr.len;
        for (i = at; i < n; i++) q->u.arr.items->v[i - at] = q->u.arr.items->v[i];
        q->u.arr.len = n >= at ? n - at : 0;
    }
}
