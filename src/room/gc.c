/* ===========================================================================
 * gc.c -- the script room's heap, and the sweeper that keeps it tidy.
 *
 * Every string, object and list a script makes is a block in one stretch
 * of memory.  A block starts with its size and kind, so the heap can be
 * walked from one end to the other.  Freed blocks go on lists by size and
 * are handed out again.
 *
 * Sweeping is mark-and-sweep: start from what the script can certainly
 * reach (its globals, the values on the machine's stack, the call in
 * progress), mark everything reachable from those, then walk the heap
 * and free every block left unmarked.  It never moves anything, so no
 * pointer anywhere has to be fixed up afterwards.
 *
 * The sweeper only runs between instructions (gc_maybe), never in the
 * middle of making something, so C code that is halfway through building
 * an object never has it freed under its feet.  C code that calls back
 * into a script (Array map, sort) keeps what it holds on vm->temps.
 * ===========================================================================
 */
#include "js.h"

static u8  *s_base, *s_top, *s_end;
static u32  s_live;                      /* bytes in blocks in use           */
static u32  s_since;                     /* bytes handed out since the last sweep */

/* Free lists: exact sizes up to 128 bytes in steps of 8, then by powers
 * of two. */
#define SMALL 16
#define NBIG  14
static GcHdr *s_small[SMALL + 1];        /* [size/8], 16..128                */
static GcHdr *s_big[NBIG];               /* 256, 512, ... and the rest       */
#define NEXT(h) (*(GcHdr **)((h) + 1))

static int big_class(u32 size)
{
    int c = 0;
    u32 s = 256;
    while (c < NBIG - 1 && size > s) { s <<= 1; c++; }
    return c;
}

static void free_put(GcHdr *h)
{
    h->kind = K_FREE;
    h->mark = 0;
    if (h->size <= 128) { NEXT(h) = s_small[h->size >> 3]; s_small[h->size >> 3] = h; }
    else { int c = big_class(h->size); NEXT(h) = s_big[c]; s_big[c] = h; }
}

void gc_init(u8 *base, u32 size)
{
    int i;
    s_base = s_top = (u8 *)(((unsigned long)base + 7) & ~7ul);
    s_end = base + size;
    s_live = s_since = 0;
    for (i = 0; i <= SMALL; i++) s_small[i] = 0;
    for (i = 0; i < NBIG; i++) s_big[i] = 0;
}

u32 gc_used(void) { return s_live; }

/* A block given back at once, by code that knows nothing else points at
 * it: the compiler's buffers as they grow and once a function is done.
 * Compiling never sweeps, so without this a big script filled the heap
 * with its own leftovers before it ran a line. */
void gc_free(void *p)
{
    GcHdr *h = p;
    if (!h || (u8 *)h < s_base || (u8 *)h >= s_top || h->kind == K_FREE) return;
    s_live -= h->size;
    free_put(h);
}
u32 gc_total(void) { return (u32)(s_end - s_base); }

static GcHdr *take_big(u32 size)
{
    int c;
    for (c = big_class(size); c < NBIG; c++) {
        GcHdr **pp = &s_big[c];
        while (*pp) {
            GcHdr *h = *pp;
            if (h->size >= size) {
                *pp = NEXT(h);
                if (h->size - size >= 32) {          /* split off the rest */
                    GcHdr *r = (GcHdr *)((u8 *)h + size);
                    r->size = h->size - size;
                    free_put(r);
                    h->size = size;
                }
                return h;
            }
            pp = &NEXT(h);
        }
    }
    return 0;
}

void *gc_alloc(u32 size, int kind)
{
    GcHdr *h = 0;
    size = (size + 7) & ~7u;
    if (size < 16) size = 16;
    if (size > 0x200000) return 0;
    if (size <= 128 && s_small[size >> 3]) {
        h = s_small[size >> 3];
        s_small[size >> 3] = NEXT(h);
    } else if (size > 128) {
        h = take_big(size);
    }
    if (!h) {
        if ((u32)(s_end - s_top) < size) {
            /* A small block that fits nowhere else: a bigger free one will do. */
            if (size <= 128) h = take_big(size);
            if (!h) return 0;
        } else {
            h = (GcHdr *)s_top;
            s_top += size;
            h->size = size;
        }
    }
    memset(h + 1, 0, h->size - sizeof(GcHdr));
    h->kind = (u8)kind;
    h->mark = 0;
    h->extra = 0;
    s_live += h->size;
    s_since += h->size;
    return h;
}

/* ---- marking ------------------------------------------------------------------ */
#define MSTACK 8192
static GcHdr *s_mstack[MSTACK];
static u32    s_msp;
static int    s_overflow;

static void grey(void *p)
{
    GcHdr *h = p;
    if (!h || h->mark) return;
    if ((u8 *)h < s_base || (u8 *)h >= s_top) return;   /* not ours (a C constant) */
    h->mark = 1;
    if (s_msp < MSTACK) s_mstack[s_msp++] = h;
    else s_overflow = 1;
}

static void grey_val(Val v)
{
    if (v.t == T_STR) grey(v.u.s);
    else if (v.t == T_OBJ) grey(v.u.o);
}

static void scan(GcHdr *h)
{
    u32 i;
    h->mark = 2;
    switch (h->kind) {
    case K_OBJ: {
        Obj *o = (Obj *)h;
        grey(o->proto);
        grey(o->props);
        switch (o->cls) {
        case C_ARRAY: case C_ARGS: grey(o->u.arr.items); break;
        case C_FUNC: grey(o->u.fn.fn); grey(o->u.fn.env); grey_val(o->u.fn.self); break;
        case C_NATIVE: grey(o->u.nat.name); break;
        case C_BOUND: grey(o->u.bound.target); grey_val(o->u.bound.self); grey(o->u.bound.args); break;
        case C_REGEXP: grey(o->u.re.src); if (o->u.re.prog) grey((GcHdr *)o->u.re.prog - 1); break;
        case C_PROMISE: grey_val(o->u.prom.value); grey(o->u.prom.reactions); break;
        case C_BOXED: grey_val(o->u.boxed.v); break;
        case C_ITER: grey_val(o->u.iter.of); grey(o->u.iter.keys); break;
        case C_ASYNC: case C_GEN:
            grey_val(o->u.co.prom); grey(o->u.co.stack);
            if (o->u.co.fr) {
                Frame *F = (Frame *)o->u.co.fr;
                grey((GcHdr *)o->u.co.fr - 1);
                grey(F->p); grey(F->fn); grey(F->env); grey_val(F->self);
            }
            break;
        }
        break;
    }
    case K_PROPS: {
        Props *p = (Props *)h;
        for (i = 0; i < p->n; i++) {
            if (!p->p[i].k) continue;
            grey(p->p[i].k);
            grey_val(p->p[i].v);
            grey_val(p->p[i].set);
        }
        break;
    }
    case K_VALS: {
        Vals *v = (Vals *)h;
        for (i = 0; i < v->n; i++) grey_val(v->v[i]);
        break;
    }
    case K_ENV: {
        Env *e = (Env *)h;
        grey(e->parent);
        for (i = 0; i < e->n; i++) grey_val(e->v[i]);
        break;
    }
    case K_PROTO: {
        Proto *p = (Proto *)h;
        if (p->code) grey((GcHdr *)p->code - 1);
        grey(p->consts);
        if (p->kids) grey((GcHdr *)p->kids - 1);
        grey(p->name);
        for (i = 0; i < p->nkids; i++) grey(p->kids[i]);
        break;
    }
    }
}

static void drain(void)
{
    for (;;) {
        while (s_msp) scan(s_mstack[--s_msp]);
        if (!s_overflow) return;
        s_overflow = 0;
        {   /* the stack ran over: find what was left grey by walking the heap */
            u8 *p = s_base;
            while (p < s_top) {
                GcHdr *h = (GcHdr *)p;
                if (h->mark == 1) { if (s_msp < MSTACK) s_mstack[s_msp++] = h; else s_overflow = 1; }
                p += h->size;
            }
        }
    }
}

int g_gc_stress;                         /* tests: sweep at every chance, and spoil what is freed */

void gc_maybe(Vm *vm)
{
    u32 total = gc_total();
    if (g_gc_stress || s_since > total / 4 || s_live > total - total / 4) gc_collect(vm);
}

void gc_collect(Vm *vm)
{
    u32 i;
    u8 *p, *run = 0;

    s_msp = 0; s_overflow = 0;
    for (i = 0; i < vm->sp; i++) grey_val(vm->stack[i]);
    for (i = 0; i < vm->ntemps; i++) grey_val(vm->temps[i]);
    for (i = 0; i < vm->nframes; i++) {
        Frame *f = &vm->frames[i];
        grey(f->p); grey(f->fn); grey(f->env); grey_val(f->self); grey(f->co);
    }
    grey(vm->global);
    grey(vm->object_proto); grey(vm->function_proto); grey(vm->array_proto);
    grey(vm->string_proto); grey(vm->number_proto); grey(vm->boolean_proto);
    grey(vm->error_proto); grey(vm->regexp_proto); grey(vm->date_proto);
    grey(vm->promise_proto); grey(vm->iter_proto); grey(vm->gen_proto);
    for (i = 0; i < 6; i++) grey(vm->err_ctor[i]);
    grey(vm->element_proto); grey(vm->document_obj); grey(vm->style_proto);
    grey(vm->classlist_proto); grey(vm->event_proto);
    grey(vm->hosts); grey(vm->microq); grey(vm->timers);
    grey_val(vm->exception);
    intern_mark();                       /* the names kept forever           */
    drain();
    intern_sweep();                      /* forget interned strings nobody holds */

    /* Sweep: free every unmarked block, joining neighbours. */
    for (i = 0; i <= SMALL; i++) s_small[i] = 0;
    for (i = 0; i < NBIG; i++) s_big[i] = 0;
    s_live = 0;
    p = s_base;
    while (p < s_top) {
        GcHdr *h = (GcHdr *)p;
        u32 size = h->size;
        if (h->kind != K_FREE && h->mark) {
            if (run) {
                GcHdr *r = (GcHdr *)run;
                r->size = (u32)(p - run);
                if (g_gc_stress) memset(r + 1, 0xDD, r->size - sizeof(GcHdr));
                free_put(r);
                run = 0;
            }
            if (h->extra != 1) h->mark = 0;   /* extra 1: kept forever     */
            s_live += size;
        } else if (!run) run = p;
        p += size;
    }
    if (run) s_top = run;                /* free space at the end: back to the open heap */
    s_since = 0;
}
