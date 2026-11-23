/* ===========================================================================
 * compile.c -- following the grammar, and writing instructions.
 *
 * Reading and writing happen together: as each piece of the grammar is
 * recognised, the instructions for it are written straight away, for a
 * stack machine (vm.c): "push this", "add the top two", "jump back".
 *
 * Names are the subtle part.  `x` may be one of this function's own
 * variables, one of an enclosing function's, or a global -- and a var can
 * be used before the line that declares it.  So every name is first
 * written as a global, and noted; when its function is finished, and all
 * its declarations are known, the notes are settled: a name declared
 * there is patched to point at that function's slot, and the rest are
 * passed out to the function around it.  What reaches the top stays
 * global.
 *
 * Functions declared with `function name()` work from anywhere in their
 * scope, before their own line too: each function starts with a jump to
 * a short prologue at its end that makes them, then jumps back.
 * ===========================================================================
 */
#include "js.h"
#include "lex.h"

#define MOD_SPECS   64
#define MOD_IMPORTS 256
#define MOD_EXPORTS 256
enum { EX_LOCAL, EX_FROM, EX_NS_FROM, EX_STAR };   /* export kinds */

/* A module's addresses, for whoever compiled it (engine.c). */
Str *g_mod_spec[MOD_SPECS];
int  g_mod_nspec;

typedef struct { int k; u16 idx; Str *name; } Ex;
enum { X_VAL, X_NAME, X_PROP, X_ELEM };
enum { R_GET, R_SET, R_TYPEOF };

typedef struct {
    struct Fs *fs;                       /* whose code it is in             */
    struct Fs *at;                       /* whose scope is asked next       */
    Proto *p;                            /* once that code is finished      */
    u32 off;
    Str *name;
    u16 depth;
    u8  kind, done;
} Ref;

typedef struct { int fin; u32 fin_pos, fin_line; } Region;

typedef struct Loop {
    struct Loop *up;
    Str *label;
    int loop;                            /* continue is allowed             */
    int sw;                              /* a switch: an unlabelled break ends it */
    int regions;                         /* try regions open when it began  */
    u32 *brk; u32 nbrk, cbrk;
    u32 *cnt; u32 ncnt, ccnt;
} Loop;

typedef struct Fs {
    struct Fs *up;
    u8   *code; u32 n, cap;
    Vals *consts; u32 nconst;
    Proto **kids; u32 nkids, ckids;
    Str **vars; u32 nvars, cvars;
    u16  nparams;
    int  arrow, top, rest;
    int  async_fn;                       /* an async function: await is allowed */
    int  gen_fn;                         /* a generator: yield is allowed    */
    int  args_slot, self_slot;
    Str *name;
    Loop *loop;
    Region reg[24]; int nreg;
    u16 *hk; Str **hn; u32 nh, ch;       /* hoisted functions: kid, name   */
    Str **globals; u32 ng, cg;           /* top level: vars to declare     */
} Fs;

typedef struct {
    Lex   L;
    Tok   tok;
    Fs   *fs;
    Vm   *vm;
    Ref  *refs; u32 nrefs, crefs;
    char *why; u32 whycap;
    void *jb[5];
    Str  *super_name;                    /* the class whose super is in reach */
    int   static_ctx;
    u32   classes;
    Str  *fields_name;                   /* compiling a constructor: its class's field-setting method */
    int   no_in;
    int   next_async;                    /* the next function() is async     */
    int   next_gen;                      /* the next function() is a generator */
    /* a module (js_compile's as_function 2): what it imports and exports */
    int   module;
    Fs   *mod_top;
    Str  *mspec[MOD_SPECS]; int nspec;   /* the addresses it names, in order */
    struct { Str *local, *name; int spec; } imp[MOD_IMPORTS]; int nimp;   /* name 0: the whole namespace */
    struct { Str *name, *local; int spec, kind; } exp[MOD_EXPORTS]; int nexp;
} C;

typedef struct { Lex L; Tok tok; } State;

static void expr(C *c);
static void assign(C *c);
static void statement(C *c);
static void function(C *c, Str *name, int arrow, int is_expr, int method);
static void destructure(C *c, int decl, int kw);
/* await: in an async generator the generator's own pause (vm.c's driver waits) */
#define emit_await(c) emit((c), (c)->fs->gen_fn ? OP_AWAITY : OP_AWAIT)
static void unary(C *c, Ex *e);
static void prologue(C *c, Fs *f);
static Proto *finish(C *c, Fs *f);
static int  import_name(C *c, Str *n, Ex *e);
static void import_statement(C *c);
static void export_statement(C *c);
static void import_expr(C *c, Ex *e);
static void fs_open(C *c, Fs *f, int arrow, int top);

/* ---- trouble -------------------------------------------------------------------- */
static void put(C *c, u32 *n, const char *s) { while (*s && *n + 1 < c->whycap) c->why[(*n)++] = *s++; }

__attribute__((noreturn)) static void fail(C *c, const char *msg)
{
    u32 n = 0, line = c->L.err ? c->L.err_line : c->tok.line;
    char t[12]; int k = 0;
    if (c->L.err) msg = c->L.err;
    put(c, &n, "line ");
    do { t[k++] = (char)('0' + line % 10); line /= 10; } while (line);
    while (k) { char ch[2] = { t[--k], 0 }; put(c, &n, ch); }
    put(c, &n, ": ");
    put(c, &n, msg);
    c->why[n] = 0;
    __builtin_longjmp(c->jb, 1);
}

static void *grow(C *c, void *old, u32 have, u32 *cap, u32 each)
{
    u32 nc = *cap ? *cap * 2 : 16;
    u8 *p = gc_alloc(sizeof(GcHdr) + nc * each, K_BYTES);
    if (!p) fail(c, "the script is too big for the memory");
    p += sizeof(GcHdr);
    if (old) { memcpy(p, old, have * each); gc_free((GcHdr *)old - 1); }
    *cap = nc;
    return p;
}

/* A buffer grow() made, given back. */
static void drop(void *b) { if (b) gc_free((GcHdr *)b - 1); }

/* ---- the words --------------------------------------------------------------- */
static void next(C *c)
{
    lex_next(&c->L, &c->tok);
    if (c->L.err) fail(c, c->L.err);
}

static int is_p(C *c, int p) { return c->tok.t == TK_PUNCT && c->tok.p == p; }
static int is_kw(C *c, int k) { return c->tok.t == TK_IDENT && c->tok.kw == k; }
static int accept(C *c, int p) { if (is_p(c, p)) { next(c); return 1; } return 0; }
static void want(C *c, int p, const char *what) { if (!accept(c, p)) fail(c, what); }

static void save(C *c, State *s) { s->L = c->L; s->tok = c->tok; }
static void load(C *c, State *s) { c->L = s->L; c->tok = s->tok; }

/* Past a bracketed stretch; tok is the opening ( [ or {. */
static void skip_brackets(C *c)
{
    char st[256];
    int d = 0, keep = c->L.skipping;
    c->L.skipping = 1;                       /* inside: only where it ends matters */
    do {
        if (c->tok.t == TK_EOF) fail(c, "a bracket is never closed");
        if (c->tok.t == TK_TEMPLATE && !c->tok.tail) { if (d >= 255) fail(c, "brackets too deep"); st[d++] = '$'; next(c); continue; }
        if (c->tok.t == TK_PUNCT && (c->tok.p == '(' || c->tok.p == '[' || c->tok.p == '{')) {
            if (d >= 255) fail(c, "brackets too deep");
            st[d++] = (char)c->tok.p;
        } else if (c->tok.t == TK_PUNCT && (c->tok.p == ')' || c->tok.p == ']' || c->tok.p == '}')) {
            if (d && st[d - 1] == '$' && c->tok.p == '}') {
                d--;
                lex_template_more(&c->L, &c->tok, c->tok.end);
                if (!c->tok.tail) st[d++] = '$';
                if (!d) c->L.skipping = keep;    /* what follows is read in full */
                next(c);
                continue;
            }
            d--;
        }
        if (!d) c->L.skipping = keep;
        next(c);
    } while (d > 0);
    c->L.skipping = keep;
}

static void skip_one(C *c)
{
    if (is_p(c, '(') || is_p(c, '[') || is_p(c, '{') || (c->tok.t == TK_TEMPLATE && !c->tok.tail)) skip_brackets(c);
    else next(c);
}

/* ---- writing code ------------------------------------------------------------- */
static void emit(C *c, u32 b)
{
    Fs *f = c->fs;
    if (f->n == f->cap) f->code = grow(c, f->code, f->n, &f->cap, 1);
    f->code[f->n++] = (u8)b;
}
static void emit16(C *c, u32 v) { emit(c, v & 0xFF); emit(c, (v >> 8) & 0xFF); }
static void emit32(C *c, u32 v) { emit16(c, v & 0xFFFF); emit16(c, v >> 16); }
static u32 here(C *c) { return c->fs->n; }

static u32 jump(C *c, int op) { emit(c, (u32)op); emit32(c, 0); return here(c) - 4; }
static void patch_to(C *c, u32 at, u32 to)
{
    s32 off = (s32)to - (s32)(at + 4);
    u8 *p = c->fs->code + at;
    p[0] = (u8)off; p[1] = (u8)(off >> 8); p[2] = (u8)(off >> 16); p[3] = (u8)(off >> 24);
}
static void patch(C *c, u32 at) { patch_to(c, at, here(c)); }
static void jump_to(C *c, int op, u32 to) { u32 at = jump(c, op); patch_to(c, at, to); }

static u16 konst(C *c, Val v)
{
    Fs *f = c->fs;
    u32 i;
    for (i = 0; i < f->nconst && i < 64 * 1024; i++) {
        Val k = f->consts->v[i];
        if (k.t != v.t) continue;
        if (v.t == T_STR && (k.u.s == v.u.s || str_eq(k.u.s, v.u.s))) return (u16)i;
        if (v.t == T_NUM && k.u.n == v.u.n && (k.u.n != 0 || 1 / k.u.n == 1 / v.u.n)) return (u16)i;
    }
    if (f->nconst >= 65535) fail(c, "too many constants in one function");
    if (!f->consts || f->nconst == f->consts->n) {
        u32 cap = f->consts ? f->consts->n * 2 : 16;
        Vals *nv = gc_alloc(sizeof(Vals) + cap * sizeof(Val), K_VALS);
        if (!nv) fail(c, "the script is too big for the memory");
        nv->n = cap;
        if (f->consts) { memcpy(nv->v, f->consts->v, f->nconst * sizeof(Val)); gc_free(f->consts); }
        f->consts = nv;
    }
    f->consts->v[f->nconst] = v;
    return (u16)f->nconst++;
}
static u16 kstr(C *c, Str *s) { return konst(c, v_str(str_intern(s))); }

static void push_num(C *c, f64 n)
{
    if (n == (f64)(s32)(s64)n && n >= -128 && n <= 127 && !(n == 0 && 1 / n < 0)) { emit(c, OP_INT8); emit(c, (u32)(s32)n & 0xFF); }
    else { emit(c, OP_CONST); emit16(c, konst(c, v_num(n))); }
}

/* ---- names -------------------------------------------------------------------------- */
static int find_var(Fs *f, Str *n)
{
    u32 i;
    for (i = 0; i < f->nvars; i++) if (f->vars[i] == n) return (int)i;
    return -1;
}

static int declare_local(C *c, Fs *f, Str *n)
{
    int i = find_var(f, n);
    Fs *keep;
    if (i >= 0) return i;
    if (f->nvars >= 65000) fail(c, "too many variables in one function");
    keep = c->fs; c->fs = f;
    if (f->nvars == f->cvars) f->vars = grow(c, f->vars, f->nvars, &f->cvars, sizeof(Str *));
    c->fs = keep;
    f->vars[f->nvars] = n;
    return (int)f->nvars++;
}

/* var, let, const, function: at the top level they are globals. */
static void declare(C *c, Str *n)
{
    Fs *f = c->fs;
    u32 i;
    if (!f->top) { declare_local(c, f, n); return; }
    for (i = 0; i < f->ng; i++) if (f->globals[i] == n) return;
    if (f->ng == f->cg) f->globals = grow(c, f->globals, f->ng, &f->cg, sizeof(Str *));
    f->globals[f->ng++] = n;
}

static void ref(C *c, int kind, Str *name)
{
    u16 k = kstr(c, name);
    Ref *r;
    emit(c, kind == R_GET ? OP_GETGLOBAL : kind == R_SET ? OP_SETGLOBAL : OP_TYPEOF_GLOBAL);
    emit16(c, k); emit(c, 0);
    if (c->nrefs == c->crefs) c->refs = grow(c, c->refs, c->nrefs, &c->crefs, sizeof(Ref));
    r = &c->refs[c->nrefs++];
    r->fs = r->at = c->fs; r->p = 0; r->off = here(c) - 4; r->name = name; r->depth = 0;
    r->kind = (u8)kind; r->done = 0;
}

/* A function is finished: settle the names asked of it. */
static void settle(C *c, Fs *f, Proto *p)
{
    u32 i;
    for (i = 0; i < c->nrefs; i++) {
        Ref *r = &c->refs[i];
        int slot;
        if (r->done || r->at != f) continue;
        slot = find_var(f, r->name);
        if (slot < 0 && r->name == g_kw[KW_ARGUMENTS] && !f->arrow && !f->top) {
            slot = declare_local(c, f, r->name);
            f->args_slot = slot;
        }
        if (slot >= 0) {
            u8 *code = r->p ? r->p->code : r->fs->code;
            code[r->off] = r->kind == R_SET ? OP_SETVAR : OP_GETVAR;
            code[r->off + 1] = (u8)r->depth;
            code[r->off + 2] = (u8)slot;
            code[r->off + 3] = (u8)(slot >> 8);
            r->done = 1;
            if (r->depth > 255) fail(c, "functions nested too deep");
        } else if (f->up) {
            r->at = f->up;
            r->depth++;
        } else r->done = 1;                  /* a global, as written         */
    }
    {                                        /* what is done is not kept     */
        u32 k = 0;
        for (i = 0; i < c->nrefs; i++) if (!c->refs[i].done) c->refs[k++] = c->refs[i];
        c->nrefs = k;
    }
}

static void store_name(C *c, Str *n) { ref(c, R_SET, n); }

/* ---- expressions ------------------------------------------------------------------ */
static void value(C *c, Ex *e)
{
    switch (e->k) {
    case X_NAME: ref(c, R_GET, e->name); break;
    case X_PROP: emit(c, OP_GETPROP); emit16(c, e->idx); break;
    case X_ELEM: emit(c, OP_GETELEM); break;
    }
    e->k = X_VAL;
}

/* The value on top goes where e says; it stays on the stack. */
static void store(C *c, Ex *e)
{
    switch (e->k) {
    case X_NAME: store_name(c, e->name); break;
    case X_PROP: emit(c, OP_SETPROP); emit16(c, e->idx); break;
    case X_ELEM: emit(c, OP_SETELEM); break;
    default: fail(c, "that cannot be assigned to");
    }
}

/* For op=: the old value, leaving what store needs underneath. */
static void load_for_update(C *c, Ex *e)
{
    switch (e->k) {
    case X_NAME: ref(c, R_GET, e->name); break;
    case X_PROP: emit(c, OP_DUP); emit(c, OP_GETPROP); emit16(c, e->idx); break;
    case X_ELEM: emit(c, OP_DUP2); emit(c, OP_GETELEM); break;
    default: fail(c, "that cannot be assigned to");
    }
}

static int prop_name(C *c, Str **out)
{
    if (c->tok.t == TK_IDENT || c->tok.t == TK_STR) { *out = str_intern(c->tok.s); next(c); return 1; }
    if (c->tok.t == TK_NUM) { *out = str_intern(num_to_str(c->tok.num, 10)); next(c); return 1; }
    return 0;
}

static void template(C *c)
{
    emit(c, OP_CONST); emit16(c, kstr(c, c->tok.s));
    while (!c->tok.tail) {
        next(c);
        expr(c);
        emit(c, OP_TOSTR);
        emit(c, OP_ADD);
        if (!is_p(c, '}')) fail(c, "a template's ${ is never closed");
        lex_template_more(&c->L, &c->tok, c->tok.end);
        if (c->L.err) fail(c, c->L.err);
        if (c->tok.s->len) { emit(c, OP_CONST); emit16(c, kstr(c, c->tok.s)); emit(c, OP_ADD); }
    }
    next(c);
}

/* tag`a${x}b`: tag(["a", "b"] with .raw, x).  The pieces are read first
 * (skipping what is between them), then the values, as arguments. */
static void tagged_template(C *c, int method)
{
    State s0;
    u32 raw_at[64], raw_n[64];
    Str *cooked[64];
    int n = 0, i, argc;
    save(c, &s0);
    for (;;) {
        u32 from = c->tok.src_at;
        if (n >= 64) fail(c, "a tagged template with too many pieces");
        cooked[n] = c->tok.s;
        raw_at[n] = from;
        raw_n[n] = c->tok.end - (c->tok.tail ? 1 : 2) - from;
        n++;
        if (c->tok.tail) break;
        next(c);
        while (!is_p(c, '}')) { if (c->tok.t == TK_EOF) fail(c, "a template's ${ is never closed"); skip_one(c); }
        lex_template_more(&c->L, &c->tok, c->tok.end);
        if (c->L.err) fail(c, c->L.err);
        c->tok.src_at = c->tok.pos;
    }
    load(c, &s0);
    emit(c, OP_ARRAY);
    for (i = 0; i < n; i++) { emit(c, OP_CONST); emit16(c, kstr(c, cooked[i])); emit(c, OP_APPEND); }
    emit(c, OP_DUP);
    emit(c, OP_ARRAY);
    for (i = 0; i < n; i++) {
        emit(c, OP_CONST); emit16(c, kstr(c, str_from16(c->L.src + raw_at[i], raw_n[i])));
        emit(c, OP_APPEND);
    }
    emit(c, OP_SETPROP); emit16(c, kstr(c, atom("raw")));
    emit(c, OP_POP);
    argc = 1;
    while (!c->tok.tail) {
        next(c);
        assign(c);
        argc++;
        if (!is_p(c, '}')) fail(c, "a template's ${ is never closed");
        lex_template_more(&c->L, &c->tok, c->tok.end);
        if (c->L.err) fail(c, c->L.err);
    }
    next(c);
    emit(c, method ? OP_CALLM : OP_CALL); emit(c, (u32)argc);
}

static void array_literal(C *c)
{
    next(c);
    emit(c, OP_ARRAY);
    while (!is_p(c, ']')) {
        if (is_p(c, ',')) { next(c); emit(c, OP_UNDEF); emit(c, OP_APPEND); continue; }
        if (accept(c, P3('.', '.', '.'))) { assign(c); emit(c, OP_SPREAD); }
        else { assign(c); emit(c, OP_APPEND); }
        if (!is_p(c, ']')) want(c, ',', "expected , or ] in a list");
    }
    next(c);
}

static void object_literal(C *c)
{
    next(c);
    emit(c, OP_OBJECT);
    while (!is_p(c, '}')) {
        Str *name = 0;
        int computed = 0, is_async = 0, is_gen = 0;
        if (accept(c, P3('.', '.', '.'))) { assign(c); emit(c, OP_SPREAD); goto sep; }
        if (c->tok.t == TK_IDENT && (c->tok.kw == KW_GET || c->tok.kw == KW_SET)) {
            int kind = c->tok.kw;
            State s; save(c, &s);
            next(c);
            if (c->tok.t == TK_IDENT || c->tok.t == TK_STR || c->tok.t == TK_NUM || is_p(c, '[')) {
                if (is_p(c, '[')) { next(c); assign(c); want(c, ']', "expected ]"); computed = 1; }
                else prop_name(c, &name);
                function(c, name, 0, 1, 1);
                if (computed) emit(c, kind == KW_GET ? OP_INITGETELEM : OP_INITSETELEM);
                else { emit(c, kind == KW_GET ? OP_INITGET : OP_INITSET); emit16(c, kstr(c, name)); }
                goto sep;
            }
            load(c, &s);
        }
        if (c->tok.t == TK_IDENT && c->tok.kw == KW_ASYNC) {
            State s; save(c, &s);
            next(c);
            if (!is_p(c, ',') && !is_p(c, ':') && !is_p(c, '(') && !is_p(c, '}') && !c->tok.nl) {
                is_async = 1;                    /* async m() { }, async *m() { } */
            } else load(c, &s);
        }
        if (accept(c, '*')) is_gen = 1;      /* *m() { } */
        if (is_p(c, '[')) {
            next(c); assign(c); want(c, ']', "expected ]");
            computed = 1;
        } else {
            int was_ident = c->tok.t == TK_IDENT;
            if (!prop_name(c, &name)) fail(c, "expected a property name");
            if (was_ident && (is_p(c, ',') || is_p(c, '}'))) {   /* { a } */
                Ex im;
                if (c->module && import_name(c, name, &im)) value(c, &im);
                else ref(c, R_GET, name);
                emit(c, OP_INITPROP); emit16(c, kstr(c, name));
                goto sep;
            }
            if (was_ident && is_p(c, '=')) fail(c, "a default here is only for destructuring");
        }
        if (is_p(c, '(')) { c->next_async = is_async; c->next_gen = is_gen; function(c, name, 0, 1, 1); }   /* a method */
        else if (is_gen) fail(c, "expected ( after a generator's name");
        else if (is_async) fail(c, "expected ( after an async method's name");
        else { want(c, ':', "expected : after a property name"); assign(c); }
        if (computed) emit(c, OP_INITELEM);
        else { emit(c, OP_INITPROP); emit16(c, kstr(c, name)); }
    sep:
        if (!is_p(c, '}')) want(c, ',', "expected , or } in an object");
    }
    next(c);
}

/* The end of a function made here rather than from its source (a class's
 * fields, a static block): its prologue, and it is made a kid of outer. */
static u16 end_fn(C *c, Fs *f, Fs *outer)
{
    Proto *p;
    u16 kid;
    c->fs = f;
    prologue(c, f);
    p = finish(c, f);
    c->fs = outer;
    if (outer->nkids >= 65535) fail(c, "too many functions in one function");
    if (outer->nkids == outer->ckids) outer->kids = grow(c, outer->kids, outer->nkids, &outer->ckids, sizeof(Proto *));
    kid = (u16)outer->nkids;
    outer->kids[outer->nkids++] = p;
    return kid;
}

/* A member's name, private (#x: kept as a hidden "#x") or not.  0 if none. */
static int member_name(C *c, Str **name, int *priv)
{
    *priv = 0;
    if (accept(c, '#')) {
        u16 b[130]; u32 i;
        if (c->tok.t != TK_IDENT || !c->tok.s || c->tok.s->len > 128) fail(c, "expected a name after #");
        b[0] = '#';
        for (i = 0; i < c->tok.s->len; i++) b[i + 1] = c->tok.s->c[i];
        *name = str_intern16(b, c->tok.s->len + 1);
        *priv = 1;
        next(c);
        return 1;
    }
    return prop_name(c, name);
}

/* Do the class's members (from just inside its {) include a field of each
 * instance (x = 1, or x;)?  Only looked at: the position is put back. */
static int has_instance_fields(C *c)
{
    State s0;
    int found = 0;
    save(c, &s0);
    while (!is_p(c, '}') && c->tok.t != TK_EOF) {
        Str *n; int priv, stat = 0;
        if (accept(c, ';')) continue;
        if (is_kw(c, KW_STATIC)) {
            State s; save(c, &s); next(c);
            if (is_p(c, '(') || is_p(c, '=') || is_p(c, ';') || is_p(c, '}')) load(c, &s); else stat = 1;
            if (stat && is_p(c, '{')) { skip_brackets(c); continue; }   /* static { } */
        }
        while ((is_kw(c, KW_GET) || is_kw(c, KW_SET) || is_kw(c, KW_ASYNC) || is_p(c, '*'))) {
            State s; save(c, &s); next(c);
            if (is_p(c, '(') || is_p(c, '=') || is_p(c, ';') || is_p(c, '}') || c->tok.nl) { load(c, &s); break; }
        }
        if (is_p(c, '[')) skip_brackets(c);
        else if (!member_name(c, &n, &priv)) break;
        if (is_p(c, '(')) { skip_brackets(c); skip_brackets(c); continue; }   /* a method */
        (void)stat;
        found = 1;                       /* a field, static or not: the method is made either way */
        break;
    }
    load(c, &s0);
    return found;
}

/* A class: a constructor function with its methods on its prototype.
 * Fields of each instance are set by a hidden method on the prototype,
 * which the constructor calls as it starts (or, in a class that extends
 * another, as soon as super() returns); static fields are set on the
 * class as it is made, static blocks run then too. */
static void class_body(C *c, Str *name, int bind)
{
    Str *keep_super = c->super_name, *sup = 0, *keep_fields = c->fields_name, *fields = 0;
    int keep_static = c->static_ctx;
    State body, ctor;
    int have_ctor = 0;
    Fs initf, *outer;
    int init_open = 0;
    if (is_kw(c, KW_EXTENDS)) {
        /* the parent is kept in a hidden variable beside the class */
        char b[24]; int k = 0, j; u32 n = ++c->classes;
        const char *pre = "__flopos_super";
        char t[12]; int tk = 0;
        next(c);
        for (j = 0; pre[j]; j++) b[k++] = pre[j];
        do { t[tk++] = (char)('0' + n % 10); n /= 10; } while (n);
        while (tk) b[k++] = t[--tk];
        b[k] = 0;
        sup = str_intern(str_ascii(b));
        declare_local(c, c->fs, sup);
        {
            Ex e;
            unary(c, &e);
            value(c, &e);
        }
        store_name(c, sup);
        emit(c, OP_POP);
    }
    if (!is_p(c, '{')) fail(c, "expected { to start a class");
    save(c, &body);
    next(c);
    if (has_instance_fields(c)) {
        char b[24]; int k = 0, j; u32 n = ++c->classes;
        const char *pre = "__flopos_fields";
        char t[12]; int tk = 0;
        for (j = 0; pre[j]; j++) b[k++] = pre[j];
        do { t[tk++] = (char)('0' + n % 10); n /= 10; } while (n);
        while (tk) b[k++] = t[--tk];
        b[k] = 0;
        fields = str_intern(str_ascii(b));
    }
    /* find the constructor */
    while (!is_p(c, '}') && c->tok.t != TK_EOF) {
        if (c->tok.t == TK_IDENT && c->tok.s && str_eq_ascii(c->tok.s, "constructor")) {
            State s; save(c, &s);
            next(c);
            if (is_p(c, '(')) { have_ctor = 1; save(c, &ctor); break; }
            load(c, &s);
        }
        skip_one(c);
    }
    c->super_name = sup;
    c->static_ctx = 0;
    c->fields_name = fields;
    if (have_ctor) {
        load(c, &ctor);
        function(c, name, 0, 1, 2);
    } else {
        /* function () { super.apply(this, arguments) } -- or nothing; with
         * fields, (...a) { super(...a) }, so they are set after it */
        static u16 src[80];
        static const char *a = "(){", *b = ".apply(this,arguments)}", *a2 = "(...a){super(...a)}";
        u32 n = 0, i;
        State s; save(c, &s);
        if (sup && fields) for (i = 0; a2[i]; i++) src[n++] = (u16)a2[i];
        else {
            for (i = 0; a[i]; i++) src[n++] = (u16)a[i];
            if (sup) {
                for (i = 0; i < sup->len && n < 40; i++) src[n++] = sup->c[i];
                for (i = 0; b[i]; i++) src[n++] = (u16)b[i];
            } else src[n++] = '}';
        }
        lex_start(&c->L, src, n);
        next(c);
        function(c, name, 0, 1, 2);
        load(c, &s);
    }
    c->fields_name = 0;
    /* [C] */
    if (sup) {
        emit(c, OP_DUP); emit(c, OP_GETPROP); emit16(c, kstr(c, S_prototype));
        ref(c, R_GET, sup); emit(c, OP_GETPROP); emit16(c, kstr(c, S_prototype));
        emit(c, OP_SETPROTO); emit(c, OP_POP);
        ref(c, R_GET, sup); emit(c, OP_SETPROTO);
    }
    if (bind) store_name(c, name);       /* its name, already, for static fields and blocks that use it */
    load(c, &body);
    next(c);
    outer = c->fs;
    while (!is_p(c, '}')) {
        Str *mname = 0;
        int kind = 0, stat = 0, computed = 0, is_async = 0, priv = 0, is_gen = 0;
        if (accept(c, ';')) continue;
        if (is_kw(c, KW_STATIC)) {
            State s; save(c, &s);
            next(c);
            if (is_p(c, '(') || is_p(c, '=') || is_p(c, ';') || is_p(c, '}')) load(c, &s); else stat = 1;
        }
        if (stat && is_p(c, '{')) {          /* static { ... }: run as the class is made, this the class */
            Fs bf;
            u16 kid;
            fs_open(c, &bf, 0, 0);
            c->static_ctx = 1;
            next(c);
            while (!is_p(c, '}')) {
                if (c->tok.t == TK_EOF) fail(c, "a static block is never closed");
                statement(c);
            }
            next(c);
            kid = end_fn(c, &bf, outer);
            emit(c, OP_DUP); emit(c, OP_CLOSURE); emit16(c, kid);
            emit(c, OP_CALLM); emit(c, 0); emit(c, OP_POP);
            continue;
        }
        if ((is_kw(c, KW_GET) || is_kw(c, KW_SET))) {
            State s; save(c, &s);
            kind = c->tok.kw;
            next(c);
            if (is_p(c, '(') || is_p(c, '=') || is_p(c, ';') || is_p(c, '}') || c->tok.nl) { load(c, &s); kind = 0; }
        }
        if (is_kw(c, KW_ASYNC)) {
            State s; save(c, &s);
            next(c);
            if (is_p(c, '(') || is_p(c, '=') || is_p(c, ';') || is_p(c, '}') || c->tok.nl) load(c, &s);   /* a member called async */
            else is_async = 1;
        }
        if (accept(c, '*')) is_gen = 1;
        if (is_p(c, '[')) { computed = 1; }
        else if (!member_name(c, &mname, &priv)) fail(c, "expected a method name");
        if (!computed && !stat && !kind && !priv && str_eq_ascii(mname, "constructor")) {
            skip_brackets(c);            /* its parameters */
            skip_brackets(c);            /* its body: made already */
            continue;
        }
        if (!kind && !is_async && (computed ? 0 : !is_p(c, '('))) {
            /* a field: x = value, or x; on each instance, or on the class (static) */
            if (stat) {
                emit(c, OP_DUP);
                c->static_ctx = 1;
                if (accept(c, '=')) assign(c); else emit(c, OP_UNDEF);
                emit(c, priv ? OP_INITHIDDEN : OP_INITPROP); emit16(c, kstr(c, mname));
                emit(c, OP_POP);
            } else {
                if (!fields) fail(c, "a field the class did not expect");
                if (!init_open) { c->fs = outer; fs_open(c, &initf, 0, 0); init_open = 1; }
                c->fs = &initf;
                c->static_ctx = 0;
                emit(c, OP_THIS);
                if (accept(c, '=')) assign(c); else emit(c, OP_UNDEF);
                emit(c, priv ? OP_INITHIDDEN : OP_INITPROP); emit16(c, kstr(c, mname));
                emit(c, OP_POP);
                c->fs = outer;
            }
            if (!accept(c, ';') && !is_p(c, '}') && !c->tok.nl) fail(c, "expected ; after a field");
            continue;
        }
        if (computed) {
            /* [key] = value: a computed field; or [key]() { }: a method */
            State s; save(c, &s);
            skip_brackets(c);
            if (!is_p(c, '(')) {
                load(c, &s);
                if (stat) {
                    emit(c, OP_DUP);
                    c->static_ctx = 1;
                    next(c); assign(c); want(c, ']', "expected ]");
                    if (accept(c, '=')) assign(c); else emit(c, OP_UNDEF);
                    emit(c, OP_INITELEM); emit(c, OP_POP);
                } else {
                    if (!fields) fail(c, "a field the class did not expect");
                    if (!init_open) { c->fs = outer; fs_open(c, &initf, 0, 0); init_open = 1; }
                    c->fs = &initf;
                    c->static_ctx = 0;
                    emit(c, OP_THIS);
                    next(c); assign(c); want(c, ']', "expected ]");
                    if (accept(c, '=')) assign(c); else emit(c, OP_UNDEF);
                    emit(c, OP_INITELEM); emit(c, OP_POP);
                    c->fs = outer;
                }
                if (!accept(c, ';') && !is_p(c, '}') && !c->tok.nl) fail(c, "expected ; after a field");
                continue;
            }
            load(c, &s);
        }
        emit(c, OP_DUP);                 /* [C C]                           */
        if (!stat) { emit(c, OP_GETPROP); emit16(c, kstr(c, S_prototype)); }
        c->static_ctx = stat;
        if (computed) { next(c); assign(c); want(c, ']', "expected ]"); }
        c->next_async = is_async;
        c->next_gen = is_gen;
        function(c, mname, 0, 1, 1);
        if (computed) emit(c, kind == KW_GET ? OP_INITGETELEM : kind == KW_SET ? OP_INITSETELEM : OP_INITELEM);
        else {
            emit(c, kind == KW_GET ? OP_INITGET : kind == KW_SET ? OP_INITSET : OP_INITHIDDEN);
            emit16(c, kstr(c, mname));
        }
        emit(c, OP_POP);
    }
    next(c);
    if (fields) {                        /* the fields' method, on the prototype, hidden */
        u16 kid;
        if (!init_open) { fs_open(c, &initf, 0, 0); init_open = 1; }
        kid = end_fn(c, &initf, outer);
        emit(c, OP_DUP); emit(c, OP_GETPROP); emit16(c, kstr(c, S_prototype));
        emit(c, OP_CLOSURE); emit16(c, kid);
        emit(c, OP_INITHIDDEN); emit16(c, kstr(c, fields));
        emit(c, OP_POP);
    }
    c->super_name = keep_super;
    c->static_ctx = keep_static;
    c->fields_name = keep_fields;
}

static void args_and_call(C *c, int method)
{
    int argc = 0, spread = 0;
    State s;
    next(c);
    save(c, &s);
    /* any ... among them? then they go into a list */
    while (!is_p(c, ')') && c->tok.t != TK_EOF) {
        if (is_p(c, P3('.', '.', '.'))) { spread = 1; break; }
        skip_one(c);
    }
    load(c, &s);
    if (spread) emit(c, OP_ARRAY);
    while (!is_p(c, ')')) {
        if (accept(c, P3('.', '.', '.'))) { assign(c); emit(c, OP_SPREAD); }
        else { assign(c); if (spread) emit(c, OP_APPEND); }
        argc++;
        if (!is_p(c, ')')) want(c, ',', "expected , or ) after an argument");
    }
    next(c);
    if (argc > 255 && !spread) fail(c, "too many arguments");
    if (spread) { emit(c, OP_CALLSPREAD); emit(c, (u32)method); }
    else { emit(c, method ? OP_CALLM : OP_CALL); emit(c, (u32)argc); }
}

static int arrow_ahead(C *c)
{
    State s;
    int r;
    save(c, &s);
    skip_brackets(c);
    r = is_p(c, P2('=', '>')) && !c->tok.nl;
    load(c, &s);
    return r;
}

static void primary(C *c, Ex *e)
{
    Tok *t = &c->tok;
    e->k = X_VAL;
    switch (t->t) {
    case TK_NUM: push_num(c, t->num); next(c); return;
    case TK_STR: emit(c, OP_CONST); emit16(c, kstr(c, t->s)); next(c); return;
    case TK_TEMPLATE: template(c); return;
    case TK_REGEX:
        emit(c, OP_REGEXP); emit16(c, kstr(c, t->s)); emit16(c, kstr(c, t->flags));
        next(c);
        return;
    case TK_IDENT:
        switch (t->kw) {
        case KW_THIS: {                      /* at a module's top level (or in its arrows): undefined */
            Fs *f = c->fs;
            while (c->module && f && f != c->mod_top && f->arrow) f = f->up;
            emit(c, c->module && f == c->mod_top ? OP_UNDEF : OP_THIS);
            next(c);
            return;
        }
        case KW_NULL: emit(c, OP_NULL); next(c); return;
        case KW_TRUE: emit(c, OP_TRUE); next(c); return;
        case KW_FALSE: emit(c, OP_FALSE); next(c); return;
        case KW_FUNCTION: {
            Str *name = 0;
            next(c);
            if (accept(c, '*')) c->next_gen = 1;
            if (c->tok.t == TK_IDENT && !is_p(c, '(')) { name = c->tok.s; next(c); }
            function(c, name, 0, 1, 0);
            return;
        }
        case KW_CLASS: {
            Str *name = 0;
            next(c);
            if (c->tok.t == TK_IDENT && c->tok.kw != KW_EXTENDS) { name = c->tok.s; next(c); }
            class_body(c, name, 0);
            return;
        }
        case KW_ASYNC: {
            State s; save(c, &s);
            next(c);
            if (!c->tok.nl && is_kw(c, KW_FUNCTION)) {          /* async function () { } */
                Str *name = 0;
                next(c);
                if (accept(c, '*')) c->next_gen = 1;          /* async function* () { } */
                if (c->tok.t == TK_IDENT && !is_p(c, '(')) { name = c->tok.s; next(c); }
                c->next_async = 1;
                function(c, name, 0, 1, 0);
                return;
            }
            if (!c->tok.nl && is_p(c, '(') && arrow_ahead(c)) {   /* async (a, b) => ... */
                c->next_async = 1;
                function(c, 0, 1, 1, 0);
                return;
            }
            if (!c->tok.nl && c->tok.t == TK_IDENT && !KW_RESERVED(c->tok.kw)) {   /* async x => ... */
                State s2; Str *n = c->tok.s;
                save(c, &s2);
                next(c);
                if (is_p(c, P2('=', '>')) && !c->tok.nl) {
                    c->next_async = 1;
                    function(c, n, 1, 1, 3);
                    return;
                }
                load(c, &s2);
            }
            load(c, &s);
            break;
        }
        case KW_SUPER:
            next(c);
            if (!c->super_name) fail(c, "super outside a class that extends another");
            if (is_p(c, '(')) {                  /* super(...)              */
                emit(c, OP_THIS);
                ref(c, R_GET, c->super_name);
                args_and_call(c, 1);
                emit(c, OP_POP); emit(c, OP_THIS);
                if (c->fields_name) {            /* this class's fields, once the parent has made this */
                    emit(c, OP_DUP); emit(c, OP_DUP);
                    emit(c, OP_GETPROP); emit16(c, kstr(c, c->fields_name));
                    emit(c, OP_CALLM); emit(c, 0);
                    emit(c, OP_POP);
                }
                return;
            }
            /* super.name: the parent's (prototype's) property, as a method of this */
            ref(c, R_GET, c->super_name);
            if (!c->static_ctx) { emit(c, OP_GETPROP); emit16(c, kstr(c, S_prototype)); }
            if (accept(c, '.')) {
                Str *n;
                if (!prop_name(c, &n)) fail(c, "expected a name after super.");
                emit(c, OP_GETPROP); emit16(c, kstr(c, n));
            } else if (accept(c, '[')) { expr(c); want(c, ']', "expected ]"); emit(c, OP_GETELEM); }
            else fail(c, "expected super. or super(");
            if (is_p(c, '(')) {                  /* call it with this       */
                emit(c, OP_THIS); emit(c, OP_SWAP);
                args_and_call(c, 1);
            }
            return;
        case KW_NEW: {
            Ex f;
            int argc = 0;
            next(c);
            if (accept(c, '.')) {                /* new.target: not kept    */
                next(c);
                emit(c, OP_UNDEF);
                return;
            }
            primary(c, &f);
            for (;;) {                           /* member accesses, no calls */
                if (accept(c, '.')) {
                    Str *n;
                    value(c, &f);
                    if (c->tok.t != TK_IDENT) fail(c, "expected a name after .");
                    n = c->tok.s; next(c);
                    f.k = X_PROP; f.idx = kstr(c, n);
                } else if (is_p(c, '[')) {
                    value(c, &f); next(c); expr(c); want(c, ']', "expected ]"); f.k = X_ELEM;
                } else break;
            }
            value(c, &f);
            if (is_p(c, '(')) {
                State s;
                int spread = 0;
                next(c);
                save(c, &s);
                while (!is_p(c, ')') && c->tok.t != TK_EOF) { if (is_p(c, P3('.', '.', '.'))) { spread = 1; break; } skip_one(c); }
                load(c, &s);
                if (spread) {                    /* new F(...xs): the arguments as a list, then made */
                    ref(c, R_GET, atom("__flopos_construct"));
                    emit(c, OP_SWAP);
                    emit(c, OP_ARRAY);
                    while (!is_p(c, ')')) {
                        if (accept(c, P3('.', '.', '.'))) { assign(c); emit(c, OP_SPREAD); }
                        else { assign(c); emit(c, OP_APPEND); }
                        if (!is_p(c, ')')) want(c, ',', "expected , or )");
                    }
                    next(c);
                    emit(c, OP_CALL); emit(c, 2);
                    return;
                }
                while (!is_p(c, ')')) {
                    assign(c); argc++;
                    if (!is_p(c, ')')) want(c, ',', "expected , or )");
                }
                next(c);
            }
            emit(c, OP_NEW); emit(c, (u32)argc);
            return;
        }
        case KW_YIELD: case KW_AWAIT:
            if (t->kw == KW_AWAIT) { State s; save(c, &s); next(c); if (c->tok.t != TK_PUNCT || is_p(c, '(') || is_p(c, '[')) fail(c, "await is not supported yet"); load(c, &s); }
            break;
        case KW_IMPORT: import_expr(c, e); return;
        default:
            if (KW_RESERVED(t->kw)) fail(c, "a word that is not allowed here");
        }
        {
            Str *n = t->s;
            next(c);
            if (is_p(c, P2('=', '>')) && !c->tok.nl) {   /* x => ...    */
                function(c, n, 1, 1, 3);
                return;
            }
            if (c->module && import_name(c, n, e)) return;
            e->k = X_NAME; e->name = n;
        }
        return;
    case TK_PUNCT:
        if (is_p(c, '(')) {
            if (arrow_ahead(c)) { function(c, 0, 1, 1, 0); return; }
            next(c);
            {
                int keep = c->no_in;
                c->no_in = 0;
                expr(c);
                c->no_in = keep;
            }
            want(c, ')', "expected )");
            return;
        }
        if (is_p(c, '[')) { array_literal(c); return; }
        if (is_p(c, '{')) { object_literal(c); return; }
        if (is_p(c, '#')) {                  /* #x in obj */
            Str *n; int priv;
            member_name(c, &n, &priv);
            emit(c, OP_CONST); emit16(c, kstr(c, n));
            return;
        }
    }
    fail(c, t->t == TK_EOF ? "the script ends in the middle of something" : "something unexpected");
}

static void postfix_chain(C *c, Ex *e)
{
    u32 opt[64];
    int nopt = 0;
    for (;;) {
        if (is_p(c, '.')) {
            Str *n;
            next(c);
            value(c, e);
            if (is_p(c, '#')) { int priv; member_name(c, &n, &priv); }   /* this.#x: kept as a hidden "#x" */
            else {
                if (c->tok.t != TK_IDENT) fail(c, "expected a name after .");
                n = c->tok.s; next(c);
            }
            e->k = X_PROP; e->idx = kstr(c, n);
        } else if (is_p(c, P2('?', '.'))) {
            next(c);
            value(c, e);
            if (nopt >= 64) fail(c, "too many ?. in one chain");
            opt[nopt++] = jump(c, OP_OPTCHAIN);
            if (is_p(c, '(')) { args_and_call(c, 0); e->k = X_VAL; }
            else if (is_p(c, '[')) { next(c); expr(c); want(c, ']', "expected ]"); e->k = X_ELEM; }
            else { Str *n; if (c->tok.t != TK_IDENT) fail(c, "expected a name after ?."); n = c->tok.s; next(c); e->k = X_PROP; e->idx = kstr(c, n); }
        } else if (is_p(c, '[')) {
            next(c);
            value(c, e);
            {
                int keep = c->no_in;
                c->no_in = 0;
                expr(c);
                c->no_in = keep;
            }
            want(c, ']', "expected ]");
            e->k = X_ELEM;
        } else if (is_p(c, '(')) {
            if (e->k == X_PROP) {
                emit(c, OP_DUP); emit(c, OP_GETPROP); emit16(c, e->idx);
                args_and_call(c, 1);
            } else if (e->k == X_ELEM) {
                emit(c, OP_DUP2); emit(c, OP_GETELEM); emit(c, OP_SWAP); emit(c, OP_POP);
                args_and_call(c, 1);
            } else { value(c, e); args_and_call(c, 0); }
            e->k = X_VAL;
        } else if (c->tok.t == TK_TEMPLATE) {
            c->tok.src_at = c->tok.pos + 1;          /* after the ` */
            if (e->k == X_PROP) {
                emit(c, OP_DUP); emit(c, OP_GETPROP); emit16(c, e->idx);
                tagged_template(c, 1);
            } else if (e->k == X_ELEM) {
                emit(c, OP_DUP2); emit(c, OP_GETELEM); emit(c, OP_SWAP); emit(c, OP_POP);
                tagged_template(c, 1);
            } else { value(c, e); tagged_template(c, 0); }
            e->k = X_VAL;
        } else break;
    }
    if (nopt) {
        int i;
        value(c, e);
        for (i = 0; i < nopt; i++) patch(c, opt[i]);
    }
}

static void unary(C *c, Ex *e)
{
    if (c->tok.t == TK_PUNCT) {
        int p = c->tok.p;
        if (p == '!' || p == '-' || p == '+' || p == '~') {
            next(c); unary(c, e); value(c, e);
            emit(c, p == '!' ? OP_NOT : p == '-' ? OP_NEG : p == '+' ? OP_PLUS : OP_BITNOT);
            return;
        }
        if (p == P2('+', '+') || p == P2('-', '-')) {
            next(c); unary(c, e);
            if (e->k == X_VAL) fail(c, "that cannot be counted up or down");
            load_for_update(c, e);
            emit(c, p == P2('+', '+') ? OP_INC : OP_DEC);
            store(c, e);
            e->k = X_VAL;
            return;
        }
    } else if (c->tok.t == TK_IDENT) {
        int k = c->tok.kw;
        if (k == KW_TYPEOF) {
            next(c); unary(c, e);
            if (e->k == X_NAME) ref(c, R_TYPEOF, e->name);
            else value(c, e);
            emit(c, OP_TYPEOF);
            e->k = X_VAL;
            return;
        }
        if (k == KW_VOID) { next(c); unary(c, e); value(c, e); emit(c, OP_VOID); return; }
        if (k == KW_DELETE) {
            next(c); unary(c, e);
            if (e->k == X_PROP) { emit(c, OP_DELPROP); emit16(c, e->idx); }
            else if (e->k == X_ELEM) emit(c, OP_DELELEM);
            else { value(c, e); emit(c, OP_POP); emit(c, OP_TRUE); }
            e->k = X_VAL;
            return;
        }
        if (k == KW_AWAIT && c->fs->async_fn) {      /* await x: wait for it, then go on */
            next(c); unary(c, e);
            value(c, e);
            emit_await(c);
            e->k = X_VAL;
            return;
        }
        if (k == KW_AWAIT) {
            State s; save(c, &s); next(c);
            if (c->tok.t != TK_PUNCT || is_p(c, '(') || is_p(c, '[')) fail(c, "await outside an async function");
            load(c, &s);
        }
    }
    primary(c, e);
    postfix_chain(c, e);
    if (c->tok.t == TK_PUNCT && (c->tok.p == P2('+', '+') || c->tok.p == P2('-', '-')) && !c->tok.nl) {
        int inc = c->tok.p == P2('+', '+');
        if (e->k == X_VAL) fail(c, "that cannot be counted up or down");
        next(c);
        load_for_update(c, e);
        emit(c, OP_PLUS);                     /* the old value, as a number */
        if (e->k == X_NAME) {
            emit(c, OP_DUP); emit(c, inc ? OP_INC : OP_DEC); store(c, e); emit(c, OP_POP);
        } else if (e->k == X_PROP) {
            emit(c, OP_DUP); emit(c, OP_ROT3); emit(c, inc ? OP_INC : OP_DEC); store(c, e); emit(c, OP_POP);
        } else {
            emit(c, OP_DUP); emit(c, OP_ROT4); emit(c, inc ? OP_INC : OP_DEC); store(c, e); emit(c, OP_POP);
        }
        e->k = X_VAL;
    }
}

static int binop(C *c, int *op, int *right)
{
    Tok *t = &c->tok;
    *right = 0;
    if (t->t == TK_IDENT) {
        if (t->kw == KW_INSTANCEOF) { *op = OP_INSTANCEOF; return 8; }
        if (t->kw == KW_IN && !c->no_in) { *op = OP_IN; return 8; }
        return 0;
    }
    if (t->t != TK_PUNCT) return 0;
    switch (t->p) {
    case P2('?', '?'): *op = -3; return 1;
    case P2('|', '|'): *op = -1; return 2;
    case P2('&', '&'): *op = -2; return 3;
    case '|': *op = OP_OR; return 4;
    case '^': *op = OP_XOR; return 5;
    case '&': *op = OP_AND; return 6;
    case P2('=', '='): *op = OP_EQ; return 7;
    case P2('!', '='): *op = OP_NE; return 7;
    case P3('=', '=', '='): *op = OP_SEQ; return 7;
    case P3('!', '=', '='): *op = OP_SNE; return 7;
    case '<': *op = OP_LT; return 8;
    case '>': *op = OP_GT; return 8;
    case P2('<', '='): *op = OP_LE; return 8;
    case P2('>', '='): *op = OP_GE; return 8;
    case P2('<', '<'): *op = OP_SHL; return 9;
    case P2('>', '>'): *op = OP_SHR; return 9;
    case P3('>', '>', '>'): *op = OP_USHR; return 9;
    case '+': *op = OP_ADD; return 10;
    case '-': *op = OP_SUB; return 10;
    case '*': *op = OP_MUL; return 11;
    case '/': *op = OP_DIV; return 11;
    case '%': *op = OP_MOD; return 11;
    case P2('*', '*'): *op = OP_EXP; *right = 1; return 12;
    }
    return 0;
}

static void binary(C *c, Ex *e, int min)
{
    int op, right, prec;
    unary(c, e);
    while ((prec = binop(c, &op, &right)) && prec >= min) {
        value(c, e);
        next(c);
        if (op < 0) {                         /* || && ??: short circuits  */
            u32 j = jump(c, op == -1 ? OP_JTKEEP : op == -2 ? OP_JFKEEP : OP_JNNKEEP);
            Ex r;
            emit(c, OP_POP);
            binary(c, &r, prec + 1);
            value(c, &r);
            patch(c, j);
        } else {
            Ex r;
            binary(c, &r, right ? prec : prec + 1);
            value(c, &r);
            emit(c, (u32)op);
        }
        e->k = X_VAL;
    }
}

static void conditional(C *c, Ex *e)
{
    binary(c, e, 1);
    if (is_p(c, '?')) {
        u32 jf, je;
        int keep = c->no_in;
        value(c, e);
        next(c);
        jf = jump(c, OP_JF);
        c->no_in = 0;
        assign(c);
        c->no_in = keep;
        je = jump(c, OP_JMP);
        patch(c, jf);
        want(c, ':', "expected : in a ? : choice");
        assign(c);
        patch(c, je);
        e->k = X_VAL;
    }
}

static int assign_op(int p)
{
    switch (p) {
    case P2('+', '='): return OP_ADD;   case P2('-', '='): return OP_SUB;
    case P2('*', '='): return OP_MUL;   case P2('/', '='): return OP_DIV;
    case P2('%', '='): return OP_MOD;   case P3('*', '*', '='): return OP_EXP;
    case P3('<', '<', '='): return OP_SHL; case P3('>', '>', '='): return OP_SHR;
    case P4('>', '>', '>', '='): return OP_USHR;
    case P2('&', '='): return OP_AND;   case P2('|', '='): return OP_OR;
    case P2('^', '='): return OP_XOR;
    case P3('&', '&', '='): return -2;  case P3('|', '|', '='): return -1;
    case P3('?', '?', '='): return -3;
    }
    return 0;
}

static int pattern_ahead(C *c)
{
    State s;
    int r;
    save(c, &s);
    skip_brackets(c);
    r = is_p(c, '=');
    load(c, &s);
    return r;
}

static void assign(C *c)
{
    Ex e;
    if (is_kw(c, KW_YIELD) && c->fs->gen_fn) {
        int star;
        next(c);
        star = !c->tok.nl && accept(c, '*');
        if (star && c->fs->async_fn) {       /* in an async generator: each of x's, awaited, yielded */
            u32 top, exit;
            assign(c);
            emit(c, OP_FORAWAIT);
            top = here(c);
            emit(c, OP_ANEXT);
            emit(c, OP_AWAITY);
            exit = jump(c, OP_ITERDONE);
            emit(c, OP_YIELD);
            emit(c, OP_POP);
            jump_to(c, OP_JMP, top);
            patch(c, exit);
            emit(c, OP_POP);
            emit(c, OP_UNDEF);
            return;
        }
        if (star) {                          /* yield* x: each of x's, yielded in turn */
            u32 top, exit;
            assign(c);
            emit(c, OP_FOROF);
            top = here(c);
            exit = jump(c, OP_ITERNEXT);
            emit(c, OP_YIELD);
            emit(c, OP_POP);
            jump_to(c, OP_JMP, top);
            patch(c, exit);
            emit(c, OP_POP);
            emit(c, OP_UNDEF);
            return;
        }
        if (c->tok.nl || c->tok.t == TK_EOF || is_p(c, ')') || is_p(c, ']') || is_p(c, '}') || is_p(c, ',') ||
            is_p(c, ';') || is_p(c, ':')) emit(c, OP_UNDEF);
        else assign(c);
        if (c->fs->async_fn) emit(c, OP_AWAITY);   /* an async generator yields what it awaited */
        emit(c, OP_YIELD);                   /* its value: what next() is given */
        return;
    }
    if ((is_p(c, '[') || is_p(c, '{')) && pattern_ahead(c)) {
        /* [a, b] = value: the value first, then the pattern */
        State pat, after;
        save(c, &pat);
        skip_brackets(c);
        next(c);                              /* the = */
        assign(c);
        emit(c, OP_DUP);
        save(c, &after);
        load(c, &pat);
        destructure(c, 0, 0);
        load(c, &after);
        return;
    }
    conditional(c, &e);
    if (c->tok.t == TK_PUNCT && c->tok.p == '=') {
        if (e.k == X_VAL) fail(c, "that cannot be assigned to");
        next(c);
        assign(c);
        store(c, &e);
        return;
    }
    if (c->tok.t == TK_PUNCT && assign_op(c->tok.p)) {
        int op = assign_op(c->tok.p);
        if (e.k == X_VAL) fail(c, "that cannot be assigned to");
        next(c);
        load_for_update(c, &e);
        if (op < 0) {                         /* a &&= b, a ||= b, a ??= b */
            u32 j = jump(c, op == -1 ? OP_JTKEEP : op == -2 ? OP_JFKEEP : OP_JNNKEEP), j2;
            emit(c, OP_POP);
            assign(c);
            store(c, &e);
            j2 = jump(c, OP_JMP);
            patch(c, j);
            if (e.k == X_PROP) { emit(c, OP_SWAP); emit(c, OP_POP); }
            else if (e.k == X_ELEM) { emit(c, OP_ROT3); emit(c, OP_POP); emit(c, OP_POP); }
            patch(c, j2);
            return;
        }
        assign(c);
        emit(c, (u32)op);
        store(c, &e);
        return;
    }
    value(c, &e);
}

static void expr(C *c)
{
    assign(c);
    while (is_p(c, ',')) { next(c); emit(c, OP_POP); assign(c); }
}

/* ---- destructuring --------------------------------------------------------------- */
/* Where a destructured value goes: a name (declared if decl), a pattern
 * again, or (assigning) anything that can be assigned to.  The value is
 * on the stack, and is used up. */
static void target(C *c, int decl, int kw)
{
    if (is_p(c, '[') || is_p(c, '{')) { destructure(c, decl, kw); return; }
    if (decl) {
        Str *n;
        if (c->tok.t != TK_IDENT || KW_RESERVED(c->tok.kw)) fail(c, "expected a name");
        n = c->tok.s; next(c);
        declare(c, n);
        store_name(c, n);
        emit(c, OP_POP);
        return;
    }
    {
        Ex e;
        unary(c, &e);
        if (e.k == X_NAME) { store(c, &e); emit(c, OP_POP); }
        else if (e.k == X_PROP) { emit(c, OP_SWAP); store(c, &e); emit(c, OP_POP); }
        else if (e.k == X_ELEM) { emit(c, OP_ROT3); emit(c, OP_ROT3); store(c, &e); emit(c, OP_POP); }
        else fail(c, "that cannot be assigned to");
    }
}

static void with_default(C *c)
{
    if (accept(c, '=')) {
        u32 j;
        emit(c, OP_DUP); emit(c, OP_UNDEF); emit(c, OP_SEQ);
        j = jump(c, OP_JF);
        emit(c, OP_POP);
        assign(c);
        patch(c, j);
    }
}

/* In a pattern: is what comes a property (a.b, a[0]) rather than a name? */
static int prop_target_ahead(C *c)
{
    State s;
    int r;
    save(c, &s);
    next(c);
    r = is_p(c, '.') || is_p(c, '[');
    load(c, &s);
    return r;
}

/* The value on top, given its default, stored into the property the
 * pattern names at ts: read past the target, take any default, then go
 * back and store. */
static void into_target(C *c, State *ts)
{
    State after;
    while (!is_p(c, ',') && !is_p(c, ']') && !is_p(c, '}') && !is_p(c, '=') && c->tok.t != TK_EOF) skip_one(c);
    with_default(c);
    save(c, &after);
    load(c, ts);
    target(c, 0, 0);
    load(c, &after);
}

static void destructure(C *c, int decl, int kw)
{
    if (accept(c, '[')) {
        int i = 0;
        emit(c, OP_LISTOF);
        while (!is_p(c, ']')) {
            if (is_p(c, ',')) { next(c); i++; continue; }
            if (accept(c, P3('.', '.', '.'))) {
                emit(c, OP_DUP);
                emit(c, OP_DUP); emit(c, OP_GETPROP); emit16(c, kstr(c, atom("slice")));
                push_num(c, i);
                emit(c, OP_CALLM); emit(c, 1);
                target(c, decl, kw);
                break;
            }
            emit(c, OP_DUP);
            push_num(c, i);
            emit(c, OP_GETELEM);
            if (!is_p(c, '[') && !is_p(c, '{')) {
                Str *n;
                State ts;
                save(c, &ts);
                if (!decl && (c->tok.t != TK_IDENT || KW_RESERVED(c->tok.kw) || prop_target_ahead(c))) {
                    into_target(c, &ts);             /* [a.b, c[0]] = ... */
                    goto next_item;
                }
                if (c->tok.t != TK_IDENT || KW_RESERVED(c->tok.kw)) fail(c, "expected a name");
                n = c->tok.s; next(c);
                with_default(c);
                if (decl) declare(c, n);
                store_name(c, n);
                emit(c, OP_POP);
            } else {
                State s; save(c, &s);
                skip_brackets(c);
                with_default(c);
                {
                    State after; save(c, &after);
                    load(c, &s);
                    target(c, decl, kw);
                    load(c, &after);
                }
            }
        next_item:
            i++;
            if (!is_p(c, ']')) want(c, ',', "expected , or ]");
        }
        next(c);
        emit(c, OP_POP);
        return;
    }
    if (accept(c, '{')) {
        Str *named[64];                  /* the keys taken, for a ...rest after them */
        int nnamed = 0, any_computed = 0;
        while (!is_p(c, '}')) {
            Str *key = 0;
            int computed = 0;
            if (accept(c, P3('.', '.', '.'))) {          /* ...rest: the keys not named */
                int k;
                if (any_computed || nnamed > 64) fail(c, "...rest after a computed key is not supported yet");
                emit(c, OP_DUP);
                ref(c, R_GET, atom("__flopos_rest"));
                emit(c, OP_SWAP);
                emit(c, OP_ARRAY);
                for (k = 0; k < nnamed; k++) { emit(c, OP_CONST); emit16(c, kstr(c, named[k])); emit(c, OP_APPEND); }
                emit(c, OP_CALL); emit(c, 2);
                target(c, decl, kw);
                if (!is_p(c, '}')) fail(c, "...rest must come last in a pattern");
                break;
            }
            emit(c, OP_DUP);
            if (accept(c, '[')) { assign(c); want(c, ']', "expected ]"); computed = 1; any_computed = 1; emit(c, OP_GETELEM); }
            else {
                int was_ident = c->tok.t == TK_IDENT;
                if (!prop_name(c, &key)) fail(c, "expected a property name");
                if (nnamed < 64) named[nnamed] = key;
                nnamed++;
                emit(c, OP_GETPROP); emit16(c, kstr(c, key));
                if (was_ident && !is_p(c, ':')) {          /* { a } or { a = 1 } */
                    with_default(c);
                    if (decl) declare(c, key);
                    store_name(c, key);
                    emit(c, OP_POP);
                    goto sep;
                }
            }
            (void)computed;
            want(c, ':', "expected : in a pattern");
            if (is_p(c, '[') || is_p(c, '{')) {
                State s, after; save(c, &s);
                skip_brackets(c);
                with_default(c);
                save(c, &after);
                load(c, &s);
                target(c, decl, kw);
                load(c, &after);
            } else if (decl) {
                Str *n;
                if (c->tok.t != TK_IDENT) fail(c, "expected a name");
                n = c->tok.s; next(c);
                with_default(c);
                declare(c, n);
                store_name(c, n);
                emit(c, OP_POP);
            } else {
                Str *n;
                State ts;
                save(c, &ts);
                if (c->tok.t != TK_IDENT || KW_RESERVED(c->tok.kw) || prop_target_ahead(c)) { into_target(c, &ts); goto sep; }
                n = c->tok.s; next(c);
                with_default(c);
                store_name(c, n);
                emit(c, OP_POP);
            }
        sep:
            if (!is_p(c, '}')) want(c, ',', "expected , or }");
        }
        next(c);
        emit(c, OP_POP);
        return;
    }
    fail(c, "expected a pattern");
}

/* ---- statements ------------------------------------------------------------------ */
static void semi(C *c)
{
    if (accept(c, ';')) return;
    if (is_p(c, '}') || c->tok.t == TK_EOF || c->tok.nl) return;
    fail(c, "expected ; or the end of a line");
}

static void block(C *c)
{
    want(c, '{', "expected {");
    while (!is_p(c, '}')) {
        if (c->tok.t == TK_EOF) fail(c, "a { is never closed");
        statement(c);
    }
    next(c);
}

static void declaration(C *c, int kw)
{
    do {
        if (is_p(c, '[') || is_p(c, '{')) {
            State pat, after;
            save(c, &pat);
            skip_brackets(c);
            if (!accept(c, '=')) {
                if (is_kw(c, KW_IN) || is_kw(c, KW_OF)) { load(c, &pat); return; }
                fail(c, "a pattern needs a value");
            }
            assign(c);
            save(c, &after);
            load(c, &pat);
            destructure(c, 1, kw);
            load(c, &after);
        } else {
            Str *n;
            if (c->tok.t != TK_IDENT || KW_RESERVED(c->tok.kw)) fail(c, "expected a name to declare");
            n = c->tok.s;
            next(c);
            declare(c, n);
            if (accept(c, '=')) { assign(c); store_name(c, n); emit(c, OP_POP); }
            else if (kw != KW_VAR && !is_kw(c, KW_IN) && !is_kw(c, KW_OF)) { emit(c, OP_UNDEF); store_name(c, n); emit(c, OP_POP); }
        }
    } while (accept(c, ','));
}

static Loop *loop_begin(C *c, Loop *l, Str *label, int is_loop)
{
    memset(l, 0, sizeof *l);
    l->up = c->fs->loop;
    l->label = label;
    l->loop = is_loop;
    l->regions = c->fs->nreg;
    c->fs->loop = l;
    return l;
}

static void loop_end(C *c, Loop *l, u32 cont)
{
    u32 i;
    for (i = 0; i < l->ncnt; i++) patch_to(c, l->cnt[i], cont);
    for (i = 0; i < l->nbrk; i++) patch(c, l->brk[i]);
    c->fs->loop = l->up;
}

/* The finally blocks of the try regions above `down`, run on the way out. */
static void leave_regions(C *c, int down)
{
    int i;
    for (i = c->fs->nreg - 1; i >= down; i--) {
        Region r = c->fs->reg[i];
        emit(c, OP_ENDTRY);
        if (r.fin) {
            State s;
            int keep = c->fs->nreg;
            c->fs->nreg = i;                /* a break inside it is not ours */
            save(c, &s);
            c->L.pos = r.fin_pos; c->L.line = r.fin_line; c->L.regex_ok = 1;
            next(c);
            block(c);
            load(c, &s);
            c->fs->nreg = keep;
        }
    }
}

static void add_jump(C *c, u32 **list, u32 *n, u32 *cap, u32 at)
{
    if (*n == *cap) *list = grow(c, *list, *n, cap, sizeof(u32));
    (*list)[(*n)++] = at;
}

static void break_or_continue(C *c, int is_break)
{
    Loop *l = c->fs->loop;
    Str *label = 0;
    next(c);
    if (c->tok.t == TK_IDENT && !c->tok.nl && !KW_RESERVED(c->tok.kw)) { label = c->tok.s; next(c); }
    for (; l; l = l->up) {
        if (label) { if (l->label == label) break; }
        else if (is_break) { if (l->loop || l->sw) break; }
        else if (l->loop) break;
    }
    if (!l) fail(c, is_break ? "break outside a loop or switch" : "continue outside a loop");
    if (!is_break && !l->loop) fail(c, "continue to a label that is not a loop");
    leave_regions(c, l->regions);
    if (is_break) add_jump(c, &l->brk, &l->nbrk, &l->cbrk, jump(c, OP_JMP));
    else add_jump(c, &l->cnt, &l->ncnt, &l->ccnt, jump(c, OP_JMP));
    semi(c);
}

static void for_statement(C *c, Str *label)
{
    Loop lp;
    State s;
    int kw = -1, of = 0, in = 0, wait = 0;
    next(c);
    if (is_kw(c, KW_AWAIT)) {                   /* for await (x of list): each awaited in turn */
        if (!c->fs->async_fn) fail(c, "for await outside an async function");
        next(c); wait = 1;
    }
    want(c, '(', "expected ( after for");
    save(c, &s);
    if (is_kw(c, KW_VAR) || is_kw(c, KW_LET) || is_kw(c, KW_CONST)) { kw = c->tok.kw; next(c); }
    if (is_p(c, '[') || is_p(c, '{')) skip_brackets(c);
    else if (c->tok.t == TK_IDENT) {
        next(c);
        while (is_p(c, '.')) { next(c); next(c); }  /* for (a.b in o)        */
    }
    of = is_kw(c, KW_OF); in = is_kw(c, KW_IN);
    load(c, &s);
    if (wait && !of) fail(c, "for await needs of");
    if (of || in) {
        State tgt, after;
        u32 top, exit;
        if (kw >= 0) next(c);
        save(c, &tgt);
        if (is_p(c, '[') || is_p(c, '{')) skip_brackets(c);
        else { next(c); while (is_p(c, '.')) { next(c); next(c); } }
        next(c);                                /* in / of                  */
        expr(c);
        want(c, ')', "expected ) after for (... in ...)");
        emit(c, in ? OP_FORIN : wait ? OP_FORAWAIT : OP_FOROF);
        loop_begin(c, &lp, label, 1);
        top = here(c);
        if (wait) {                             /* next(), awaited; done?; the value, awaited */
            emit(c, OP_ANEXT);
            emit_await(c);
            exit = jump(c, OP_ITERDONE);
            emit_await(c);
        } else exit = jump(c, OP_ITERNEXT);
        save(c, &after);
        load(c, &tgt);
        target(c, kw >= 0, kw);
        load(c, &after);
        statement(c);
        jump_to(c, OP_JMP, top);
        patch(c, exit);
        loop_end(c, &lp, top);
        emit(c, OP_POP);
        return;
    }
    {
        u32 top, cond_exit = 0, body_jump, update;
        int has_cond = 0;
        int keep = c->no_in;
        c->no_in = 1;
        if (kw >= 0) { next(c); declaration(c, kw); }
        else if (!is_p(c, ';')) { expr(c); emit(c, OP_POP); }
        c->no_in = keep;
        want(c, ';', "expected ; in for (;;)");
        top = here(c);
        if (!is_p(c, ';')) { expr(c); cond_exit = jump(c, OP_JF); has_cond = 1; }
        want(c, ';', "expected ; in for (;;)");
        body_jump = jump(c, OP_JMP);
        update = here(c);
        if (!is_p(c, ')')) { expr(c); emit(c, OP_POP); }
        jump_to(c, OP_JMP, top);
        want(c, ')', "expected ) after for (;;)");
        patch(c, body_jump);
        loop_begin(c, &lp, label, 1);
        statement(c);
        jump_to(c, OP_JMP, update);
        if (has_cond) patch(c, cond_exit);
        loop_end(c, &lp, update);
    }
}

static void switch_statement(C *c, Str *label)
{
    Loop lp;
    u32 next_test = 0, fall = 0;
    int have_test = 0, have_fall = 0, have_default = 0;
    u32 default_at = 0, end_jump;
    next(c);
    want(c, '(', "expected ( after switch");
    expr(c);
    want(c, ')', "expected ) after switch (...)");
    want(c, '{', "expected { after switch (...)");
    loop_begin(c, &lp, label, 0);
    lp.sw = 1;
    while (!is_p(c, '}')) {
        if (is_kw(c, KW_CASE)) {
            next(c);
            if (have_fall) fall = jump(c, OP_JMP);
            if (have_test) patch(c, next_test);
            emit(c, OP_DUP);
            expr(c);
            emit(c, OP_SEQ);
            next_test = jump(c, OP_JF); have_test = 1;
            if (have_fall) patch(c, fall);
            want(c, ':', "expected : after case");
        } else if (is_kw(c, KW_DEFAULT)) {
            next(c);
            want(c, ':', "expected : after default");
            default_at = here(c); have_default = 1;
        } else if (c->tok.t == TK_EOF) fail(c, "a switch is never closed");
        else {
            statement(c);
            continue;
        }
        have_fall = 1;
    }
    next(c);
    end_jump = jump(c, OP_JMP);
    if (have_test) patch(c, next_test);
    if (have_default) jump_to(c, OP_JMP, default_at);
    patch(c, end_jump);
    {
        u32 i;
        for (i = 0; i < lp.nbrk; i++) patch(c, lp.brk[i]);
        c->fs->loop = lp.up;
    }
    emit(c, OP_POP);
}

static void while_statement(C *c, Str *label)
{
    Loop lp;
    u32 top = here(c), exit;
    next(c);
    want(c, '(', "expected ( after while");
    expr(c);
    want(c, ')', "expected ) after while (...)");
    exit = jump(c, OP_JF);
    loop_begin(c, &lp, label, 1);
    statement(c);
    jump_to(c, OP_JMP, top);
    patch(c, exit);
    loop_end(c, &lp, top);
}

static void do_statement(C *c, Str *label)
{
    Loop lp;
    u32 top = here(c), cont;
    next(c);
    loop_begin(c, &lp, label, 1);
    statement(c);
    cont = here(c);
    if (!is_kw(c, KW_WHILE)) fail(c, "expected while after do {...}");
    next(c);
    want(c, '(', "expected ( after while");
    expr(c);
    want(c, ')', "expected )");
    jump_to(c, OP_JT, top);
    loop_end(c, &lp, cont);
    accept(c, ';');
}

static void inline_finally(C *c, Region *r)
{
    State t;
    save(c, &t);
    c->L.pos = r->fin_pos; c->L.line = r->fin_line; c->L.regex_ok = 1;
    next(c);
    block(c);
    load(c, &t);
}

static void try_statement(C *c)
{
    State s;
    Region r;
    u32 handler, out[2];
    int nout = 0, has_catch = 0;
    next(c);
    /* find a finally first: the try block may need to run it on the way out */
    save(c, &s);
    memset(&r, 0, sizeof r);
    skip_brackets(c);
    if (is_kw(c, KW_CATCH)) { next(c); if (is_p(c, '(')) skip_brackets(c); skip_brackets(c); }
    if (is_kw(c, KW_FINALLY)) { r.fin = 1; r.fin_pos = c->tok.end; r.fin_line = c->tok.line; }
    load(c, &s);
    if (c->fs->nreg >= 24) fail(c, "try blocks nested too deep");

    handler = jump(c, OP_TRY);
    c->fs->reg[c->fs->nreg++] = r;
    block(c);
    c->fs->nreg--;
    emit(c, OP_ENDTRY);
    if (r.fin) inline_finally(c, &r);
    out[nout++] = jump(c, OP_JMP);
    patch(c, handler);                         /* the exception is on the stack */
    if (is_kw(c, KW_CATCH)) {
        u32 h2 = 0;
        has_catch = 1;
        next(c);
        if (r.fin) { h2 = jump(c, OP_TRY); c->fs->reg[c->fs->nreg++] = r; }
        if (accept(c, '(')) {
            if (is_p(c, '[') || is_p(c, '{')) destructure(c, 1, KW_LET);
            else {
                Str *n;
                if (c->tok.t != TK_IDENT) fail(c, "expected a name after catch (");
                n = c->tok.s; next(c);
                declare_local(c, c->fs, n);
                store_name(c, n);
                emit(c, OP_POP);
            }
            want(c, ')', "expected )");
        } else emit(c, OP_POP);
        block(c);
        if (r.fin) {
            c->fs->nreg--;
            emit(c, OP_ENDTRY);
            inline_finally(c, &r);
            out[nout++] = jump(c, OP_JMP);
            patch(c, h2);                      /* thrown in the catch: finally, then on */
        } else out[nout++] = jump(c, OP_JMP);
    }
    if (r.fin) {
        if (!is_kw(c, KW_FINALLY)) fail(c, "expected finally");
        next(c);
        block(c);                              /* exception still on the stack */
        emit(c, OP_THROW);
    } else if (!has_catch) fail(c, "a try needs a catch or a finally");
    while (nout) patch(c, out[--nout]);
}

static void statement(C *c)
{
    Tok *t = &c->tok;
    if (t->t == TK_PUNCT) {
        if (t->p == '{') { block(c); return; }
        if (t->p == ';') { next(c); return; }
    }
    if (t->t == TK_IDENT) {
        switch (t->kw) {
        case KW_VAR: case KW_CONST:
            { int kw = t->kw; next(c); declaration(c, kw); semi(c); return; }
        case KW_LET: {
            State s; save(c, &s);
            next(c);
            if (c->tok.t == TK_IDENT || is_p(c, '[') || is_p(c, '{')) { declaration(c, KW_LET); semi(c); return; }
            load(c, &s);
            break;
        }
        case KW_FUNCTION: {
            Str *name;
            next(c);
            if (accept(c, '*')) c->next_gen = 1;
            if (c->tok.t != TK_IDENT) fail(c, "expected a function's name");
            name = c->tok.s; next(c);
            function(c, name, 0, 0, 0);
            return;
        }
        case KW_ASYNC: {
            State s; save(c, &s);
            next(c);
            if (is_kw(c, KW_FUNCTION) && !c->tok.nl) {             /* async function name() { } */
                Str *name;
                next(c);
                if (accept(c, '*')) c->next_gen = 1;          /* async function* name() { } */
                if (c->tok.t != TK_IDENT) fail(c, "expected a function's name");
                name = c->tok.s; next(c);
                c->next_async = 1;
                function(c, name, 0, 0, 0);
                return;
            }
            load(c, &s);
            break;
        }
        case KW_CLASS: {
            Str *name;
            next(c);
            if (c->tok.t != TK_IDENT) fail(c, "expected a class's name");
            name = c->tok.s; next(c);
            declare(c, name);
            class_body(c, name, 1);
            store_name(c, name);
            emit(c, OP_POP);
            return;
        }
        case KW_IF: {
            u32 jf, je;
            next(c);
            want(c, '(', "expected ( after if");
            expr(c);
            want(c, ')', "expected ) after if (...)");
            jf = jump(c, OP_JF);
            statement(c);
            if (is_kw(c, KW_ELSE)) {
                je = jump(c, OP_JMP);
                patch(c, jf);
                next(c);
                statement(c);
                patch(c, je);
            } else patch(c, jf);
            return;
        }
        case KW_WHILE: while_statement(c, 0); return;
        case KW_DO: do_statement(c, 0); return;
        case KW_FOR: for_statement(c, 0); return;
        case KW_SWITCH: switch_statement(c, 0); return;
        case KW_BREAK: break_or_continue(c, 1); return;
        case KW_CONTINUE: break_or_continue(c, 0); return;
        case KW_RETURN:
            next(c);
            if (c->fs->top) fail(c, "return outside a function");
            if (is_p(c, ';') || is_p(c, '}') || c->tok.t == TK_EOF || c->tok.nl) emit(c, OP_UNDEF);
            else expr(c);
            if (c->fs->gen_fn && c->fs->async_fn) emit(c, OP_AWAITY);
            leave_regions(c, 0);
            emit(c, OP_RET);
            semi(c);
            return;
        case KW_THROW:
            next(c);
            if (c->tok.nl) fail(c, "nothing to throw on the line after throw");
            expr(c);
            emit(c, OP_THROW);
            semi(c);
            return;
        case KW_TRY: try_statement(c); return;
        case KW_DEBUGGER: next(c); semi(c); return;
        case KW_IMPORT: {
            State s; save(c, &s);
            next(c);
            if (is_p(c, '(') || is_p(c, '.')) { load(c, &s); break; }   /* import(...) or import.meta: an expression */
            if (!c->module) fail(c, "import in a script that is not a module");
            import_statement(c);
            return;
        }
        case KW_EXPORT:
            if (!c->module) fail(c, "export in a script that is not a module");
            export_statement(c);
            return;
        case KW_WITH: fail(c, "with is not supported");
        default:
            if (t->kw < 0 || !KW_RESERVED(t->kw)) {   /* a label?       */
                State s; Str *n = t->s;
                save(c, &s);
                next(c);
                if (is_p(c, ':')) {
                    next(c);
                    if (is_kw(c, KW_FOR)) { for_statement(c, n); return; }
                    if (is_kw(c, KW_SWITCH)) { switch_statement(c, n); return; }
                    if (is_kw(c, KW_WHILE)) { while_statement(c, n); return; }
                    if (is_kw(c, KW_DO)) { do_statement(c, n); return; }
                    {
                        Loop lp;
                        loop_begin(c, &lp, n, 0);
                        statement(c);
                        loop_end(c, &lp, here(c));
                        return;
                    }
                }
                load(c, &s);
            }
        }
    }
    expr(c);
    emit(c, OP_POP);
    semi(c);
}

/* ---- functions ----------------------------------------------------------------------- */
static Proto *finish(C *c, Fs *f)
{
    Proto *p = gc_alloc(sizeof(Proto), K_PROTO);
    u8 *code;
    u32 i;
    if (!p) fail(c, "the script is too big for the memory");
    code = gc_alloc(sizeof(GcHdr) + f->n, K_BYTES);
    if (!code) fail(c, "the script is too big for the memory");
    code += sizeof(GcHdr);
    memcpy(code, f->code, f->n);
    p->code = code;
    p->ncode = f->n;
    for (i = 0; i < c->nrefs; i++) if (c->refs[i].fs == f) { c->refs[i].p = p; c->refs[i].fs = 0; }
    settle(c, f, p);
    if (f->nconst) {
        Vals *k = gc_alloc(sizeof(Vals) + f->nconst * sizeof(Val), K_VALS);
        if (!k) fail(c, "the script is too big for the memory");
        k->n = f->nconst;
        memcpy(k->v, f->consts->v, f->nconst * sizeof(Val));
        p->consts = k;
    }
    if (f->nkids) {
        u8 *k = gc_alloc(sizeof(GcHdr) + f->nkids * sizeof(Proto *), K_BYTES);
        if (!k) fail(c, "the script is too big for the memory");
        k += sizeof(GcHdr);
        memcpy(k, f->kids, f->nkids * sizeof(Proto *));
        p->kids = (Proto **)k;
        p->nkids = f->nkids;
    }
    /* the function's working buffers: copied out above, done with now */
    drop(f->code); f->code = 0;
    if (f->consts) { gc_free(f->consts); f->consts = 0; }
    drop(f->kids); f->kids = 0;
    drop(f->hk); f->hk = 0; drop(f->hn); f->hn = 0;
    drop(f->globals); f->globals = 0;
    p->nparams = f->nparams;
    p->nslots = (u16)f->nvars;
    drop(f->vars); f->vars = 0;
    p->args_slot = f->args_slot >= 0 ? (u16)f->args_slot : 0xFFFF;
    p->self_slot = f->self_slot >= 0 ? (u16)f->self_slot : 0xFFFF;
    p->arrow = (u8)f->arrow;
    p->pad[0] = (u8)f->rest;
    p->pad[1] = (u8)(f->async_fn && !f->gen_fn);   /* an async generator's promises are its own (vm.c) */
    p->gen = (u8)(f->gen_fn ? (f->async_fn ? 2 : 1) : 0);
    p->name = f->name;
    return p;
}

/* The end of a function's code: return, then the prologue it jumps to
 * first, which makes its declared functions and comes back. */
static void prologue(C *c, Fs *f)
{
    u32 i, start;
    emit(c, OP_RETUNDEF);
    start = here(c);
    patch_to(c, 1, start);
    for (i = 0; i < f->ng; i++) { emit(c, OP_DECLGLOBAL); emit16(c, kstr(c, f->globals[i])); }
    for (i = 0; i < f->nh; i++) {
        emit(c, OP_CLOSURE); emit16(c, f->hk[i]);
        if (f->top) { emit(c, OP_SETGLOBAL); emit16(c, kstr(c, f->hn[i])); emit(c, 0); }
        else {
            int slot = declare_local(c, f, f->hn[i]);
            emit(c, OP_SETVAR); emit(c, 0); emit16(c, (u32)slot);
        }
        emit(c, OP_POP);
    }
    jump_to(c, OP_JMP, 5);
}

static void fs_open(C *c, Fs *f, int arrow, int top)
{
    memset(f, 0, sizeof *f);
    f->up = c->fs;
    f->arrow = arrow;
    f->top = top;
    f->args_slot = f->self_slot = -1;
    c->fs = f;
    jump(c, OP_JMP);                     /* to the prologue                 */
}

/* method: 0 plain, 1 a method or getter, 2 a class constructor, 3 an arrow
 * whose single parameter (name) has been read already. */
static void function(C *c, Str *name, int arrow, int is_expr, int method)
{
    Fs f, *outer = c->fs;
    Proto *p;
    u16 kid;
    int keep_no_in = c->no_in;
    /* Defaults and patterns, done in the parameters' order as the body starts. */
    State acts[48]; int act_slot[48], act_pat[48], nact = 0, npat = 0;

    fs_open(c, &f, arrow, 0);
    f.async_fn = c->next_async;
    f.gen_fn = c->next_gen;
    c->next_async = 0;
    c->next_gen = 0;
    f.name = (method == 3) ? 0 : name;
    c->no_in = 0;
    if (method == 3) {
        declare_local(c, &f, name);
        f.nparams = 1;
    } else {
        want(c, '(', "expected ( to start the parameters");
        while (!is_p(c, ')')) {
            if (accept(c, P3('.', '.', '.'))) {
                Str *n;
                if (c->tok.t != TK_IDENT) fail(c, "expected a name after ...");
                n = c->tok.s; next(c);
                declare_local(c, &f, n);
                f.nparams++;
                f.rest = 1;
                break;
            }
            if (is_p(c, '[') || is_p(c, '{')) {
                char b[8]; int k = 0, n = f.nparams;
                b[k++] = '%'; b[k++] = 'p';
                do { b[k++] = (char)('0' + n % 10); n /= 10; } while (n);
                b[k] = 0;
                if (npat >= 16 || nact >= 46) fail(c, "too many patterns in the parameters");
                npat++;
                act_slot[nact] = declare_local(c, &f, str_intern(str_ascii(b)));
                act_pat[nact] = 1;
                save(c, &acts[nact++]);
                skip_brackets(c);
                f.nparams++;
                if (is_p(c, '=')) {                  /* ({a} = {}): the default, then the pattern */
                    next(c);
                    acts[nact] = acts[nact - 1];
                    act_slot[nact] = act_slot[nact - 1]; act_pat[nact] = 1;
                    save(c, &acts[nact - 1]);
                    act_pat[nact - 1] = 0;
                    nact++;
                    while (!is_p(c, ',') && !is_p(c, ')')) {
                        if (c->tok.t == TK_EOF) fail(c, "the parameters never end");
                        skip_one(c);
                    }
                }
            } else {
                Str *n;
                int slot;
                if (c->tok.t != TK_IDENT || KW_RESERVED(c->tok.kw)) fail(c, "expected a parameter's name");
                n = c->tok.s; next(c);
                slot = declare_local(c, &f, n);
                if ((int)f.nparams != slot) {   /* a repeated name: give it its own place */
                    char b[8]; int k = 0, m = f.nparams;
                    b[k++] = '%'; b[k++] = 'd';
                    do { b[k++] = (char)('0' + m % 10); m /= 10; } while (m);
                    b[k] = 0;
                    slot = declare_local(c, &f, str_intern(str_ascii(b)));
                }
                f.nparams++;
                if (is_p(c, '=')) {
                    if (nact >= 46) fail(c, "too many defaults");
                    next(c);
                    act_slot[nact] = slot; act_pat[nact] = 0;
                    save(c, &acts[nact++]);
                    /* skip the default expression: up to , or ) at this depth */
                    while (!is_p(c, ',') && !is_p(c, ')')) {
                        if (c->tok.t == TK_EOF) fail(c, "the parameters never end");
                        skip_one(c);
                    }
                }
            }
            if (!is_p(c, ')')) want(c, ',', "expected , or ) in the parameters");
        }
        next(c);
    }
    if (arrow) {
        if (!is_p(c, P2('=', '>'))) fail(c, "expected =>");
        next(c);
    }
    /* a named function expression can call itself by its name */
    if (is_expr && name && !arrow && method == 0) f.self_slot = declare_local(c, &f, name);
    {
        int i;
        State after;
        save(c, &after);
        for (i = 0; i < nact; i++) {
            u32 j;
            load(c, &acts[i]);
            emit(c, OP_GETVAR); emit(c, 0); emit16(c, (u32)act_slot[i]);
            if (act_pat[i]) { destructure(c, 1, KW_LET); continue; }
            emit(c, OP_UNDEF); emit(c, OP_SEQ);
            j = jump(c, OP_JF);
            assign(c);
            emit(c, OP_SETVAR); emit(c, 0); emit16(c, (u32)act_slot[i]);
            emit(c, OP_POP);
            patch(c, j);
        }
        load(c, &after);
    }
    if (method == 2 && c->fields_name && !c->super_name) {   /* a class's fields, as it starts */
        emit(c, OP_THIS); emit(c, OP_DUP);
        emit(c, OP_GETPROP); emit16(c, kstr(c, c->fields_name));
        emit(c, OP_CALLM); emit(c, 0);
        emit(c, OP_POP); emit(c, OP_POP);
    }
    if (arrow && !is_p(c, '{')) {
        assign(c);
        emit(c, OP_RET);
    } else {
        want(c, '{', "expected { to start a function's body");
        if (f.gen_fn) { emit(c, OP_GENSTART); emit(c, OP_POP); }   /* made, not run: next() runs it */
        while (!is_p(c, '}')) {
            if (c->tok.t == TK_EOF) fail(c, "a function is never closed");
            statement(c);
        }
        next(c);
    }
    prologue(c, &f);
    p = finish(c, &f);
    c->fs = outer;
    c->no_in = keep_no_in;
    if (outer->nkids >= 65535) fail(c, "too many functions in one function");
    if (outer->nkids == outer->ckids) {
        Fs *keep = c->fs;
        outer->kids = grow(c, outer->kids, outer->nkids, &outer->ckids, sizeof(Proto *));
        c->fs = keep;
    }
    kid = (u16)outer->nkids;
    outer->kids[outer->nkids++] = p;
    if (!is_expr) {                          /* a declaration: made at the start */
        if (outer->nh == outer->ch) {
            u32 cap = outer->ch;
            outer->hk = grow(c, outer->hk, outer->nh, &cap, sizeof(u16));
            outer->hn = grow(c, outer->hn, outer->nh, &outer->ch, sizeof(Str *));
        }
        outer->hk[outer->nh] = kid;
        outer->hn[outer->nh++] = name;
        if (!outer->top) declare_local(c, outer, name);
        else declare(c, name);
        return;
    }
    emit(c, OP_CLOSURE); emit16(c, kid);
}

/* ---- modules -------------------------------------------------------------------------
 * A module is compiled as one function.  Before its body, a quick pass
 * over its top level finds every import and export.  Its start then
 * takes each module it imports from (already run, in browzer's order)
 * into a hidden local, __flopos_m0.., and defines each export on its own
 * namespace as a getter, ()=>local, so an importer always sees the
 * current value (imports are live, as in a browser).  A name imported is
 * read as __flopos_mN.name wherever it is used.  The import and export
 * statements themselves then compile to nothing, or to the declaration
 * they carry. */
static int word_is(C *c, const char *w) { return c->tok.t == TK_IDENT && c->tok.s && str_eq_ascii(c->tok.s, w); }

static Str *mod_local(int i)
{
    char b[24] = "__flopos_m";
    int k = 10, t = 0; char d[8];
    do { d[t++] = (char)('0' + i % 10); i /= 10; } while (i);
    while (t) b[k++] = d[--t];
    b[k] = 0;
    return str_intern(str_ascii(b));
}

static int spec_index(C *c, Str *spec)
{
    int i;
    spec = str_intern(spec);
    for (i = 0; i < c->nspec; i++) if (c->mspec[i] == spec || str_eq(c->mspec[i], spec)) return i;
    if (c->nspec >= MOD_SPECS) fail(c, "a module that imports from too many places");
    c->mspec[c->nspec] = spec;
    return c->nspec++;
}

static Str *export_name(C *c)                /* a name, or "a string" */
{
    Str *n;
    if (c->tok.t != TK_IDENT && c->tok.t != TK_STR) fail(c, "expected a name in an import or export");
    n = str_intern(c->tok.s);
    next(c);
    return n;
}

static void skip_attributes(C *c)            /* with { type: "json" } */
{
    if ((is_kw(c, KW_WITH) || word_is(c, "assert")) && !c->tok.nl) { next(c); if (is_p(c, '{')) skip_brackets(c); }
}

static int from_spec(C *c)                   /* from "x" */
{
    int i;
    if (!word_is(c, "from")) fail(c, "expected from");
    next(c);
    if (c->tok.t != TK_STR) fail(c, "expected the address to import from");
    i = spec_index(c, c->tok.s);
    next(c);
    skip_attributes(c);
    return i;
}

/* After `import`: its bindings, noted when record is set. */
static void import_clause(C *c, int record)
{
    Str *loc[256], *nm[256];
    int n = 0, i, sp;
    if (c->tok.t == TK_STR) {                /* import "x": run it, take nothing */
        spec_index(c, c->tok.s);
        next(c);
        skip_attributes(c);
        return;
    }
    if (c->tok.t == TK_IDENT) {              /* import X ... */
        loc[n] = str_intern(c->tok.s); nm[n++] = atom("default");
        next(c);
        accept(c, ',');
    }
    if (accept(c, '*')) {
        if (!word_is(c, "as")) fail(c, "expected as after import *");
        next(c);
        if (c->tok.t != TK_IDENT) fail(c, "expected a name");
        loc[n] = str_intern(c->tok.s); nm[n++] = 0;
        next(c);
    } else if (accept(c, '{')) {
        while (!is_p(c, '}')) {
            Str *name = export_name(c), *local = name;
            if (word_is(c, "as")) { next(c); if (c->tok.t != TK_IDENT) fail(c, "expected a name"); local = str_intern(c->tok.s); next(c); }
            if (n >= 256) fail(c, "too many names in one import");
            loc[n] = local; nm[n++] = name;
            if (!is_p(c, '}')) want(c, ',', "expected , or } in an import");
        }
        next(c);
    }
    sp = from_spec(c);
    if (!record) return;
    for (i = 0; i < n; i++) {
        if (c->nimp >= MOD_IMPORTS) fail(c, "a module that imports too many names");
        c->imp[c->nimp].local = loc[i]; c->imp[c->nimp].name = nm[i]; c->imp[c->nimp].spec = sp;
        c->nimp++;
    }
}

static void add_export(C *c, Str *name, Str *local, int spec, int kind)
{
    if (c->nexp >= MOD_EXPORTS) fail(c, "a module that exports too many names");
    c->exp[c->nexp].name = name; c->exp[c->nexp].local = local;
    c->exp[c->nexp].spec = spec; c->exp[c->nexp].kind = kind;
    c->nexp++;
}

/* The names a declaration after `export` makes: var a = 1, {b, c: d} = o, ... */
static void declared_names(C *c)
{
    for (;;) {
        if (is_p(c, '{') || is_p(c, '[')) {          /* a pattern: the names bound in it */
            int depth = 0;
            do {
                if (is_p(c, '{') || is_p(c, '[')) depth++;
                else if (is_p(c, '}') || is_p(c, ']')) depth--;
                else if (c->tok.t == TK_IDENT && c->tok.kw < 0) {
                    Str *n = c->tok.s;
                    next(c);
                    if (is_p(c, ',') || is_p(c, '}') || is_p(c, ']') || is_p(c, '='))
                        add_export(c, str_intern(n), str_intern(n), 0, EX_LOCAL);
                    continue;
                }
                next(c);
            } while (depth > 0 && c->tok.t != TK_EOF);
        } else if (c->tok.t == TK_IDENT) {
            add_export(c, str_intern(c->tok.s), str_intern(c->tok.s), 0, EX_LOCAL);
            next(c);
        } else return;
        /* its value: to the next , at this level, or the end of the statement */
        while (c->tok.t != TK_EOF && !is_p(c, ',') && !is_p(c, ';')) {
            if (c->tok.nl && (is_kw(c, KW_EXPORT) || is_kw(c, KW_IMPORT) || is_kw(c, KW_CONST) || is_kw(c, KW_LET) ||
                              is_kw(c, KW_VAR) || is_kw(c, KW_FUNCTION) || is_kw(c, KW_CLASS))) return;
            skip_one(c);
        }
        if (!accept(c, ',')) return;
    }
}

/* After `export`, in the first pass: what it exports. */
static void export_clause(C *c)
{
    if (accept(c, '*')) {
        if (word_is(c, "as")) { Str *n; next(c); n = export_name(c); add_export(c, n, 0, from_spec(c), EX_NS_FROM); }
        else add_export(c, 0, 0, from_spec(c), EX_STAR);
        return;
    }
    if (accept(c, '{')) {
        Str *loc[256], *nm[256];
        int n = 0, i;
        while (!is_p(c, '}')) {
            Str *local = export_name(c), *name = local;
            if (word_is(c, "as")) { next(c); name = export_name(c); }
            if (n >= 256) fail(c, "too many names in one export");
            loc[n] = local; nm[n++] = name;
            if (!is_p(c, '}')) want(c, ',', "expected , or } in an export");
        }
        next(c);
        if (word_is(c, "from")) { int sp = from_spec(c); for (i = 0; i < n; i++) add_export(c, nm[i], loc[i], sp, EX_FROM); }
        else for (i = 0; i < n; i++) add_export(c, nm[i], loc[i], 0, EX_LOCAL);
        return;
    }
    if (is_kw(c, KW_DEFAULT)) {
        next(c);
        if (is_kw(c, KW_ASYNC)) next(c);
        if (is_kw(c, KW_FUNCTION) || is_kw(c, KW_CLASS)) {
            next(c);
            accept(c, '*');
            if (c->tok.t == TK_IDENT && c->tok.kw < 0) { add_export(c, atom("default"), str_intern(c->tok.s), 0, EX_LOCAL); return; }
        }
        add_export(c, atom("default"), atom("__flopos_default"), 0, EX_LOCAL);
        return;
    }
    if (is_kw(c, KW_VAR) || is_kw(c, KW_LET) || is_kw(c, KW_CONST)) { next(c); declared_names(c); return; }
    if (is_kw(c, KW_ASYNC)) next(c);
    if (is_kw(c, KW_FUNCTION) || is_kw(c, KW_CLASS)) {
        next(c);
        accept(c, '*');
        if (c->tok.t == TK_IDENT) add_export(c, str_intern(c->tok.s), str_intern(c->tok.s), 0, EX_LOCAL);
        return;
    }
    fail(c, "expected what to export");
}

/* The first pass: every import and export at the top level. */
static void module_scan(C *c)
{
    State s0;
    int after_dot = 0;
    save(c, &s0);
    while (c->tok.t != TK_EOF) {
        if (!after_dot && is_kw(c, KW_IMPORT)) {
            next(c);
            if (is_p(c, '(') || is_p(c, '.')) continue;
            import_clause(c, 1);
            continue;
        }
        if (!after_dot && is_kw(c, KW_EXPORT)) { next(c); export_clause(c); continue; }
        after_dot = is_p(c, '.');
        skip_one(c);
    }
    load(c, &s0);
}

/* Is n imported here, and not hidden by a name of the function using it?
 * Then e reads it: __flopos_mN.name, or the namespace itself. */
static int import_name(C *c, Str *n, Ex *e)
{
    int i;
    Fs *f;
    for (i = 0; i < c->nimp; i++) if (c->imp[i].local == n || str_eq(c->imp[i].local, n)) break;
    if (i == c->nimp) return 0;
    for (f = c->fs; f && f != c->mod_top; f = f->up) if (find_var(f, n) >= 0) return 0;
    if (c->mod_top && find_var(c->mod_top, n) >= 0) return 0;
    ref(c, R_GET, mod_local(c->imp[i].spec));
    if (c->imp[i].name) { e->k = X_PROP; e->idx = kstr(c, c->imp[i].name); }
    else e->k = X_VAL;
    return 1;
}

static void import_statement(C *c)          /* the binding was noted in the first pass */
{
    import_clause(c, 0);
    semi(c);
}

static void export_statement(C *c)
{
    next(c);
    if (is_p(c, '*') || is_p(c, '{')) {
        int keep = c->nexp;
        export_clause(c);                    /* noted already: read past it */
        c->nexp = keep;
        semi(c);
        return;
    }
    if (is_kw(c, KW_DEFAULT)) {
        State s; int named = 0;
        next(c);
        save(c, &s);
        if (is_kw(c, KW_ASYNC)) next(c);
        if (is_kw(c, KW_FUNCTION) || is_kw(c, KW_CLASS)) {
            next(c);
            accept(c, '*');
            named = c->tok.t == TK_IDENT && c->tok.kw < 0;
        }
        load(c, &s);
        if (named) { statement(c); return; }
        assign(c);
        store_name(c, atom("__flopos_default"));
        emit(c, OP_POP);
        semi(c);
        return;
    }
    statement(c);                            /* var, let, const, function, class */
}

/* import(x) or import.meta, in any script. */
static void import_expr(C *c, Ex *e)
{
    next(c);
    if (accept(c, '.')) {
        if (!word_is(c, "meta")) fail(c, "expected import.meta");
        next(c);
        ref(c, R_GET, atom("__flopos_meta"));
        e->k = X_VAL;
        return;
    }
    if (!is_p(c, '(')) fail(c, "expected ( after import");
    next(c);
    ref(c, R_GET, atom("__flopos_import"));
    assign(c);
    if (accept(c, ',') && !is_p(c, ')')) { assign(c); emit(c, OP_POP); accept(c, ','); }
    want(c, ')', "expected ) after import(");
    if (c->module) ref(c, R_GET, atom("__flopos_meta")); else emit(c, OP_UNDEF);
    emit(c, OP_CALL); emit(c, 2);
    e->k = X_VAL;
}

/* A getter, ()=>what or ()=>what["prop"], compiled from text made here. */
static void getter_of(C *c, Str *what, Str *prop)
{
    static u16 src[700];
    u32 n = 0, i;
    State s;
    src[n++] = '('; src[n++] = ')'; src[n++] = '='; src[n++] = '>';
    for (i = 0; i < what->len && n < 300; i++) src[n++] = what->c[i];
    if (prop) {
        src[n++] = '['; src[n++] = '"';
        for (i = 0; i < prop->len && n < 690; i++) {
            if (prop->c[i] == '"' || prop->c[i] == '\\') src[n++] = '\\';
            src[n++] = prop->c[i];
        }
        src[n++] = '"'; src[n++] = ']';
    }
    save(c, &s);
    lex_start(&c->L, src, n);
    next(c);
    function(c, 0, 1, 1, 0);
    load(c, &s);
}

/* The module's start: its imports, then its exports as getters. */
static void module_start(C *c)
{
    int i;
    Fs *top = c->fs;
    for (i = 0; i < c->nspec; i++) {
        declare_local(c, top, mod_local(i));
        ref(c, R_GET, atom("__flopos_modns"));
        push_num(c, i);
        emit(c, OP_CALL); emit(c, 1);
        store_name(c, mod_local(i));
        emit(c, OP_POP);
    }
    declare_local(c, top, atom("__flopos_default"));
    declare_local(c, top, atom("__flopos_meta"));
    ref(c, R_GET, atom("__flopos_modmeta"));
    emit(c, OP_CALL); emit(c, 0);
    store_name(c, atom("__flopos_meta"));
    emit(c, OP_POP);
    ref(c, R_GET, atom("__flopos_modself"));
    emit(c, OP_CALL); emit(c, 0);            /* [ns] */
    for (i = 0; i < c->nexp; i++) {
        int k = c->exp[i].kind, j;
        if (k == EX_STAR) continue;
        if (k == EX_LOCAL) {
            for (j = 0; j < c->nimp; j++) if (str_eq(c->imp[j].local, c->exp[i].local)) break;
            if (j < c->nimp) getter_of(c, mod_local(c->imp[j].spec), c->imp[j].name);   /* passed on */
            else getter_of(c, c->exp[i].local, 0);
        } else getter_of(c, mod_local(c->exp[i].spec), k == EX_FROM ? c->exp[i].local : 0);
        emit(c, OP_INITGET); emit16(c, kstr(c, c->exp[i].name));
    }
    emit(c, OP_POP);
    for (i = 0; i < c->nexp; i++) {
        if (c->exp[i].kind != EX_STAR) continue;
        ref(c, R_GET, atom("__flopos_star"));
        push_num(c, c->exp[i].spec);
        emit(c, OP_CALL); emit(c, 1);
        emit(c, OP_POP);
    }
}

Proto *js_compile(Vm *vm, const char *src, u32 len, const char *name, char *why, u32 whycap, int as_function)
{
    static C cs;
    C *c = &cs;
    Fs top;
    Str *text;
    Proto *p;
    memset(c, 0, sizeof *c);
    c->vm = vm; c->why = why; c->whycap = whycap;
    if (whycap) why[0] = 0;
    text = str_utf8(src, len);
    if (!text) { if (whycap > 30) memcpy(why, "the script is too big for the memory", 37 < whycap ? 37 : whycap - 1); return 0; }
    if (__builtin_setjmp(c->jb)) { c->fs = 0; return 0; }
    lex_start(&c->L, text->c, text->len);
    c->fs = 0;
    fs_open(c, &top, 0, 1);
    top.name = name ? str_ascii(name) : 0;
    next(c);
    if (as_function == 2) {                  /* a module: its own scope, imports and exports */
        top.top = 0;
        top.async_fn = 1;                    /* await at its top level */
        c->module = 1;
        c->mod_top = &top;
        module_scan(c);
        module_start(c);
    } else if (as_function) {                /* an onclick="..." handler: one function, event in hand */
        Str *ev = atom("event");
        declare_local(c, &top, ev);
        top.top = 0; top.nparams = 1;
    }
    while (c->tok.t != TK_EOF) statement(c);
    {
        int i;
        g_mod_nspec = c->nspec;
        for (i = 0; i < c->nspec; i++) g_mod_spec[i] = c->mspec[i];
    }
    prologue(c, &top);
    p = finish(c, &top);
    (void)text;
    return p;
}
