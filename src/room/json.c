/* ===========================================================================
 * json.c -- JSON.parse and JSON.stringify.
 *
 * Reading is strict JSON, nothing more, nested at most 200 deep.  Writing
 * collects the pieces in a list and joins them once at the end.
 * ===========================================================================
 */
#include "js.h"

#define ARG(i) ((i) < argc ? args[i] : v_undef())
#define NATIVE(name) static Val name(Vm *vm, Val self, Val *args, int argc, u32 magic)
#define UNUSED (void)self; (void)args; (void)argc; (void)magic

Str *array_join(Vm *vm, Obj *a, Str *sep, int depth);

/* ---- reading ------------------------------------------------------------------------ */
typedef struct { const u16 *p; u32 n, i; int depth; } J;

static void ws(J *j) { while (j->i < j->n && (j->p[j->i] == ' ' || j->p[j->i] == '\t' || j->p[j->i] == '\n' || j->p[j->i] == '\r')) j->i++; }

static Val jfail(Vm *vm, J *j)
{
    return throw_err(vm, E_SYNTAX, "JSON.parse: not valid JSON at character %d", (int)j->i);
}

static Str *jstring(Vm *vm, J *j)
{
    u32 start = ++j->i, n = 0, k;
    Str *s;
    while (j->i < j->n && j->p[j->i] != '"') { if (j->p[j->i] == '\\') j->i++; j->i++; n++; }
    if (j->i >= j->n) return 0;
    s = str_new(n);
    if (!s) { vm->out_of_room = 1; return 0; }
    n = 0;
    for (k = start; k < j->i; k++) {
        u16 c = j->p[k];
        if (c == '\\') {
            c = j->p[++k];
            switch (c) {
            case 'n': c = '\n'; break; case 't': c = '\t'; break; case 'r': c = '\r'; break;
            case 'b': c = '\b'; break; case 'f': c = '\f'; break;
            case 'u': {
                u32 v = 0, q;
                for (q = 0; q < 4 && k + 1 < j->i; q++) {
                    u32 h = j->p[++k];
                    v = v * 16 + (h >= '0' && h <= '9' ? h - '0' : ((h | 32) - 'a' + 10) & 15);
                }
                c = (u16)v;
                break;
            }
            }
        } else if (c < 0x20) return 0;
        s->c[n++] = c;
    }
    s->len = n;
    j->i++;
    return s;
}

static Val jvalue(Vm *vm, J *j)
{
    u16 c;
    ws(j);
    if (j->i >= j->n) return jfail(vm, j);
    if (++j->depth > 200) return throw_err(vm, E_RANGE, "JSON nested too deep");
    c = j->p[j->i];
    if (c == '{') {
        Obj *o = obj_new(vm, vm->object_proto, C_OBJECT);
        if (!o) return v_undef();
        TEMP(vm, v_obj(o));
        j->i++; ws(j);
        if (j->i < j->n && j->p[j->i] == '}') { j->i++; UNTEMP(vm, 1); j->depth--; return v_obj(o); }
        for (;;) {
            Str *k;
            Val v;
            ws(j);
            if (j->i >= j->n || j->p[j->i] != '"') { UNTEMP(vm, 1); return jfail(vm, j); }
            k = jstring(vm, j);
            if (!k) { UNTEMP(vm, 1); return jfail(vm, j); }
            k = str_intern(k);
            TEMP(vm, v_str(k));
            ws(j);
            if (j->i >= j->n || j->p[j->i] != ':') { UNTEMP(vm, 2); return jfail(vm, j); }
            j->i++;
            v = jvalue(vm, j);
            UNTEMP(vm, 1);
            if (vm->throwing) { UNTEMP(vm, 1); return v_undef(); }
            if (o->cls == C_OBJECT) prop_put(vm, o, k, v, 0);
            ws(j);
            if (j->i < j->n && j->p[j->i] == ',') { j->i++; continue; }
            if (j->i < j->n && j->p[j->i] == '}') { j->i++; break; }
            UNTEMP(vm, 1);
            return jfail(vm, j);
        }
        UNTEMP(vm, 1);
        j->depth--;
        return v_obj(o);
    }
    if (c == '[') {
        Obj *a = arr_new(vm, 0);
        if (!a) return v_undef();
        TEMP(vm, v_obj(a));
        j->i++; ws(j);
        if (j->i < j->n && j->p[j->i] == ']') { j->i++; UNTEMP(vm, 1); j->depth--; return v_obj(a); }
        for (;;) {
            Val v = jvalue(vm, j);
            if (vm->throwing) { UNTEMP(vm, 1); return v_undef(); }
            arr_push(vm, a, v);
            ws(j);
            if (j->i < j->n && j->p[j->i] == ',') { j->i++; continue; }
            if (j->i < j->n && j->p[j->i] == ']') { j->i++; break; }
            UNTEMP(vm, 1);
            return jfail(vm, j);
        }
        UNTEMP(vm, 1);
        j->depth--;
        return v_obj(a);
    }
    j->depth--;
    if (c == '"') { Str *s = jstring(vm, j); return s ? v_str(s) : jfail(vm, j); }
    if (c == 't' && j->i + 4 <= j->n && j->p[j->i + 1] == 'r' && j->p[j->i + 2] == 'u' && j->p[j->i + 3] == 'e') { j->i += 4; return v_bool(1); }
    if (c == 'f' && j->i + 5 <= j->n && j->p[j->i + 1] == 'a' && j->p[j->i + 2] == 'l' && j->p[j->i + 3] == 's' && j->p[j->i + 4] == 'e') { j->i += 5; return v_bool(0); }
    if (c == 'n' && j->i + 4 <= j->n && j->p[j->i + 1] == 'u' && j->p[j->i + 2] == 'l' && j->p[j->i + 3] == 'l') { j->i += 4; return v_null(); }
    if (c == '-' || (c >= '0' && c <= '9')) {
        u32 used;
        f64 v = num_parse_prefix(j->p + j->i, j->n - j->i, &used);
        if (!used) return jfail(vm, j);
        j->i += used;
        return v_num(v);
    }
    return jfail(vm, j);
}

static Val revive(Vm *vm, Val holder, Str *key, Val fn, int depth)
{
    Val v = get_prop(vm, holder, key), a[2];
    if (vm->throwing || depth > 200) return v_undef();
    if (v.t == T_OBJ) {
        Obj *keys = own_keys(vm, v.u.o, 0);
        u32 i;
        if (!keys) return v_undef();
        TEMP(vm, v); TEMP(vm, v_obj(keys));
        for (i = 0; i < keys->u.arr.len && !vm->throwing; i++) {
            Str *k = to_key(vm, arr_get(keys, i));
            Val nv = revive(vm, v, k, fn, depth + 1);
            if (nv.t == T_UNDEF) del_prop(vm, v, v_str(k)); else set_prop(vm, v, k, nv);
        }
        UNTEMP(vm, 2);
    }
    a[0] = v_str(key); a[1] = v;
    return vm_call(vm, fn, holder, a, 2);
}

NATIVE(json_parse)
{
    Str *s = to_str(vm, ARG(0));
    J j;
    Val v;
    UNUSED;
    j.p = s->c; j.n = s->len; j.i = 0; j.depth = 0;
    TEMP(vm, v_str(s));
    v = jvalue(vm, &j);
    UNTEMP(vm, 1);
    if (vm->throwing) return v_undef();
    ws(&j);
    if (j.i != j.n) return jfail(vm, &j);
    if (is_callable(ARG(1))) {
        Obj *root = obj_new(vm, vm->object_proto, C_OBJECT);
        if (!root) return v_undef();
        TEMP(vm, v_obj(root));
        prop_put(vm, root, S_empty, v, 0);
        v = revive(vm, v_obj(root), S_empty, ARG(1), 0);
        UNTEMP(vm, 1);
    }
    return v;
}

/* ---- writing ------------------------------------------------------------------------- */
typedef struct { Obj *out; Val fn; Obj *keys; Str *gap; Obj *stack; } W;

static int put(Vm *vm, W *w, Str *s) { return arr_push(vm, w->out, v_str(s)); }

static Str *quote(Vm *vm, Str *s)
{
    u32 i, n = 2;
    Str *r;
    for (i = 0; i < s->len; i++) {
        u16 c = s->c[i];
        n += (c == '"' || c == '\\' || c == '\n' || c == '\r' || c == '\t' || c == '\b' || c == '\f') ? 2 : c < 0x20 ? 6 : 1;
    }
    r = str_new(n);
    if (!r) { vm->out_of_room = 1; return S_empty; }
    n = 0;
    r->c[n++] = '"';
    for (i = 0; i < s->len; i++) {
        u16 c = s->c[i];
        switch (c) {
        case '"': r->c[n++] = '\\'; r->c[n++] = '"'; break;
        case '\\': r->c[n++] = '\\'; r->c[n++] = '\\'; break;
        case '\n': r->c[n++] = '\\'; r->c[n++] = 'n'; break;
        case '\r': r->c[n++] = '\\'; r->c[n++] = 'r'; break;
        case '\t': r->c[n++] = '\\'; r->c[n++] = 't'; break;
        case '\b': r->c[n++] = '\\'; r->c[n++] = 'b'; break;
        case '\f': r->c[n++] = '\\'; r->c[n++] = 'f'; break;
        default:
            if (c < 0x20) {
                static const char hx[] = "0123456789abcdef";
                r->c[n++] = '\\'; r->c[n++] = 'u'; r->c[n++] = '0'; r->c[n++] = '0';
                r->c[n++] = (u16)hx[c >> 4]; r->c[n++] = (u16)hx[c & 15];
            } else r->c[n++] = c;
        }
    }
    r->c[n++] = '"';
    return r;
}

static void newline(Vm *vm, W *w, int depth)
{
    int i;
    if (!w->gap || !w->gap->len) return;
    put(vm, w, atom("\n"));
    for (i = 0; i < depth; i++) put(vm, w, w->gap);
}

/* 1 if something was written, 0 if the value is left out (undefined, functions) */
static int str1(Vm *vm, W *w, Val holder, Str *key, Val v, int depth)
{
    u32 i;
    if (depth > 200) { throw_err(vm, E_RANGE, "JSON.stringify: nested too deep"); return 0; }
    if (v.t == T_OBJ) {
        Val tj = get_prop(vm, v, S_toJSON);
        if (vm->throwing) return 0;
        if (is_callable(tj)) { Val a = v_str(key); v = vm_call(vm, tj, v, &a, 1); if (vm->throwing) return 0; }
    }
    if (is_callable(w->fn)) {
        Val a[2]; a[0] = v_str(key); a[1] = v;
        v = vm_call(vm, w->fn, holder, a, 2);
        if (vm->throwing) return 0;
    }
    if (v.t == T_OBJ && v.u.o->cls == C_BOXED) v = v.u.o->u.boxed.v;
    switch (v.t) {
    case T_NULL: put(vm, w, S_null); return 1;
    case T_BOOL: put(vm, w, v.u.b ? S_true : S_false); return 1;
    case T_NUM: put(vm, w, m_isnan(v.u.n) || m_isinf(v.u.n) ? S_null : num_to_str(v.u.n, 10)); return 1;
    case T_STR: put(vm, w, quote(vm, v.u.s)); return 1;
    case T_UNDEF: return 0;
    }
    if (is_callable(v)) return 0;
    /* a loop: refuse it */
    for (i = 0; i < w->stack->u.arr.len; i++) if (arr_get(w->stack, i).u.o == v.u.o) { throw_err(vm, E_TYPE, "JSON.stringify: an object that contains itself"); return 0; }
    arr_push(vm, w->stack, v);
    if (v.u.o->cls == C_ARRAY) {
        u32 n = v.u.o->u.arr.len;
        put(vm, w, atom("["));
        for (i = 0; i < n && !vm->throwing; i++) {
            if (i) put(vm, w, atom(","));
            newline(vm, w, depth + 1);
            if (!str1(vm, w, v, num_to_str(i, 10), arr_get(v.u.o, i), depth + 1)) put(vm, w, S_null);
        }
        if (n) newline(vm, w, depth);
        put(vm, w, atom("]"));
    } else {
        Obj *keys = w->keys ? w->keys : own_keys(vm, v.u.o, 0);
        int any = 0;
        if (!keys) return 0;
        TEMP(vm, v_obj(keys));
        put(vm, w, atom("{"));
        for (i = 0; i < keys->u.arr.len && !vm->throwing; i++) {
            Str *k = to_key(vm, arr_get(keys, i));
            Val pv = get_prop(vm, v, k);
            u32 mark = w->out->u.arr.len;
            if (vm->throwing) break;
            if (any) put(vm, w, atom(","));
            newline(vm, w, depth + 1);
            put(vm, w, quote(vm, k));
            put(vm, w, atom(w->gap && w->gap->len ? ": " : ":"));
            if (!str1(vm, w, v, k, pv, depth + 1)) { w->out->u.arr.len = mark; continue; }
            any = 1;
        }
        if (any) newline(vm, w, depth);
        put(vm, w, atom("}"));
        UNTEMP(vm, 1);
    }
    w->stack->u.arr.len--;
    return 1;
}

Str *json_stringify(Vm *vm, Val v)
{
    W w;
    Str *r;
    memset(&w, 0, sizeof w);
    w.out = arr_new(vm, 0);
    w.stack = arr_new(vm, 0);
    if (!w.out || !w.stack) return 0;
    TEMP(vm, v_obj(w.out)); TEMP(vm, v_obj(w.stack));
    if (!str1(vm, &w, v_undef(), S_empty, v, 0)) { UNTEMP(vm, 2); return 0; }
    r = array_join(vm, w.out, S_empty, 0);
    UNTEMP(vm, 2);
    return r;
}

NATIVE(json_stringify_n)
{
    W w;
    Val sp = ARG(2), rep = ARG(1);
    Str *r;
    UNUSED;
    memset(&w, 0, sizeof w);
    w.out = arr_new(vm, 0);
    if (!w.out) return v_undef();
    TEMP(vm, v_obj(w.out));
    w.stack = arr_new(vm, 0);
    if (!w.stack) { UNTEMP(vm, 1); return v_undef(); }
    TEMP(vm, v_obj(w.stack));
    if (is_callable(rep)) w.fn = rep;
    else if (rep.t == T_OBJ && rep.u.o->cls == C_ARRAY) w.keys = rep.u.o;
    if (sp.t == T_NUM) {
        int n = sp.u.n > 10 ? 10 : sp.u.n < 0 ? 0 : (int)sp.u.n, i;
        char b[11];
        for (i = 0; i < n; i++) b[i] = ' ';
        b[n] = 0;
        w.gap = str_ascii(b);
    } else if (sp.t == T_STR) w.gap = str_slice(sp.u.s, 0, 10);
    if (w.gap) TEMP(vm, v_str(w.gap)); else TEMP(vm, v_undef());
    if (!str1(vm, &w, v_undef(), S_empty, ARG(0), 0)) { UNTEMP(vm, 3); return v_undef(); }
    r = array_join(vm, w.out, S_empty, 0);
    UNTEMP(vm, 3);
    return v_str(r);
}

void json_init(Vm *vm)
{
    Obj *o = obj_new(vm, vm->object_proto, C_OBJECT);
    def_val(vm, vm->global, "JSON", v_obj(o));
    def_native(vm, o, "parse", json_parse, 2, 0);
    def_native(vm, o, "stringify", json_stringify_n, 3, 0);
}
