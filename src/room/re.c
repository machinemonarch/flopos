/* ===========================================================================
 * re.c -- regular expressions.
 *
 * A pattern is compiled to a small program: match this character, any
 * of these, try this way first and that way if it fails.  Matching tries
 * the ways one after another and backs up when one fails; what it must
 * come back to is kept on a list of its own, not on the C stack, so a
 * long text cannot make it overflow.  Every step it takes is counted
 * against the script's time, so a pattern that would take forever stops
 * like any other slow script.
 *
 * Supported: . [] [^] \d \w \s \b and their opposites, ^ $, groups,
 * (?:), (?=), (?!), named groups (numbered only), | and the repeats
 * * + ? {n,m} with lazy forms, back-references, and the flags g i m s y.
 * ===========================================================================
 */
#include "js.h"

#define ARG(i) ((i) < argc ? args[i] : v_undef())
#define NATIVE(name) static Val name(Vm *vm, Val self, Val *args, int argc, u32 magic)
#define UNUSED (void)self; (void)args; (void)argc; (void)magic

enum { R_CHAR, R_ANY, R_CLASS, R_MATCH, R_JMP, R_SPLIT, R_SAVE, R_BOL, R_EOL,
       R_WORDB, R_NWORDB, R_BACKREF, R_LOOK, R_LOOPSET, R_LOOPCHK };

typedef struct { u8 op, neg; u16 a; s32 x, y; } Inst;   /* x, y: relative jumps */

#define F_G 1
#define F_I 2
#define F_M 4
#define F_S 8
#define F_Y 16

#define MAXINST 4096
#define MAXRANGE 2048
#define NSLOT 96                         /* 64 for 32 groups, 32 for loops */

typedef struct {
    const u16 *p; u32 n, i;
    Inst *code; u32 ncode;
    u16 *ranges; u32 nranges;
    u32 groups, loops;
    const char *err;
} RC;

typedef struct {
    u32 ninst, nranges, groups, loops;
    /* Inst code[ninst]; u16 ranges[nranges] follow */
} ReProg;

static Inst *I_(RC *c)
{
    if (c->ncode >= MAXINST) { c->err = "too long"; return &c->code[0]; }
    memset(&c->code[c->ncode], 0, sizeof(Inst));
    return &c->code[c->ncode++];
}

static int add_range(RC *c, u32 a, u32 b)
{
    if (c->nranges + 2 > MAXRANGE) { c->err = "too many ranges"; return 0; }
    c->ranges[c->nranges++] = (u16)a;
    c->ranges[c->nranges++] = (u16)b;
    return 1;
}

static const u16 R_DIGIT[] = { '0', '9' };
static const u16 R_WORD[] = { '0', '9', 'A', 'Z', '_', '_', 'a', 'z' };
static const u16 R_SPACE[] = { 9, 13, 32, 32, 0xA0, 0xA0, 0x1680, 0x1680, 0x2000, 0x200A,
    0x2028, 0x2029, 0x202F, 0x202F, 0x205F, 0x205F, 0x3000, 0x3000, 0xFEFF, 0xFEFF };

static void add_set(RC *c, const u16 *r, u32 n, int neg)
{
    u32 i;
    if (!neg) { for (i = 0; i < n; i += 2) add_range(c, r[i], r[i + 1]); return; }
    {
        u32 lo = 0;
        for (i = 0; i < n; i += 2) {
            if (r[i] > lo) add_range(c, lo, r[i] - 1u);
            lo = r[i + 1] + 1u;
        }
        if (lo <= 0xFFFF) add_range(c, lo, 0xFFFF);
    }
}

static int hexv(u32 ch) { return ch >= '0' && ch <= '9' ? (int)(ch - '0') : (ch | 32) >= 'a' && (ch | 32) <= 'f' ? (int)((ch | 32) - 'a' + 10) : -1; }

/* An escape's single character, or -1 for a class escape (\d etc.) */
static int esc_char(RC *c, u32 e)
{
    switch (e) {
    case 'n': return '\n'; case 't': return '\t'; case 'r': return '\r';
    case 'f': return '\f'; case 'v': return 0x0B; case '0': return 0;
    case 'x': {
        int a, b;
        if (c->i + 1 >= c->n) return 'x';
        a = hexv(c->p[c->i]); b = hexv(c->p[c->i + 1]);
        if (a < 0 || b < 0) return 'x';
        c->i += 2;
        return a * 16 + b;
    }
    case 'u': {
        int v = 0, k;
        if (c->i < c->n && c->p[c->i] == '{') {
            c->i++;
            while (c->i < c->n && c->p[c->i] != '}') { int h = hexv(c->p[c->i++]); if (h < 0) return 'u'; v = v * 16 + h; }
            c->i++;
            return v > 0xFFFF ? 0xFFFD : v;
        }
        if (c->i + 3 >= c->n) return 'u';
        for (k = 0; k < 4; k++) { int h = hexv(c->p[c->i + (u32)k]); if (h < 0) return 'u'; v = v * 16 + h; }
        c->i += 4;
        return v;
    }
    case 'c':
        if (c->i < c->n) return c->p[c->i++] % 32;
        return 'c';
    }
    return (int)e;
}

static void class_escape(RC *c, u32 e, int *handled)
{
    *handled = 1;
    switch (e) {
    case 'd': add_set(c, R_DIGIT, 2, 0); break;
    case 'D': add_set(c, R_DIGIT, 2, 1); break;
    case 'w': add_set(c, R_WORD, 8, 0); break;
    case 'W': add_set(c, R_WORD, 8, 1); break;
    case 's': add_set(c, R_SPACE, 20, 0); break;
    case 'S': add_set(c, R_SPACE, 20, 1); break;
    default: *handled = 0;
    }
}

static void bracket(RC *c)
{
    Inst *in = I_(c);
    u32 start = c->nranges;
    in->op = R_CLASS;
    c->i++;                               /* past [ */
    if (c->i < c->n && c->p[c->i] == '^') { in->neg = 1; c->i++; }
    while (c->i < c->n && c->p[c->i] != ']') {
        u32 a = c->p[c->i++], b;
        if (a == '\\' && c->i < c->n) {
            u32 e = c->p[c->i++];
            int h;
            if (e == 'b') a = 8;
            else {
                class_escape(c, e, &h);
                if (h) continue;
                a = (u32)esc_char(c, e);
            }
        }
        b = a;
        if (c->i + 1 < c->n && c->p[c->i] == '-' && c->p[c->i + 1] != ']') {
            c->i++;
            b = c->p[c->i++];
            if (b == '\\' && c->i < c->n) {
                u32 e = c->p[c->i++];
                if (e == 'd' || e == 'w' || e == 's' || e == 'D' || e == 'W' || e == 'S') { add_range(c, a, a); add_range(c, '-', '-'); { int h; class_escape(c, e, &h); } continue; }
                b = (u32)esc_char(c, e);
            }
            if (b < a) { c->err = "a range backwards"; return; }
        }
        add_range(c, a, b);
    }
    if (c->i >= c->n) { c->err = "a [ is never closed"; return; }
    c->i++;
    in = &c->code[c->ncode - 1];
    in->a = (u16)start;
    in->x = (s32)((c->nranges - start) / 2);
}

static void alt(RC *c);

static void re_atom(RC *c)
{
    u32 ch = c->p[c->i];
    Inst *in;
    if (ch == '(') {
        c->i++;
        if (c->i + 1 < c->n && c->p[c->i] == '?') {
            u32 k = c->p[c->i + 1];
            if (k == ':') { c->i += 2; alt(c); }
            else if (k == '=' || k == '!') {
                u32 at;
                c->i += 2;
                in = I_(c); in->op = R_LOOK; in->neg = k == '!';
                at = c->ncode - 1;
                alt(c);
                in = I_(c); in->op = R_MATCH;
                c->code[at].x = (s32)(c->ncode - at);   /* where to go on after it */
            } else if (k == '<' && c->i + 2 < c->n && c->p[c->i + 2] != '=' && c->p[c->i + 2] != '!') {
                u32 g;
                c->i += 2;
                while (c->i < c->n && c->p[c->i] != '>') c->i++;
                c->i++;
                g = ++c->groups;
                if (g >= 32) { c->err = "too many groups"; return; }
                in = I_(c); in->op = R_SAVE; in->a = (u16)(g * 2);
                alt(c);
                in = I_(c); in->op = R_SAVE; in->a = (u16)(g * 2 + 1);
            } else { c->err = "lookbehind is not supported"; return; }
        } else {
            u32 g = ++c->groups;
            if (g >= 32) { c->err = "too many groups"; return; }
            in = I_(c); in->op = R_SAVE; in->a = (u16)(g * 2);
            alt(c);
            in = I_(c); in->op = R_SAVE; in->a = (u16)(g * 2 + 1);
        }
        if (c->i >= c->n || c->p[c->i] != ')') { if (!c->err) c->err = "a ( is never closed"; return; }
        c->i++;
        return;
    }
    if (ch == '[') { bracket(c); return; }
    c->i++;
    in = I_(c);
    switch (ch) {
    case '.': in->op = R_ANY; return;
    case '^': in->op = R_BOL; return;
    case '$': in->op = R_EOL; return;
    case '\\': {
        u32 e;
        int h;
        if (c->i >= c->n) { c->err = "a \\ at the end"; return; }
        e = c->p[c->i++];
        if (e == 'b') { in->op = R_WORDB; return; }
        if (e == 'B') { in->op = R_NWORDB; return; }
        if (e >= '1' && e <= '9') {
            u32 g = e - '0';
            while (c->i < c->n && c->p[c->i] >= '0' && c->p[c->i] <= '9' && g * 10 + (c->p[c->i] - '0') <= c->groups) g = g * 10 + (c->p[c->i++] - '0');
            in->op = R_BACKREF; in->a = (u16)g;
            return;
        }
        if (e == 'k' && c->i < c->n && c->p[c->i] == '<') { c->err = "named back-references are not supported"; return; }
        {
            u32 start = c->nranges;
            class_escape(c, e, &h);
            if (h) { in = &c->code[c->ncode - 1]; in->op = R_CLASS; in->a = (u16)start; in->x = (s32)((c->nranges - start) / 2); return; }
        }
        in->op = R_CHAR; in->a = (u16)esc_char(c, e);
        return;
    }
    case '*': case '+': case '?': c->err = "nothing to repeat"; return;
    default: in->op = R_CHAR; in->a = (u16)ch; return;
    }
}

/* Copy the code from `from` to the end, appended again. */
static void dup(RC *c, u32 from, u32 to)
{
    u32 i, n = to - from;
    if (c->ncode + n >= MAXINST) { c->err = "too long"; return; }
    for (i = 0; i < n; i++) c->code[c->ncode + i] = c->code[from + i];
    c->ncode += n;
}

/* Make room for one instruction at `at`, before the code of an atom. */
static void insert(RC *c, u32 at)
{
    if (c->ncode + 1 >= MAXINST) { c->err = "too long"; return; }
    memmove(&c->code[at + 1], &c->code[at], (c->ncode - at) * sizeof(Inst));
    memset(&c->code[at], 0, sizeof(Inst));
    c->ncode++;
}

static void star(RC *c, u32 start, int lazy, int plus)
{
    /* L: SPLIT body, out ; LOOPSET k ; body ; LOOPCHK k ; JMP L ; out:
     * (a+ is body, then the same loop) */
    u32 k = c->loops++, split;
    Inst *in;
    if (k >= 32) { c->err = "too many repeats"; return; }
    if (plus) { u32 end = c->ncode; dup(c, start, end); start = end; }
    insert(c, start);                    /* SPLIT */
    insert(c, start + 1);                /* LOOPSET */
    if (c->err) return;
    split = start;
    c->code[split + 1].op = R_LOOPSET; c->code[split + 1].a = (u16)(64 + k);
    in = I_(c); in->op = R_LOOPCHK; in->a = (u16)(64 + k);
    in = I_(c); in->op = R_JMP; in->x = (s32)split - (s32)(c->ncode - 1);
    c->code[split].op = R_SPLIT;
    c->code[split].x = lazy ? (s32)(c->ncode - split) : 1;
    c->code[split].y = lazy ? 1 : (s32)(c->ncode - split);
}

static void optional(RC *c, u32 start, int lazy)
{
    insert(c, start);
    if (c->err) return;
    c->code[start].op = R_SPLIT;
    c->code[start].x = lazy ? (s32)(c->ncode - start) : 1;
    c->code[start].y = lazy ? 1 : (s32)(c->ncode - start);
}

static int number(RC *c, u32 *out)
{
    u32 v = 0, any = 0;
    while (c->i < c->n && c->p[c->i] >= '0' && c->p[c->i] <= '9') { v = v * 10 + (c->p[c->i++] - '0'); any = 1; if (v > 1000) v = 1000; }
    *out = v;
    return (int)any;
}

static void seq(RC *c)
{
    while (c->i < c->n && c->p[c->i] != '|' && c->p[c->i] != ')' && !c->err) {
        u32 start = c->ncode, q;
        int lazy;
        re_atom(c);
        if (c->err || c->i >= c->n) break;
        q = c->p[c->i];
        if (q == '{') {
            u32 save = c->i, lo, hi;
            c->i++;
            if (!number(c, &lo)) { c->i = save; /* a plain { */ { Inst *in; c->i++; in = I_(c); in->op = R_CHAR; in->a = '{'; } continue; }
            hi = lo;
            if (c->i < c->n && c->p[c->i] == ',') { c->i++; if (!number(c, &hi)) hi = 0xFFFFFFFFu; }
            if (c->i >= c->n || c->p[c->i] != '}') { c->i = save + 1; { Inst *in = I_(c); in->op = R_CHAR; in->a = '{'; } continue; }
            c->i++;
            lazy = c->i < c->n && c->p[c->i] == '?';
            if (lazy) c->i++;
            if (hi != 0xFFFFFFFFu && hi < lo) { c->err = "a repeat backwards"; return; }
            {
                u32 end = c->ncode, k;
                if (lo == 0) {
                    /* drop the atom, then add hi optional copies (or a star) */
                    Inst keep[64];
                    u32 n = end - start;
                    if (n > 64) { c->err = "a repeated part too long"; return; }
                    memcpy(keep, &c->code[start], n * sizeof(Inst));
                    c->ncode = start;
                    if (hi == 0xFFFFFFFFu) { memcpy(&c->code[c->ncode], keep, n * sizeof(Inst)); c->ncode += n; star(c, start, lazy, 0); }
                    else for (k = 0; k < hi && !c->err; k++) {
                        u32 s2 = c->ncode;
                        if (c->ncode + n >= MAXINST) { c->err = "too long"; return; }
                        memcpy(&c->code[c->ncode], keep, n * sizeof(Inst)); c->ncode += n;
                        optional(c, s2, lazy);
                    }
                } else {
                    for (k = 1; k < lo && !c->err; k++) dup(c, start, end);
                    if (hi == 0xFFFFFFFFu) { u32 s2 = c->ncode; dup(c, start, end); star(c, s2, lazy, 0); }
                    else for (k = lo; k < hi && !c->err; k++) { u32 s2 = c->ncode; dup(c, start, end); optional(c, s2, lazy); }
                }
            }
            continue;
        }
        if (q != '*' && q != '+' && q != '?') continue;
        c->i++;
        lazy = c->i < c->n && c->p[c->i] == '?';
        if (lazy) c->i++;
        if (q == '?') optional(c, start, lazy);
        else star(c, start, lazy, q == '+');
    }
}

static void alt(RC *c)
{
    u32 start = c->ncode;
    seq(c);
    while (!c->err && c->i < c->n && c->p[c->i] == '|') {
        u32 jmp;
        c->i++;
        insert(c, start);                /* SPLIT left, right */
        jmp = c->ncode;
        { Inst *in = I_(c); in->op = R_JMP; }
        c->code[start].op = R_SPLIT;
        c->code[start].x = 1;
        c->code[start].y = (s32)(c->ncode - start);
        seq(c);
        c->code[jmp].x = (s32)(c->ncode - jmp);
    }
}

static u32 flags_of(Str *f, int *bad)
{
    u32 r = 0, i;
    *bad = 0;
    for (i = 0; f && i < f->len; i++) {
        switch (f->c[i]) {
        case 'g': r |= F_G; break; case 'i': r |= F_I; break; case 'm': r |= F_M; break;
        case 's': r |= F_S; break; case 'y': r |= F_Y; break; case 'u': case 'd': break;
        default: *bad = 1;
        }
    }
    return r;
}

Obj *re_new(Vm *vm, Str *src, Str *flags)
{
    static Inst code[MAXINST];
    static u16 ranges[MAXRANGE];
    RC c;
    Obj *re;
    ReProg *p;
    u8 *blob;
    int bad;
    u32 size;
    memset(&c, 0, sizeof c);
    c.p = src->c; c.n = src->len; c.code = code; c.ranges = ranges;
    {   Inst *in = I_(&c); in->op = R_SAVE; in->a = 0; }
    alt(&c);
    if (!c.err && c.i < c.n) c.err = "an unmatched )";
    { Inst *in = I_(&c); in->op = R_SAVE; in->a = 1; }
    { Inst *in = I_(&c); in->op = R_MATCH; }
    if (c.err) {
        char b[160]; u32 n = 0; const char *e = c.err;
        u32 k = str_to_utf8(src, b, 100);
        b[k] = 0;
        (void)n;
        throw_err(vm, E_SYNTAX, "bad regular expression /%s/: %s", b, e);
        return 0;
    }
    flags_of(flags, &bad);
    if (bad) { throw_err(vm, E_SYNTAX, "bad regular expression flags"); return 0; }
    size = sizeof(ReProg) + c.ncode * sizeof(Inst) + c.nranges * 2;
    re = obj_new(vm, vm->regexp_proto, C_REGEXP);
    if (!re) return 0;
    TEMP(vm, v_obj(re));
    blob = gc_alloc(sizeof(GcHdr) + size, K_BYTES);
    UNTEMP(vm, 1);
    if (!blob) { vm->out_of_room = 1; return 0; }
    blob += sizeof(GcHdr);
    p = (ReProg *)blob;
    p->ninst = c.ncode; p->nranges = c.nranges; p->groups = c.groups; p->loops = c.loops;
    memcpy(blob + sizeof(ReProg), code, c.ncode * sizeof(Inst));
    memcpy(blob + sizeof(ReProg) + c.ncode * sizeof(Inst), ranges, c.nranges * 2);
    re->u.re.src = src;
    re->u.re.prog = p;
    re->u.re.flags = flags_of(flags, &bad);
    prop_put(vm, re, S_lastIndex, v_num(0), P_HIDDEN);
    return re;
}

/* ---- matching -------------------------------------------------------------------- */
typedef struct { u32 pc, sp; s32 slot; u32 old; } Bt;     /* slot >= 0: undo a save */

static u16 fold(u16 c)
{
    if (c >= 'A' && c <= 'Z') return (u16)(c + 32);
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return (u16)(c + 32);
    if (c >= 0x410 && c <= 0x42F) return (u16)(c + 32);
    return c;
}

static int in_class(const u16 *r, u32 n, u16 ch, int icase)
{
    u32 i;
    for (i = 0; i < n; i++) if (ch >= r[2 * i] && ch <= r[2 * i + 1]) return 1;
    if (icase) {
        u16 a = fold(ch), b = ch;
        if (ch >= 'a' && ch <= 'z') b = (u16)(ch - 32);
        else if (ch >= 0xE0 && ch <= 0xFE && ch != 0xF7) b = (u16)(ch - 32);
        else if (ch >= 0x430 && ch <= 0x44F) b = (u16)(ch - 32);
        for (i = 0; i < n; i++) if ((a >= r[2 * i] && a <= r[2 * i + 1]) || (b >= r[2 * i] && b <= r[2 * i + 1])) return 1;
    }
    return 0;
}

static int is_word(u16 c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }

#define BTMAX 8000
static Bt s_bt[BTMAX];

static int run(Vm *vm, ReProg *p, Str *s, u32 start_pc, u32 at, u32 *slot, u32 flags, u32 depth)
{
    Inst *code = (Inst *)(p + 1);
    u16 *ranges = (u16 *)(code + p->ninst);
    u32 nb = 0, base, pc = start_pc, sp = at;
    static u32 s_btop;                   /* lookaheads share the list above the caller's */
    base = s_btop;
    nb = base;
    for (;;) {
        Inst *in;
        if ((++vm->steps & 4095) == 0 && !vm_check_time(vm)) { s_btop = base; return 0; }
        in = &code[pc];
        switch (in->op) {
        case R_CHAR:
            if (sp < s->len && (s->c[sp] == in->a || ((flags & F_I) && fold(s->c[sp]) == fold(in->a)))) { sp++; pc++; continue; }
            goto fail;
        case R_ANY:
            if (sp < s->len && ((flags & F_S) || (s->c[sp] != '\n' && s->c[sp] != '\r' && s->c[sp] != 0x2028 && s->c[sp] != 0x2029))) { sp++; pc++; continue; }
            goto fail;
        case R_CLASS:
            if (sp < s->len && in_class(ranges + in->a, (u32)in->x, s->c[sp], flags & F_I) != in->neg) { sp++; pc++; continue; }
            goto fail;
        case R_MATCH:
            slot[1] = slot[1];
            s_btop = base;
            if (depth) { slot[95] = sp; }
            return 1;
        case R_JMP: pc = (u32)((s32)pc + in->x); continue;
        case R_SPLIT:
            if (nb >= BTMAX) { throw_err(vm, E_RANGE, "a regular expression too complicated for this text"); s_btop = base; return 0; }
            s_bt[nb].pc = (u32)((s32)pc + in->y); s_bt[nb].sp = sp; s_bt[nb].slot = -1; nb++;
            pc = (u32)((s32)pc + in->x);
            continue;
        case R_SAVE: case R_LOOPSET:
            if (nb >= BTMAX) { throw_err(vm, E_RANGE, "a regular expression too complicated for this text"); s_btop = base; return 0; }
            s_bt[nb].slot = in->a; s_bt[nb].old = slot[in->a]; s_bt[nb].pc = 0; s_bt[nb].sp = 0; nb++;
            slot[in->a] = sp;
            pc++;
            continue;
        case R_LOOPCHK:
            if (sp == slot[in->a]) goto fail;     /* the loop matched nothing: no going round again */
            pc++;
            continue;
        case R_BOL:
            if (sp == 0 || ((flags & F_M) && (s->c[sp - 1] == '\n' || s->c[sp - 1] == '\r'))) { pc++; continue; }
            goto fail;
        case R_EOL:
            if (sp == s->len || ((flags & F_M) && (s->c[sp] == '\n' || s->c[sp] == '\r'))) { pc++; continue; }
            goto fail;
        case R_WORDB: case R_NWORDB: {
            int a = sp > 0 && is_word(s->c[sp - 1]), b = sp < s->len && is_word(s->c[sp]);
            if ((a != b) == (in->op == R_WORDB)) { pc++; continue; }
            goto fail;
        }
        case R_BACKREF: {
            u32 g = in->a, from = slot[g * 2], to = slot[g * 2 + 1], i;
            if (g * 2 + 1 >= 64 || from == 0xFFFFFFFFu || to == 0xFFFFFFFFu || to < from) { pc++; continue; }
            if (sp + (to - from) > s->len) goto fail;
            for (i = 0; i < to - from; i++) {
                u16 x = s->c[from + i], y = s->c[sp + i];
                if (x != y && !((flags & F_I) && fold(x) == fold(y))) goto fail;
            }
            sp += to - from; pc++;
            continue;
        }
        case R_LOOK: {
            u32 inner[NSLOT], k;
            int ok;
            if (depth > 8) goto fail;
            for (k = 0; k < NSLOT; k++) inner[k] = slot[k];
            s_btop = nb;
            ok = run(vm, p, s, pc + 1, sp, inner, flags, depth + 1);
            s_btop = base;
            if (vm->throwing) return 0;
            if (ok != in->neg) {
                if (!in->neg) for (k = 0; k < 64; k++) if (k > 1) slot[k] = inner[k];
                pc = (u32)((s32)pc + in->x);
                continue;
            }
            goto fail;
        }
        }
    fail:
        for (;;) {
            if (nb == base) { s_btop = base; return 0; }
            nb--;
            if (s_bt[nb].slot >= 0) { slot[s_bt[nb].slot] = s_bt[nb].old; continue; }
            pc = s_bt[nb].pc; sp = s_bt[nb].sp;
            break;
        }
    }
}

/* Find a match at or after from.  Returns the match array ([whole,
 * groups..., index, input]) and its place. */
Val re_exec(Vm *vm, Obj *re, Str *s, u32 from, int *found, u32 *ms, u32 *me)
{
    ReProg *p = re->u.re.prog;
    u32 slot[NSLOT], i, at;
    *found = 0;
    if (!p) return v_null();
    for (at = from; at <= s->len; at++) {
        for (i = 0; i < NSLOT; i++) slot[i] = 0xFFFFFFFFu;
        if (run(vm, p, s, 0, at, slot, re->u.re.flags, 0)) {
            Obj *m;
            *found = 1; *ms = slot[0]; *me = slot[1];
            m = arr_new(vm, 0);
            if (!m) return v_null();
            TEMP(vm, v_obj(m));
            for (i = 0; i <= p->groups; i++) {
                if (slot[i * 2] != 0xFFFFFFFFu && slot[i * 2 + 1] != 0xFFFFFFFFu && slot[i * 2 + 1] >= slot[i * 2])
                    arr_push(vm, m, v_str(str_slice(s, slot[i * 2], slot[i * 2 + 1])));
                else arr_push(vm, m, v_undef());
            }
            prop_put(vm, m, S_index, v_num(slot[0]), 0);
            prop_put(vm, m, S_input, v_str(s), 0);
            UNTEMP(vm, 1);
            return v_obj(m);
        }
        if (vm->throwing) return v_null();
        if (re->u.re.flags & F_Y) break;   /* sticky: only right here */
    }
    return v_null();
}

/* ---- RegExp for scripts ------------------------------------------------------------- */
NATIVE(regexp_ctor)
{
    Str *src, *fl;
    Obj *re;
    UNUSED;
    if (ARG(0).t == T_OBJ && ARG(0).u.o->cls == C_REGEXP) {
        src = ARG(0).u.o->u.re.src;
        if (ARG(1).t == T_UNDEF) {
            char f[8]; int n = 0; u32 x = ARG(0).u.o->u.re.flags;
            if (x & F_G) f[n++] = 'g';
            if (x & F_I) f[n++] = 'i';
            if (x & F_M) f[n++] = 'm';
            if (x & F_S) f[n++] = 's';
            if (x & F_Y) f[n++] = 'y';
            f[n] = 0;
            fl = str_ascii(f);
        } else fl = to_str(vm, ARG(1));
    } else {
        src = ARG(0).t == T_UNDEF ? atom("(?:)") : to_str(vm, ARG(0));
        TEMP(vm, v_str(src));
        fl = ARG(1).t == T_UNDEF ? S_empty : to_str(vm, ARG(1));
        UNTEMP(vm, 1);
    }
    TEMP(vm, v_str(src)); TEMP(vm, v_str(fl));
    re = re_new(vm, src, fl);
    UNTEMP(vm, 2);
    return re ? v_obj(re) : v_undef();
}

static Obj *this_re(Vm *vm, Val self)
{
    if (self.t == T_OBJ && self.u.o->cls == C_REGEXP) return self.u.o;
    throw_err(vm, E_TYPE, "a RegExp method on something that is not one");
    return 0;
}

NATIVE(re_exec_n)                          /* 0 exec, 1 test */
{
    Obj *re = this_re(vm, self);
    Str *s;
    u32 from = 0, ms, me;
    int found, moving;
    Val m;
    UNUSED;
    if (!re) return v_undef();
    s = to_str(vm, ARG(0));
    TEMP(vm, v_str(s));
    moving = (re->u.re.flags & (F_G | F_Y)) != 0;
    if (moving) {
        f64 li = to_num(vm, get_prop(vm, self, S_lastIndex));
        if (m_isnan(li) || li < 0) li = 0;
        if (li > s->len) { set_prop(vm, self, S_lastIndex, v_num(0)); UNTEMP(vm, 1); return magic ? v_bool(0) : v_null(); }
        from = (u32)li;
    }
    m = re_exec(vm, re, s, from, &found, &ms, &me);
    UNTEMP(vm, 1);
    if (vm->throwing) return v_undef();
    if (moving) set_prop(vm, self, S_lastIndex, v_num(found ? me : 0));
    if (magic) return v_bool(found);
    return found ? m : v_null();
}

NATIVE(re_tostring)
{
    Obj *re = this_re(vm, self);
    char b[8]; int n = 0;
    Str *r;
    UNUSED;
    if (!re) return v_undef();
    if (magic == 1) return v_str(re->u.re.src);     /* source */
    if (re->u.re.flags & F_G) b[n++] = 'g';
    if (re->u.re.flags & F_I) b[n++] = 'i';
    if (re->u.re.flags & F_M) b[n++] = 'm';
    if (re->u.re.flags & F_S) b[n++] = 's';
    if (re->u.re.flags & F_Y) b[n++] = 'y';
    b[n] = 0;
    if (magic == 2) return v_str(str_ascii(b));     /* flags  */
    if (magic >= 3) {
        static const u32 bit[] = { F_G, F_I, F_M, F_S, F_Y };
        return v_bool((re->u.re.flags & bit[magic - 3]) != 0);
    }
    r = str_concat(atom("/"), re->u.re.src);
    TEMP(vm, v_str(r));
    r = str_concat(r, atom("/"));
    UNTEMP(vm, 1);
    return v_str(str_concat(r, str_ascii(b)));
}

void re_init(Vm *vm)
{
    Obj *o = vm->regexp_proto, *c = native_new(vm, "RegExp", regexp_ctor, 2, 0);
    c->flags |= OF_CTOR;
    prop_put(vm, c, S_prototype, v_obj(o), P_HIDDEN);
    prop_put(vm, o, S_constructor, v_obj(c), P_HIDDEN);
    def_val(vm, vm->global, "RegExp", v_obj(c));
    def_native(vm, o, "exec", re_exec_n, 1, 0);
    def_native(vm, o, "test", re_exec_n, 1, 1);
    def_native(vm, o, "toString", re_tostring, 0, 0);
    def_accessor(vm, o, "source", re_tostring, 0, 1);
    def_accessor(vm, o, "flags", re_tostring, 0, 2);
    def_accessor(vm, o, "global", re_tostring, 0, 3);
    def_accessor(vm, o, "ignoreCase", re_tostring, 0, 4);
    def_accessor(vm, o, "multiline", re_tostring, 0, 5);
    def_accessor(vm, o, "dotAll", re_tostring, 0, 6);
    def_accessor(vm, o, "sticky", re_tostring, 0, 7);
}
