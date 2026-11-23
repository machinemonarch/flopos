/* ===========================================================================
 * dom.c -- the page, as scripts in the room see it.
 *
 * The page itself is in browzer's memory, not here.  A script holds
 * elements as numbers; everything it does with one -- read its text, set
 * its class, add a child -- is a request through the door (door.h), and
 * browzer checks it and does it.  No pointer ever leaves the room.
 *
 * Events are the room's own: listeners are kept here, and when browzer
 * says "element 23 was clicked", the room calls them, with the element's
 * ancestors after it, as a browser does.
 *
 * What is quicker written in JavaScript (fetch's promises,
 * XMLHttpRequest, location's parts) is: a short prelude, run as each page
 * starts.
 * ===========================================================================
 */
#include "js.h"
#include "engine.h"
#include "room.h"
#include "door.h"

#define ARG(i) ((i) < argc ? args[i] : v_undef())
#define NATIVE(name) static Val name(Vm *vm, Val self, Val *args, int argc, u32 magic)
#define UNUSED (void)self; (void)args; (void)argc; (void)magic

u32 g_door_limit = 20000;                /* knocks allowed in this task     */
u8  g_answer_flags;                      /* for the task's answer: 1 default prevented */

/* ---- the door ----------------------------------------------------------------- */
static u8  s_req[32768];
static u8  s_rep[65536];
static u32 s_rn, s_rl, s_rp;
static int s_rok;

static void rq(int op) { s_rn = 0; s_rok = 1; s_req[s_rn++] = (u8)op; }
static void rq_u32(u32 v)
{
    if (s_rn + 4 > sizeof s_req) { s_rok = 0; return; }
    s_req[s_rn++] = (u8)v; s_req[s_rn++] = (u8)(v >> 8); s_req[s_rn++] = (u8)(v >> 16); s_req[s_rn++] = (u8)(v >> 24);
}
static void rq_bytes(const char *p, u32 n)
{
    u32 i;
    rq_u32(n);
    if (s_rn + n > sizeof s_req) { s_rok = 0; return; }
    for (i = 0; i < n; i++) s_req[s_rn++] = (u8)p[i];
}
static void rq_cstr(const char *s) { rq_bytes(s, (u32)js_strlen(s)); }
static void rq_str(Vm *vm, Val v)
{
    Str *s = v.t == T_STR ? v.u.s : to_str(vm, v);
    u32 at = s_rn + 4, n;
    if (at >= sizeof s_req) { s_rok = 0; return; }
    n = str_to_utf8(s, (char *)s_req + at, sizeof s_req - at);
    rq_u32(n);
    s_rn += n;
}

static void stop(Vm *vm, const char *why)
{
    vm->fatal = 1;
    vm->stop_why = why;
    throw_val(vm, v_str(S_empty));
}

/* Knock; 1 if browzer answered.  A refusal stops the script. */
static int knock(Vm *vm)
{
    u32 t0;
    int n;
    static char why[200];
    if (vm->throwing) return 0;
    if (!s_rok) { throw_err(vm, E_RANGE, "too much to hand through the door"); return 0; }
    if (++vm->door_count > g_door_limit) { stop(vm, "it knocked on the door too often"); return 0; }
    t0 = host_ms();
    n = host_door(s_req, s_rn, s_rep, sizeof s_rep);
    vm->door_ms += host_ms() - t0;
    if (n < 1) { stop(vm, "browzer did not answer"); return 0; }
    s_rl = (u32)n; s_rp = 1;
    if (s_rep[0] == DK_ERR) {
        u32 len = n >= 5 ? (u32)s_rep[1] | ((u32)s_rep[2] << 8) | ((u32)s_rep[3] << 16) | ((u32)s_rep[4] << 24) : 0, k;
        for (k = 0; k < len && k < sizeof why - 1 && 5 + k < (u32)n; k++) why[k] = (char)s_rep[5 + k];
        why[k] = 0;
        stop(vm, why[0] ? why : "browzer refused");
        return 0;
    }
    return 1;
}

static u32 rp_u32(void)
{
    u32 v;
    if (s_rp + 4 > s_rl) return 0;
    v = (u32)s_rep[s_rp] | ((u32)s_rep[s_rp + 1] << 8) | ((u32)s_rep[s_rp + 2] << 16) | ((u32)s_rep[s_rp + 3] << 24);
    s_rp += 4;
    return v;
}

static Str *rp_str(void)
{
    u32 n = rp_u32();
    Str *s;
    if (s_rp + n > s_rl) n = s_rl - s_rp;
    s = str_utf8((const char *)s_rep + s_rp, n);
    s_rp += n;
    return s ? s : S_empty;
}

/* ---- elements as objects --------------------------------------------------------- */

static Val wrap(Vm *vm, u32 node)
{
    Val w;
    Obj *o;
    if (!node) return v_null();
    if (node == 1) return v_obj(vm->document_obj);
    if (!vm->hosts) { vm->hosts = arr_new(vm, 0); if (!vm->hosts) return v_null(); }
    if (node > 20000) return v_null();
    w = arr_get(vm->hosts, node);
    if (w.t == T_OBJ) return w;
    o = obj_new(vm, vm->element_proto, C_HOST);
    if (!o) return v_null();
    o->u.host.node = node;
    arr_set(vm, vm->hosts, node, v_obj(o));
    return v_obj(o);
}

static u32 node_of(Val v)
{
    if (v.t == T_OBJ && (v.u.o->cls == C_HOST || v.u.o->cls == C_STYLE || v.u.o->cls == C_CLASSLIST)) return v.u.o->u.host.node;
    return 0;
}

/* The answer as a value: text, a number, an element, a list of them. */
static Val rp_val(Vm *vm)
{
    u8 k = s_rep[0];
    switch (k) {
    case DK_NULL: return v_null();
    case DK_BOOL: return v_bool(s_rl > 1 && s_rep[1]);
    case DK_NUM: { s32 v = (s32)rp_u32(); return v_num(v); }
    case DK_STR: return v_str(rp_str());
    case DK_NODE: return wrap(vm, rp_u32());
    case DK_LIST: {
        u32 n = rp_u32(), i;
        Obj *a = arr_new(vm, 0);
        if (!a) return v_undef();
        TEMP(vm, v_obj(a));
        for (i = 0; i < n && !vm->throwing; i++) arr_push(vm, a, wrap(vm, rp_u32()));
        UNTEMP(vm, 1);
        return v_obj(a);
    }
    }
    return v_undef();
}

static u32 this_node(Vm *vm, Val self)
{
    u32 n = node_of(self);
    if (!n) throw_err(vm, E_TYPE, "an element's method on something that is not an element");
    return n;
}

/* ---- properties ------------------------------------------------------------------ */
NATIVE(el_get)
{
    u32 n = node_of(self);
    UNUSED;
    if (!n) return v_undef();
    rq(D_GET); rq_u32(n); rq_u32(magic);
    if (!knock(vm)) return v_undef();
    return rp_val(vm);
}

NATIVE(el_set)
{
    u32 n = node_of(self);
    Val v = ARG(0);
    UNUSED;
    if (!n) return v_undef();
    rq(D_SET); rq_u32(n); rq_u32(magic);
    if (v.t == T_BOOL) rq_cstr(v.u.b ? "1" : "");
    else if (IS_NULLISH(v) && magic != DP_VALUE) rq_cstr("");
    else rq_str(vm, v);
    knock(vm);
    return v_undef();
}

NATIVE(el_shown)                          /* offsetWidth and friends: 0 if hidden */
{
    u32 n = node_of(self);
    UNUSED;
    if (!n) return v_num(0);
    rq(D_SHOWN); rq_u32(n);
    if (!knock(vm)) return v_undef();
    if (magic == 9) return s_rep[1] ? wrap(vm, 0) : v_null();   /* offsetParent */
    return v_num(s_rl > 1 && s_rep[1] ? (magic & 1 ? 20 : 100) : 0);
}

NATIVE(el_document) { UNUSED; return v_obj(vm->document_obj); }

/* ---- methods ------------------------------------------------------------------------ */
NATIVE(el_attr)                            /* 0 get, 1 set, 2 remove, 3 has */
{
    u32 n = this_node(vm, self);
    UNUSED;
    if (!n) return v_undef();
    if (magic == 0 || magic == 3) {
        Val r;
        rq(D_ATTR); rq_u32(n); rq_str(vm, ARG(0));
        if (!knock(vm)) return v_undef();
        r = rp_val(vm);
        return magic == 3 ? v_bool(r.t != T_NULL) : r;
    }
    if (magic == 1) { rq(D_SETATTR); rq_u32(n); rq_str(vm, ARG(0)); rq_str(vm, ARG(1)); }
    else { rq(D_DELATTR); rq_u32(n); rq_str(vm, ARG(0)); }
    knock(vm);
    return v_undef();
}

static int insert(Vm *vm, u32 parent, Val child, u32 before)
{
    u32 c = node_of(child);
    if (!c) {
        /* text, not an element: a text node for it */
        rq(D_TEXT); rq_str(vm, child);
        if (!knock(vm)) return 0;
        c = rp_u32();
        if (!c) return 0;
    }
    rq(D_INSERT); rq_u32(parent); rq_u32(c); rq_u32(before);
    return knock(vm);
}

NATIVE(el_append)                          /* 0 appendChild, 1 append (many), 2 prepend */
{
    u32 n = this_node(vm, self), first = 0;
    int i;
    UNUSED;
    if (!n) return v_undef();
    if (magic == 0) { if (!node_of(ARG(0))) return throw_err(vm, E_TYPE, "appendChild needs an element"); insert(vm, n, ARG(0), 0); return ARG(0); }
    if (magic == 2) { rq(D_GET); rq_u32(n); rq_u32(DP_FIRST); if (knock(vm)) first = s_rep[0] == DK_NODE ? rp_u32() : 0; }
    for (i = 0; i < argc && !vm->throwing; i++) insert(vm, n, args[i], first);
    return v_undef();
}

NATIVE(el_insertbefore)
{
    u32 n = this_node(vm, self);
    UNUSED;
    if (!n) return v_undef();
    insert(vm, n, ARG(0), node_of(ARG(1)));
    return ARG(0);
}

NATIVE(el_removechild)                     /* 0 removeChild(x), 1 remove() */
{
    u32 n = magic ? this_node(vm, self) : node_of(ARG(0));
    UNUSED;
    if (!n) return v_undef();
    rq(D_REMOVE); rq_u32(n);
    knock(vm);
    return magic ? v_undef() : ARG(0);
}

NATIVE(el_replacechild)
{
    u32 n = this_node(vm, self), old = node_of(ARG(1));
    UNUSED;
    if (!n || !old) return v_undef();
    insert(vm, n, ARG(0), old);
    rq(D_REMOVE); rq_u32(old);
    knock(vm);
    return ARG(1);
}

NATIVE(el_beforeafter)                     /* 0 before, 1 after, 2 replaceWith */
{
    u32 n = this_node(vm, self), parent, next = 0;
    int i;
    UNUSED;
    if (!n) return v_undef();
    rq(D_GET); rq_u32(n); rq_u32(DP_PARENT);
    if (!knock(vm) || s_rep[0] != DK_NODE) return v_undef();
    parent = rp_u32();
    if (magic >= 1) { rq(D_GET); rq_u32(n); rq_u32(DP_NEXT); if (knock(vm) && s_rep[0] == DK_NODE) next = rp_u32(); }
    for (i = 0; i < argc && !vm->throwing; i++) insert(vm, parent, args[i], magic == 0 ? n : next);
    if (magic == 2) { rq(D_REMOVE); rq_u32(n); knock(vm); }
    return v_undef();
}

NATIVE(el_query)                           /* 0 querySelector, 1 querySelectorAll */
{
    u32 n = node_of(self);
    UNUSED;
    if (self.t == T_OBJ && self.u.o == vm->document_obj) n = 1;
    if (!n) return throw_err(vm, E_TYPE, "querySelector on something that is not an element");
    rq(D_QUERY); rq_u32(n); rq_str(vm, ARG(0)); rq_u32(magic);
    if (!knock(vm)) return v_undef();
    if (s_rep[0] == DK_UNDEF) return throw_err(vm, E_SYNTAX, "a selector browzer cannot read");
    return rp_val(vm);
}

NATIVE(el_bytag)                           /* 0 tag, 1 class */
{
    u32 n = node_of(self);
    UNUSED;
    if (self.t == T_OBJ && self.u.o == vm->document_obj) n = 1;
    if (!n) return v_undef();
    rq(magic ? D_CLASSES : D_TAGS); rq_u32(n); rq_str(vm, ARG(0));
    if (!knock(vm)) return v_undef();
    return rp_val(vm);
}

static int in_path(Vm *vm, u32 n, u32 ancestor)
{
    u32 k, cnt;
    rq(D_PATH); rq_u32(n);
    if (!knock(vm)) return 0;
    cnt = rp_u32();
    for (k = 0; k < cnt; k++) if (rp_u32() == ancestor) return 1;
    return 0;
}

NATIVE(el_contains)
{
    u32 n = node_of(self), other = node_of(ARG(0));
    UNUSED;
    if (self.t == T_OBJ && self.u.o == vm->document_obj) n = 1;
    if (!n || !other) return v_bool(0);
    return v_bool(in_path(vm, other, n));
}

/* matches(sel) and closest(sel): the selector over the whole page, then
 * which of the matches is this one, or its nearest ancestor. */
NATIVE(el_matches)                         /* 0 matches, 1 closest */
{
    u32 n = this_node(vm, self), path[256], np = 0, k, cnt, i;
    static u32 hits[4096];
    u32 nh;
    UNUSED;
    if (!n) return v_undef();
    rq(D_PATH); rq_u32(n);
    if (!knock(vm)) return v_undef();
    cnt = rp_u32();
    for (k = 0; k < cnt && np < 256; k++) path[np++] = rp_u32();
    rq(D_QUERY); rq_u32(1); rq_str(vm, ARG(0)); rq_u32(1);
    if (!knock(vm)) return v_undef();
    if (s_rep[0] == DK_UNDEF) return throw_err(vm, E_SYNTAX, "a selector browzer cannot read");
    nh = rp_u32();
    for (i = 0; i < nh && i < 4096; i++) hits[i] = rp_u32();
    for (k = 0; k < (magic ? np : 1); k++)
        for (i = 0; i < nh && i < 4096; i++) if (hits[i] == path[k]) return magic ? wrap(vm, path[k]) : v_bool(1);
    return magic ? v_null() : v_bool(0);
}

NATIVE(el_clone)
{
    u32 n = this_node(vm, self);
    UNUSED;
    if (!n) return v_undef();
    rq(D_CLONE); rq_u32(n); rq_u32(truthy(ARG(0)) ? 1u : 0u);
    if (!knock(vm)) return v_undef();
    return wrap(vm, rp_u32());
}

NATIVE(el_insertadjacent)                 /* insertAdjacentHTML / Element / Text */
{
    u32 n = this_node(vm, self), tmp, parent = 0, next = 0, first = 0;
    Str *where = to_str(vm, ARG(0));
    int pos = str_eq_ascii(where, "beforebegin") ? 0 : str_eq_ascii(where, "afterbegin") ? 1 :
              str_eq_ascii(where, "beforeend") ? 2 : str_eq_ascii(where, "afterend") ? 3 : -1;
    UNUSED;
    if (!n) return v_undef();
    if (pos < 0) return throw_err(vm, E_SYNTAX, "insertAdjacent: a place browzer does not know");
    if (magic) {                              /* an element or text */
        Val x = ARG(1);
        if (pos == 1 || pos == 2) {
            if (pos == 1) { rq(D_GET); rq_u32(n); rq_u32(DP_FIRST); if (knock(vm) && s_rep[0] == DK_NODE) first = rp_u32(); }
            insert(vm, n, x, pos == 1 ? first : 0);
        } else {
            rq(D_GET); rq_u32(n); rq_u32(DP_PARENT);
            if (!knock(vm) || s_rep[0] != DK_NODE) return v_undef();
            parent = rp_u32();
            if (pos == 3) { rq(D_GET); rq_u32(n); rq_u32(DP_NEXT); if (knock(vm) && s_rep[0] == DK_NODE) next = rp_u32(); }
            insert(vm, parent, x, pos == 0 ? n : next);
        }
        return x;
    }
    /* HTML: made in a spare element, then its children moved in place */
    rq(D_CREATE); rq_cstr("fl-frag");
    if (!knock(vm)) return v_undef();
    tmp = rp_u32();
    rq(D_SET); rq_u32(tmp); rq_u32(DP_HTML); rq_str(vm, ARG(1));
    if (!knock(vm)) return v_undef();
    if (pos == 1 || pos == 2) {
        if (pos == 1) { rq(D_GET); rq_u32(n); rq_u32(DP_FIRST); if (knock(vm) && s_rep[0] == DK_NODE) first = rp_u32(); }
        rq(D_INSERT); rq_u32(n); rq_u32(tmp); rq_u32(pos == 1 ? first : 0);
    } else {
        rq(D_GET); rq_u32(n); rq_u32(DP_PARENT);
        if (!knock(vm) || s_rep[0] != DK_NODE) return v_undef();
        parent = rp_u32();
        if (pos == 3) { rq(D_GET); rq_u32(n); rq_u32(DP_NEXT); if (knock(vm) && s_rep[0] == DK_NODE) next = rp_u32(); }
        rq(D_INSERT); rq_u32(parent); rq_u32(tmp); rq_u32(pos == 0 ? n : next);
    }
    knock(vm);
    return v_undef();
}

NATIVE(el_nothing) { UNUSED; return v_undef(); }

NATIVE(el_rect)
{
    Obj *r = obj_new(vm, vm->object_proto, C_OBJECT);
    static const char *k[] = { "top", "left", "right", "bottom", "x", "y", "width", "height", 0 };
    static const int v[] = { 0, 0, 100, 20, 0, 0, 100, 20 };
    int i;
    UNUSED;
    if (!r) return v_undef();
    for (i = 0; k[i]; i++) prop_put(vm, r, atom(k[i]), v_num(v[i]), 0);
    return v_obj(r);
}

/* el.style and el.classList: objects of their own that ask the door */
static Obj *sub_object(Vm *vm, Val self, int cls, Obj *proto, const char *key)
{
    Str *k = atom(key);
    Prop *q;
    Obj *o;
    if (self.t != T_OBJ) return 0;
    q = prop_find(self.u.o, k);
    if (q && q->v.t == T_OBJ) return q->v.u.o;
    o = obj_new(vm, proto, cls);
    if (!o) return 0;
    o->u.host.node = node_of(self);
    prop_put(vm, self.u.o, k, v_obj(o), P_HIDDEN);
    return o;
}

NATIVE(el_style) { Obj *o = sub_object(vm, self, C_STYLE, vm->style_proto, "%style"); UNUSED; return o ? v_obj(o) : v_undef(); }
NATIVE(el_classlist) { Obj *o = sub_object(vm, self, C_CLASSLIST, vm->classlist_proto, "%classes"); UNUSED; return o ? v_obj(o) : v_undef(); }

NATIVE(el_dataset)
{
    /* data-x-y as dataset.xY, read once; writes go back as attributes */
    UNUSED;
    return v_obj(obj_new(vm, vm->object_proto, C_OBJECT));
}

/* el.style.display = 'none' -> the style attribute, through the door. */
static void camel_to_kebab(Str *k, char *out, u32 cap)
{
    u32 i, n = 0;
    for (i = 0; i < k->len && n + 2 < cap; i++) {
        u16 c = k->c[i];
        if (c >= 'A' && c <= 'Z') { out[n++] = '-'; out[n++] = (char)(c + 32); }
        else out[n++] = (char)c;
    }
    out[n] = 0;
}

Val dom_get_hook(Vm *vm, Obj *o, Str *k, int *done)
{
    char name[64];
    *done = 0;
    if (o->cls == C_STYLE) {
        if (prop_find(vm->style_proto, k)) return v_undef();   /* a method */
        camel_to_kebab(k, name, sizeof name);
        rq(D_STYLE); rq_u32(o->u.host.node); rq_cstr(name);
        *done = 1;
        if (!knock(vm)) return v_undef();
        return rp_val(vm);
    }
    if (o->cls == C_CLASSLIST && k == S_length) {
        *done = 1;
        return v_num(0);
    }
    return v_undef();
}

int dom_set_hook(Vm *vm, Obj *o, Str *k, Val v)
{
    char name[64];
    if (o->cls != C_STYLE) return 1;
    camel_to_kebab(k, name, sizeof name);
    rq(D_SETSTYLE); rq_u32(o->u.host.node); rq_cstr(str_eq_ascii(k, "cssText") ? "css-text" : name); rq_str(vm, v);
    return knock(vm);
}

NATIVE(style_setprop)                      /* setProperty / removeProperty / getPropertyValue */
{
    UNUSED;
    if (self.t != T_OBJ || self.u.o->cls != C_STYLE) return v_undef();
    if (magic == 2) { rq(D_STYLE); rq_u32(self.u.o->u.host.node); rq_str(vm, ARG(0)); if (!knock(vm)) return v_undef(); return rp_val(vm); }
    rq(D_SETSTYLE); rq_u32(self.u.o->u.host.node); rq_str(vm, ARG(0));
    if (magic == 0) rq_str(vm, ARG(1)); else rq_cstr("");
    knock(vm);
    return v_undef();
}

NATIVE(classlist_op)                       /* 0 add 1 remove 2 toggle 3 contains */
{
    int i;
    UNUSED;
    if (self.t != T_OBJ || self.u.o->cls != C_CLASSLIST) return v_undef();
    if (magic == 2 && argc > 1) {            /* toggle(name, force) */
        rq(D_CLASS); rq_u32(self.u.o->u.host.node); rq_u32(truthy(args[1]) ? 0 : 1); rq_str(vm, args[0]);
        knock(vm);
        return v_bool(truthy(args[1]));
    }
    for (i = 0; i < (magic >= 2 ? 1 : argc) && !vm->throwing; i++) {
        rq(D_CLASS); rq_u32(self.u.o->u.host.node); rq_u32(magic); rq_str(vm, args[i]);
        if (!knock(vm)) return v_undef();
        if (magic >= 2) return v_bool(s_rl > 1 && s_rep[1]);
    }
    return v_undef();
}

NATIVE(classlist_replace)
{
    UNUSED;
    if (self.t != T_OBJ || self.u.o->cls != C_CLASSLIST) return v_undef();
    rq(D_CLASS); rq_u32(self.u.o->u.host.node); rq_u32(3); rq_str(vm, ARG(0));
    if (!knock(vm) || !(s_rl > 1 && s_rep[1])) return v_bool(0);
    rq(D_CLASS); rq_u32(self.u.o->u.host.node); rq_u32(1); rq_str(vm, ARG(0)); knock(vm);
    rq(D_CLASS); rq_u32(self.u.o->u.host.node); rq_u32(0); rq_str(vm, ARG(1)); knock(vm);
    return v_bool(1);
}

/* ---- events -------------------------------------------------------------------------- */
static Str *K_ev, *K_type, *K_target, *K_current, *K_prevented, *K_stopped;

static Obj *listeners(Vm *vm, Val target, Str *type, int make)
{
    Prop *q;
    Obj *map, *list;
    Val lv;
    if (target.t != T_OBJ) return 0;
    q = prop_find(target.u.o, K_ev);
    if (!q || q->v.t != T_OBJ) {
        if (!make) return 0;
        map = obj_new(vm, 0, C_OBJECT);
        if (!map) return 0;
        prop_put(vm, target.u.o, K_ev, v_obj(map), P_HIDDEN);
    } else map = q->v.u.o;
    {
        Prop *l = prop_find(map, str_intern(type));
        lv = l ? l->v : v_undef();
    }
    if (lv.t == T_OBJ) return lv.u.o;
    if (!make) return 0;
    TEMP(vm, v_obj(map));
    list = arr_new(vm, 0);
    if (list) prop_put(vm, map, str_intern(type), v_obj(list), 0);
    UNTEMP(vm, 1);
    return list;
}

static void tell_browzer(Vm *vm, u32 node, Str *type)
{
    if (!node) return;
    rq(D_LISTEN); rq_u32(node); rq_str(vm, v_str(type));
    knock(vm);
}

NATIVE(ev_add)
{
    Str *type;
    Obj *list;
    u32 i;
    UNUSED;
    if (!is_callable(ARG(1))) return v_undef();
    type = to_str(vm, ARG(0));
    TEMP(vm, v_str(type));
    list = listeners(vm, self, type, 1);
    UNTEMP(vm, 1);
    if (!list) return v_undef();
    for (i = 0; i < list->u.arr.len; i++) if (strict_eq(arr_get(list, i), ARG(1))) return v_undef();
    arr_push(vm, list, ARG(1));
    if (self.t == T_OBJ && self.u.o->cls == C_HOST) tell_browzer(vm, self.u.o->u.host.node, type);
    return v_undef();
}

NATIVE(ev_remove)
{
    Obj *list;
    u32 i;
    UNUSED;
    list = listeners(vm, self, to_str(vm, ARG(0)), 0);
    if (!list) return v_undef();
    for (i = 0; i < list->u.arr.len; i++)
        if (strict_eq(arr_get(list, i), ARG(1))) {
            memmove(&list->u.arr.items->v[i], &list->u.arr.items->v[i + 1], (list->u.arr.len - i - 1) * sizeof(Val));
            list->u.arr.len--;
            break;
        }
    return v_undef();
}

/* el.onclick = fn: kept, and browzer told, so the element can be picked */
NATIVE(on_get)
{
    char k[24];
    Prop *q;
    UNUSED;
    if (self.t != T_OBJ) return v_undef();
    k[0] = '%';
    {   u32 i; Str *n = vm->callee->u.nat.name; for (i = 0; i < n->len && i < 22; i++) k[i + 1] = (char)n->c[i]; k[i + 1] = 0; }
    q = prop_find(self.u.o, atom(k));
    return q ? q->v : v_null();
}

NATIVE(on_set)
{
    char k[24];
    Str *n = vm->callee->u.nat.name;
    u32 i;
    UNUSED;
    if (self.t != T_OBJ) return v_undef();
    k[0] = '%';
    for (i = 0; i < n->len && i < 22; i++) k[i + 1] = (char)n->c[i];
    k[i + 1] = 0;
    prop_put(vm, self.u.o, atom(k), ARG(0), P_HIDDEN);
    if (self.u.o->cls == C_HOST && is_callable(ARG(0)) && n->len > 2) tell_browzer(vm, self.u.o->u.host.node, str_slice(n, 2, n->len));
    return v_undef();
}

NATIVE(ev_prevent)                         /* 0 preventDefault, 1 stopPropagation, 2 stopImmediatePropagation */
{
    UNUSED;
    if (self.t != T_OBJ) return v_undef();
    if (magic == 0) prop_put(vm, self.u.o, K_prevented, v_bool(1), 0);
    else prop_put(vm, self.u.o, K_stopped, v_num(magic), P_HIDDEN);
    return v_undef();
}

static Obj *new_event(Vm *vm, Str *type, Val target)
{
    Obj *e = obj_new(vm, vm->event_proto, C_OBJECT);
    if (!e) return 0;
    TEMP(vm, v_obj(e));
    prop_put(vm, e, K_type, v_str(type), 0);
    prop_put(vm, e, K_target, target, 0);
    prop_put(vm, e, atom("srcElement"), target, 0);
    prop_put(vm, e, K_current, target, 0);
    prop_put(vm, e, K_prevented, v_bool(0), 0);
    prop_put(vm, e, atom("bubbles"), v_bool(1), 0);
    prop_put(vm, e, atom("cancelable"), v_bool(1), 0);
    prop_put(vm, e, atom("isTrusted"), v_bool(1), 0);
    prop_put(vm, e, atom("timeStamp"), v_num(host_ms()), 0);
    UNTEMP(vm, 1);
    return e;
}

/* An inline handler, onclick="...", made into a function once. */
static Val inline_handler(Vm *vm, u32 node, Str *type)
{
    char key[32], why[160];
    Str *k;
    Prop *q;
    Val target = wrap(vm, node);
    u32 i;
    if (target.t != T_OBJ) return v_undef();
    key[0] = '%'; key[1] = 'i';
    for (i = 0; i < type->len && i < 28; i++) key[2 + i] = (char)type->c[i];
    key[2 + i] = 0;
    k = atom(key);
    q = prop_find(target.u.o, k);
    if (q) return q->v;
    {
        Val src;
        char attr[32];
        attr[0] = 'o'; attr[1] = 'n';
        for (i = 0; i < type->len && i < 28; i++) attr[2 + i] = (char)type->c[i];
        attr[2 + i] = 0;
        rq(D_ATTR); rq_u32(node); rq_cstr(attr);
        if (!knock(vm)) return v_undef();
        src = rp_val(vm);
        if (src.t == T_STR && src.u.s->len) {
            static char text[8192];
            u32 n = str_to_utf8(src.u.s, text, sizeof text);
            Proto *p = js_compile(vm, text, n, "inline", why, sizeof why, 1);
            if (p) {
                Obj *fn = obj_new(vm, vm->function_proto, C_FUNC);
                if (fn) {
                    fn->flags = OF_CALLABLE;
                    fn->u.fn.fn = p;
                    prop_put(vm, target.u.o, k, v_obj(fn), P_HIDDEN);
                    return v_obj(fn);
                }
            }
        }
        prop_put(vm, target.u.o, k, v_bool(0), P_HIDDEN);
    }
    return v_undef();
}

/* Call the listeners of one place (an element, the document, window). */
static void fire_at(Vm *vm, Val at, u32 node, Obj *ev, Str *type)
{
    Obj *list = listeners(vm, at, type, 0);
    Val h;
    char k[24];
    u32 i;
    if (at.t != T_OBJ) return;
    prop_put(vm, ev, K_current, at, 0);
    TEMP(vm, at);
    if (list) {
        Obj *copy = arr_new(vm, 0);           /* a listener may add or take away others */
        if (copy) {
            TEMP(vm, v_obj(copy));
            for (i = 0; i < list->u.arr.len; i++) arr_push(vm, copy, arr_get(list, i));
            for (i = 0; i < copy->u.arr.len && !vm->fatal; i++) {
                Val e = v_obj(ev);
                Prop *s;
                vm_call(vm, arr_get(copy, i), at, &e, 1);
                if (vm->throwing && !vm->fatal) vm->throwing = 0;
                s = prop_find(ev, K_stopped);
                if (s && s->v.t == T_NUM && s->v.u.n == 2) break;
            }
            UNTEMP(vm, 1);
        }
    }
    /* the onclick property, or else the onclick attribute */
    k[0] = '%'; k[1] = 'o'; k[2] = 'n';
    for (i = 0; i < type->len && i < 20; i++) k[3 + i] = (char)type->c[i];
    k[3 + i] = 0;
    {
        Prop *q = prop_find(at.u.o, atom(k));
        h = q ? q->v : v_undef();
    }
    if (!is_callable(h) && node) h = inline_handler(vm, node, type);
    if (is_callable(h) && !vm->fatal) {
        Val e = v_obj(ev), r;
        r = vm_call(vm, h, at, &e, 1);
        if (vm->throwing && !vm->fatal) vm->throwing = 0;
        if (r.t == T_BOOL && !r.u.b) prop_put(vm, ev, K_prevented, v_bool(1), 0);   /* return false */
    }
    UNTEMP(vm, 1);
}

/* An event along a path: the target, then its ancestors, then window. */
static int dispatch(Vm *vm, Obj *ev, Str *type, const u32 *path, u32 n, int to_window)
{
    u32 i;
    TEMP(vm, v_obj(ev));
    for (i = 0; i < n && !vm->fatal; i++) {
        Prop *s;
        fire_at(vm, wrap(vm, path[i]), path[i], ev, type);
        s = prop_find(ev, K_stopped);
        if (s && truthy(s->v)) break;
    }
    if (to_window && !vm->fatal) {
        Prop *s = prop_find(ev, K_stopped);
        if (!s || !truthy(s->v)) fire_at(vm, v_obj(vm->global), 0, ev, type);
    }
    UNTEMP(vm, 1);
    {
        Prop *p = prop_find(ev, K_prevented);
        return p && truthy(p->v);
    }
}

NATIVE(ev_dispatch)                        /* el.dispatchEvent(ev), el.click() */
{
    u32 n = node_of(self), path[256], np = 0, k, cnt;
    Obj *ev;
    Str *type;
    UNUSED;
    if (self.t == T_OBJ && self.u.o == vm->document_obj) n = 1;
    if (magic) {                              /* click() */
        ev = new_event(vm, atom("click"), self);
        type = atom("click");
    } else {
        if (ARG(0).t != T_OBJ) return throw_err(vm, E_TYPE, "dispatchEvent needs an event");
        ev = ARG(0).u.o;
        type = to_str(vm, get_prop(vm, ARG(0), K_type));
        prop_put(vm, ev, K_target, self, 0);
    }
    if (!ev) return v_undef();
    TEMP(vm, v_obj(ev));
    if (n) {
        rq(D_PATH); rq_u32(n);
        if (!knock(vm)) { UNTEMP(vm, 1); return v_undef(); }
        cnt = rp_u32();
        for (k = 0; k < cnt && np < 256; k++) path[np++] = rp_u32();
        dispatch(vm, ev, type, path, np, 1);
    } else fire_at(vm, self, 0, ev, type);
    UNTEMP(vm, 1);
    return v_bool(!truthy(get_prop(vm, v_obj(ev), K_prevented)));
}

/* From browzer: [T_CALL][ms][type][target][n][path...][key]: an event. */
static u32 rd32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }

int room_dispatch(Vm *vm, const u8 *t, u32 len, char *err, u32 errcap)
{
    u32 at = 5, ms = rd32(t + 1), tl, target, n, i, path[256], np = 0;
    Str *type, *key = 0;
    Obj *ev;
    int prevented;
    if (len < 13) return ENGINE_OK;
    tl = rd32(t + at); at += 4;
    if (at + tl + 8 > len) return ENGINE_OK;
    engine_begin(vm, ms);
    g_door_limit = 2000;
    type = str_intern(str_utf8((const char *)t + at, tl)); at += tl;
    target = rd32(t + at); at += 4;
    n = rd32(t + at); at += 4;
    for (i = 0; i < n && np < 256 && at + 4 <= len; i++, at += 4) path[np++] = rd32(t + at);
    if (at + 4 <= len) { u32 kl = rd32(t + at); at += 4; if (at + kl <= len && kl) key = str_utf8((const char *)t + at, kl); }
    ev = new_event(vm, type, target == 0 ? v_obj(vm->global) : wrap(vm, target));
    if (!ev) return ENGINE_ROOM;
    TEMP(vm, v_obj(ev));
    if (key) {
        prop_put(vm, ev, atom("key"), v_str(key), 0);
        prop_put(vm, ev, atom("code"), v_str(key), 0);
        if (key->len == 1) {
            prop_put(vm, ev, atom("keyCode"), v_num(key->c[0] >= 'a' && key->c[0] <= 'z' ? key->c[0] - 32 : key->c[0]), 0);
            prop_put(vm, ev, atom("which"), v_num(key->c[0]), 0);
        }
        if (str_eq_ascii(key, "Enter")) { prop_put(vm, ev, atom("keyCode"), v_num(13), 0); prop_put(vm, ev, atom("which"), v_num(13), 0); }
    }
    prevented = dispatch(vm, ev, type, path, np, target != 0 || 1);
    UNTEMP(vm, 1);
    g_answer_flags = (u8)(prevented ? 1 : 0);
    return engine_finish(vm, err, errcap);
}

/* ---- the rest of window -------------------------------------------------------- */
NATIVE(w_log)
{
    static char line[2048];
    u32 n = 0;
    int i;
    UNUSED;
    for (i = 0; i < argc && n < sizeof line - 2; i++) {
        Str *s;
        if (i) line[n++] = ' ';
        s = args[i].t == T_OBJ && args[i].u.o->cls != C_ERROR && args[i].u.o->cls != C_FUNC ? json_stringify(vm, args[i]) : to_str(vm, args[i]);
        if (vm->throwing) { vm->throwing = 0; s = to_str(vm, args[i]); }
        if (s) n += str_to_utf8(s, line + n, sizeof line - 2 - n);
    }
    rq(D_LOG); rq_u32(magic); rq_bytes(line, n);
    knock(vm);
    return v_undef();
}

NATIVE(w_alert) { UNUSED; rq(D_ALERT); rq_str(vm, ARG(0)); knock(vm); return magic == 1 ? v_bool(0) : magic == 2 ? v_null() : v_undef(); }
NATIVE(w_href) { UNUSED; rq(D_HREF); if (!knock(vm)) return v_undef(); return rp_val(vm); }
NATIVE(w_go) { UNUSED; rq(D_GO); rq_str(vm, ARG(0)); knock(vm); return v_undef(); }
NATIVE(w_write)
{
    int i;
    UNUSED;
    for (i = 0; i < argc && !vm->throwing; i++) { rq(D_WRITE); rq_str(vm, args[i]); knock(vm); }
    if (magic) { rq(D_WRITE); rq_cstr("\n"); knock(vm); }
    return v_undef();
}

NATIVE(w_getid)
{
    UNUSED;
    rq(D_GETID); rq_str(vm, ARG(0));
    if (!knock(vm)) return v_undef();
    return rp_val(vm);
}

NATIVE(w_create)                           /* 0 createElement, 1 createTextNode, 2 createDocumentFragment, 3 createComment */
{
    UNUSED;
    if (magic == 1 || magic == 3) { rq(D_TEXT); rq_str(vm, magic == 3 ? v_str(S_empty) : ARG(0)); }
    else { rq(D_CREATE); if (magic == 2) rq_cstr("fl-frag"); else rq_str(vm, ARG(0)); }
    if (!knock(vm)) return v_undef();
    return wrap(vm, rp_u32());
}

NATIVE(w_byname)
{
    UNUSED;
    rq(D_NAMES); rq_str(vm, ARG(0));
    if (!knock(vm)) return v_undef();
    return rp_val(vm);
}

NATIVE(w_fetch)                             /* __flopos_fetch(method, url, body, type) */
{
    Obj *r;
    UNUSED;
    rq(D_FETCH); rq_str(vm, ARG(0)); rq_str(vm, ARG(1));
    if (IS_NULLISH(ARG(2))) rq_cstr(""); else rq_str(vm, ARG(2));
    if (IS_NULLISH(ARG(3))) rq_cstr(""); else rq_str(vm, ARG(3));
    if (!knock(vm)) return v_undef();
    r = arr_new(vm, 0);
    if (!r) return v_undef();
    TEMP(vm, v_obj(r));
    arr_push(vm, r, v_num(rp_u32()));
    arr_push(vm, r, v_str(rp_str()));
    arr_push(vm, r, v_str(rp_str()));
    UNTEMP(vm, 1);
    return v_obj(r);
}

NATIVE(w_shown)
{
    u32 n = node_of(ARG(0));
    UNUSED;
    if (!n) return v_bool(0);
    rq(D_SHOWN); rq_u32(n);
    if (!knock(vm)) return v_undef();
    return v_bool(s_rl > 1 && s_rep[1]);
}

NATIVE(w_now) { UNUSED; return v_num(host_ms()); }

/* document.currentScript: the <script> running now (room.c sets it as
 * each starts), or null -- in a module, an event or a timer. */
u32 g_current_script;
NATIVE(w_curscript) { UNUSED; return g_current_script ? wrap(vm, g_current_script) : v_null(); }

/* import(): asks browzer to load the module (after this task), for a
 * ticket; browzer settles it with T_DYNDONE, below. */
NATIVE(w_dynask)                            /* __flopos_dynask(address, base) */
{
    UNUSED;
    rq(D_IMPORT); rq_str(vm, ARG(0)); rq_str(vm, ARG(1));
    if (!knock(vm)) return v_undef();
    return rp_val(vm);
}

NATIVE(w_modns_url)                         /* __flopos_modns_url(address) */
{
    static char u[1024];
    Str *s = to_str(vm, ARG(0));
    u32 n;
    UNUSED;
    if (!s) return v_undef();
    n = str_to_utf8(s, u, sizeof u - 1);
    u[n] = 0;
    return module_ns(vm, u);
}

int room_dyn_done(Vm *vm, u32 ticket, const char *url, const char *why, char *err, u32 errcap)
{
    Val fn = get_prop(vm, v_obj(vm->global), atom("__flopos_dyndone")), a[3];
    Str *su = str_utf8(url, js_strlen(url)), *sw;
    if (!su) return ENGINE_ROOM;
    a[1] = v_str(su);
    TEMP(vm, a[1]);
    sw = str_utf8(why, js_strlen(why));
    UNTEMP(vm, 1);
    if (!sw) return ENGINE_ROOM;
    a[0] = v_num(ticket); a[2] = v_str(sw);
    if (!is_callable(fn)) return ENGINE_OK;
    return engine_call(vm, fn, v_undef(), a, 3, 500, err, errcap);
}

NATIVE(w_btoa)                             /* 0 btoa, 1 atob */
{
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    Str *s = to_str(vm, ARG(0)), *r;
    u32 i, n = 0;
    UNUSED;
    if (!magic) {
        r = str_new((s->len + 2) / 3 * 4);
        if (!r) return v_undef();
        for (i = 0; i < s->len; i += 3) {
            u32 a = s->c[i] & 255, b = i + 1 < s->len ? s->c[i + 1] & 255 : 0, c = i + 2 < s->len ? s->c[i + 2] & 255 : 0;
            if (s->c[i] > 255 || (i + 1 < s->len && s->c[i + 1] > 255) || (i + 2 < s->len && s->c[i + 2] > 255)) return throw_err(vm, E_ERROR, "btoa: characters outside Latin-1");
            r->c[n++] = (u16)b64[a >> 2];
            r->c[n++] = (u16)b64[((a & 3) << 4) | (b >> 4)];
            r->c[n++] = (u16)(i + 1 < s->len ? b64[((b & 15) << 2) | (c >> 6)] : '=');
            r->c[n++] = (u16)(i + 2 < s->len ? b64[c & 63] : '=');
        }
        r->len = n;
        return v_str(r);
    }
    r = str_new(s->len * 3 / 4 + 3);
    if (!r) return v_undef();
    {
        u32 acc = 0, bits = 0;
        for (i = 0; i < s->len; i++) {
            u16 c = s->c[i];
            int v = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52 : c == '+' || c == '-' ? 62 : c == '/' || c == '_' ? 63 : -1;
            if (v < 0) continue;
            acc = (acc << 6) | (u32)v; bits += 6;
            if (bits >= 8) { bits -= 8; r->c[n++] = (u16)((acc >> bits) & 255); }
        }
    }
    r->len = n;
    return v_str(r);
}

NATIVE(doc_cookie) { UNUSED; return v_str(S_empty); }     /* cookies are not offered */

/* ---- the prelude -------------------------------------------------------------------- */
static const char *s_prelude =
"(function(g){"
"function Storage(){this._d={};}"
"Storage.prototype.getItem=function(k){k=String(k);return Object.prototype.hasOwnProperty.call(this._d,k)?this._d[k]:null;};"
"Storage.prototype.setItem=function(k,v){this._d[String(k)]=String(v);};"
"Storage.prototype.removeItem=function(k){delete this._d[String(k)];};"
"Storage.prototype.clear=function(){this._d={};};"
"Storage.prototype.key=function(i){return Object.keys(this._d)[i]||null;};"
"Object.defineProperty(Storage.prototype,'length',{get:function(){return Object.keys(this._d).length;}});"
"g.localStorage=new Storage();g.sessionStorage=new Storage();"
"function Event(t,o){this.type=t;this.bubbles=!!(o&&o.bubbles);this.cancelable=!!(o&&o.cancelable);this.defaultPrevented=false;this.timeStamp=performance.now();}"
"Event.prototype=__flopos_event_proto;Event.prototype.constructor=Event;"
"function CustomEvent(t,o){Event.call(this,t,o);this.detail=o&&o.detail!==undefined?o.detail:null;}"
"CustomEvent.prototype=Event.prototype;"
"g.Event=Event;g.CustomEvent=CustomEvent;g.KeyboardEvent=Event;g.MouseEvent=Event;g.InputEvent=Event;"
"function Observer(cb){this.cb=cb;}Observer.prototype.observe=function(){};Observer.prototype.unobserve=function(){};"
"Observer.prototype.disconnect=function(){};Observer.prototype.takeRecords=function(){return [];};"
"g.MutationObserver=Observer;g.ResizeObserver=Observer;"
"g.IntersectionObserver=function(cb){this.cb=cb;};"
"g.IntersectionObserver.prototype.observe=function(el){var cb=this.cb,self=this;setTimeout(function(){cb([{target:el,isIntersecting:true,intersectionRatio:1}],self);},0);};"
"g.IntersectionObserver.prototype.unobserve=g.IntersectionObserver.prototype.disconnect=function(){};"
"function Headers(h){this._h={};if(h)for(var k in h)this._h[k.toLowerCase()]=String(h[k]);}"
"Headers.prototype.get=function(k){var v=this._h[String(k).toLowerCase()];return v===undefined?null:v;};"
"Headers.prototype.set=function(k,v){this._h[String(k).toLowerCase()]=String(v);};"
"Headers.prototype.has=function(k){return String(k).toLowerCase() in this._h;};"
"g.Headers=Headers;"
"function Response(s,t,b,u){this.status=s;this.ok=s>=200&&s<300;this.statusText=s==200?'OK':'';this.url=u;this.headers=new Headers({'content-type':t});this._b=b;this.bodyUsed=false;}"
"Response.prototype.text=function(){this.bodyUsed=true;return Promise.resolve(this._b);};"
"Response.prototype.json=function(){var b=this._b;this.bodyUsed=true;return new Promise(function(ok){ok(JSON.parse(b));});};"
"Response.prototype.clone=function(){return new Response(this.status,this.headers.get('content-type'),this._b,this.url);};"
"g.Response=Response;"
"g.fetch=function(u,o){o=o||{};var url=typeof u==='string'?u:(u&&u.url)||String(u);"
" var h=o.headers||{},t=(h.get?h.get('content-type'):(h['Content-Type']||h['content-type']))||'';"
" var b=o.body===undefined||o.body===null?'':(typeof o.body==='string'?o.body:(o.body instanceof URLSearchParams?(t=t||'application/x-www-form-urlencoded',o.body.toString()):JSON.stringify(o.body)));"
" if(o.body instanceof FormData){t=t||'application/x-www-form-urlencoded';b=o.body.toString();}"
" return new Promise(function(ok,no){if(o.signal&&o.signal.aborted){no(o.signal.reason);return;}var r=__flopos_fetch(o.method||'GET',url,b,t);if(!r[0]){no(new TypeError('Failed to fetch'));return;}ok(new Response(r[0],r[1],r[2],url));});};"
"function XMLHttpRequest(){this.readyState=0;this.status=0;this.responseText='';this.response='';this._h={};this.responseType='';}"
"XMLHttpRequest.prototype.open=function(m,u,a){this._m=m;this._u=u;this._a=a!==false;this.readyState=1;};"
"XMLHttpRequest.prototype.setRequestHeader=function(k,v){this._h[k.toLowerCase()]=v;};"
"XMLHttpRequest.prototype.getResponseHeader=function(k){return String(k).toLowerCase()==='content-type'?this._t:null;};"
"XMLHttpRequest.prototype.getAllResponseHeaders=function(){return 'content-type: '+this._t+'\\r\\n';};"
"XMLHttpRequest.prototype.abort=function(){};"
"XMLHttpRequest.prototype.addEventListener=function(t,f){this['on'+t]=f;};"
"XMLHttpRequest.prototype.send=function(b){var x=this;function go(){var r=__flopos_fetch(x._m,x._u,b==null?'':String(b),x._h['content-type']||'');"
" x.status=r[0];x._t=r[1];x.responseText=r[2];x.response=x.responseType==='json'?(function(){try{return JSON.parse(r[2]);}catch(e){return null;}})():r[2];x.readyState=4;"
" if(x.onreadystatechange)x.onreadystatechange();if(x.onload&&x.status)x.onload();if(x.onerror&&!x.status)x.onerror();if(x.onloadend)x.onloadend();}"
" if(this._a)setTimeout(go,0);else go();};"
"XMLHttpRequest.DONE=4;g.XMLHttpRequest=XMLHttpRequest;"
"function URLSearchParams(q){this._p=[];if(typeof q==='string'){q=q.replace(/^\\?/,'');if(q)q.split('&').forEach(function(kv){var i=kv.indexOf('='),k=i<0?kv:kv.slice(0,i),v=i<0?'':kv.slice(i+1);this._p.push([decodeURIComponent(k.replace(/\\+/g,' ')),decodeURIComponent(v.replace(/\\+/g,' '))]);},this);}"
" else if(q&&typeof q==='object')for(var k in q)this._p.push([k,String(q[k])]);}"
"URLSearchParams.prototype.get=function(k){for(var i=0;i<this._p.length;i++)if(this._p[i][0]===k)return this._p[i][1];return null;};"
"URLSearchParams.prototype.getAll=function(k){return this._p.filter(function(p){return p[0]===k;}).map(function(p){return p[1];});};"
"URLSearchParams.prototype.has=function(k){return this.get(k)!==null;};"
"URLSearchParams.prototype.set=function(k,v){this['delete'](k);this._p.push([k,String(v)]);};"
"URLSearchParams.prototype.append=function(k,v){this._p.push([k,String(v)]);};"
"URLSearchParams.prototype['delete']=function(k){this._p=this._p.filter(function(p){return p[0]!==k;});};"
"URLSearchParams.prototype.forEach=function(f){this._p.forEach(function(p){f(p[1],p[0]);});};"
"URLSearchParams.prototype.toString=function(){return this._p.map(function(p){return encodeURIComponent(p[0])+'='+encodeURIComponent(p[1]);}).join('&');};"
"g.URLSearchParams=URLSearchParams;"
"function parts(h){var m=/^([a-z]+:)\\/\\/([^\\/?#:]*)(:\\d+)?([^?#]*)(\\?[^#]*)?(#.*)?$/i.exec(h)||[];"
" return{href:h,protocol:m[1]||'',hostname:m[2]||'',port:(m[3]||'').slice(1),host:(m[2]||'')+(m[3]||''),pathname:m[4]||'/',search:m[5]||'',hash:m[6]||'',origin:(m[1]||'')+'//'+(m[2]||'')+(m[3]||'')};}"
"function URL(u,base){if(base&&!/^[a-z]+:/i.test(u)){var b=parts(String(base));u=u.charAt(0)==='/'?b.origin+u:b.origin+b.pathname.replace(/[^\\/]*$/,'')+u;}"
" var p=parts(String(u));for(var k in p)this[k]=p[k];this.searchParams=new URLSearchParams(p.search);}"
"URL.prototype.toString=function(){return this.href;};g.URL=URL;"
"var loc={};['href','protocol','hostname','port','host','pathname','search','hash','origin'].forEach(function(k){"
" Object.defineProperty(loc,k,{get:function(){return parts(__flopos_href())[k];},set:function(v){if(k==='href')__flopos_go(v);else if(k==='hash'){}else{var p=parts(__flopos_href());p[k]=v;__flopos_go(p.protocol+'//'+p.host+p.pathname+p.search);}}});});"
"loc.assign=loc.replace=function(u){__flopos_go(u);};loc.reload=function(){__flopos_go(__flopos_href());};loc.toString=function(){return __flopos_href();};"
"g.location=loc;document.location=loc;"
"g.history={length:1,state:null,pushState:function(s){this.state=s;},replaceState:function(s){this.state=s;},back:function(){},forward:function(){},go:function(){}};"
"g.navigator={userAgent:__flopos_ua,language:'en',languages:['en'],onLine:true,cookieEnabled:false,platform:'FLOPOS',vendor:'',appName:'Netscape',doNotTrack:'1',maxTouchPoints:0};"
"g.screen={width:800,height:600,availWidth:800,availHeight:600,colorDepth:8};"
"g.innerWidth=800;g.innerHeight=600;g.outerWidth=800;g.outerHeight=600;g.devicePixelRatio=1;"
"g.scrollX=g.scrollY=g.pageXOffset=g.pageYOffset=0;"
"g.scrollTo=g.scroll=g.scrollBy=g.focus=g.blur=g.print=g.stop=function(){};"
"g.performance={now:__flopos_now,mark:function(){},measure:function(){},getEntriesByName:function(){return [];}};"
"g.requestIdleCallback=function(f){return setTimeout(function(){f({didTimeout:false,timeRemaining:function(){return 10;}});},1);};g.cancelIdleCallback=clearTimeout;"
"g.matchMedia=function(q){var m=true;q.replace(/\\((min|max)-width:\\s*([\\d.]+)(px|em|rem)?\\)/g,function(_,w,n,u){n=parseFloat(n)*(u==='em'||u==='rem'?16:1);if(w==='min'?800<n:800>n)m=false;});"
" if(/prefers-color-scheme:\\s*light/.test(q)||/print/.test(q))m=false;return{matches:m,media:q,addListener:function(){},removeListener:function(){},addEventListener:function(){},removeEventListener:function(){}};};"
"g.getComputedStyle=function(el){var shown=__flopos_shown(el);return{display:shown?'block':'none',visibility:shown?'visible':'hidden',getPropertyValue:function(k){return k==='display'?(shown?'block':'none'):el.style[k]||'';}};};"
"g.Image=function(){return document.createElement('img');};"
"g.frames=g;g.parent=g;g.top=g;g.opener=null;g.name='';g.closed=false;g.isSecureContext=true;g.origin=loc.origin;"
/* Browser pieces pages stop on without (2026-11-16). */
"var sid=0;function Symbol(d){return '@@'+(d===undefined?'':String(d))+'@'+(++sid);}"
"var reg={};Symbol['for']=function(k){k=String(k);return reg[k]||(reg[k]='@@'+k);};Symbol.keyFor=function(s){for(var k in reg)if(reg[k]===s)return k;return undefined;};"
"['iterator','asyncIterator','hasInstance','toPrimitive','toStringTag','species','isConcatSpreadable','unscopables','match','replace','search','split'].forEach(function(n){Symbol[n]='@@'+n;});"
"g.Symbol=Symbol;"
"if(!Promise.any)Promise.any=function(a){return new Promise(function(ok,no){var n=a.length,errs=[];if(!n)no(new Error('AggregateError'));a.forEach(function(p,i){Promise.resolve(p).then(ok,function(e){errs[i]=e;if(--n===0){var x=new Error('AggregateError');x.errors=errs;no(x);}});});});};"
"function EventTarget(){this.__l={};}"
"EventTarget.prototype.addEventListener=function(t,f){if(!this.__l)this.__l={};if(f)(this.__l[t]=this.__l[t]||[]).push(f);};"
"EventTarget.prototype.removeEventListener=function(t,f){var l=this.__l&&this.__l[t];if(l){var i=l.indexOf(f);if(i>=0)l.splice(i,1);}};"
"EventTarget.prototype.dispatchEvent=function(e){var l=((this.__l&&this.__l[e.type])||[]).slice(),h=this['on'+e.type];e.target=e.currentTarget=this;for(var i=0;i<l.length;i++)(typeof l[i]==='function'?l[i].call(this,e):l[i].handleEvent(e));if(typeof h==='function')h.call(this,e);return !e.defaultPrevented;};"
"g.EventTarget=EventTarget;"
"function abortErr(n){var e=new Error(n==='TimeoutError'?'The operation timed out.':'The operation was aborted.');e.name=n||'AbortError';return e;}"
"function AbortSignal(){EventTarget.call(this);this.aborted=false;this.reason=undefined;this.onabort=null;}"
"AbortSignal.prototype=Object.create(EventTarget.prototype);AbortSignal.prototype.constructor=AbortSignal;"
"AbortSignal.prototype.throwIfAborted=function(){if(this.aborted)throw this.reason;};"
"function AbortController(){this.signal=new AbortSignal();}"
"AbortController.prototype.abort=function(r){var s=this.signal;if(s.aborted)return;s.aborted=true;s.reason=r===undefined?abortErr():r;s.dispatchEvent(new Event('abort'));};"
"AbortSignal.abort=function(r){var c=new AbortController();c.abort(r);return c.signal;};"
"AbortSignal.timeout=function(ms){var c=new AbortController();setTimeout(function(){c.abort(abortErr('TimeoutError'));},ms);return c.signal;};"
"AbortSignal.any=function(a){var c=new AbortController();a.forEach(function(s){if(s.aborted)c.abort(s.reason);else s.addEventListener('abort',function(){c.abort(s.reason);});});return c.signal;};"
"g.AbortController=AbortController;g.AbortSignal=AbortSignal;"
"function FormData(f){this._p=[];if(f&&f.querySelectorAll){var els=f.querySelectorAll('input,select,textarea');for(var i=0;i<els.length;i++){var el=els[i],n=el.name,t=(el.type||'').toLowerCase();if(!n||el.disabled)continue;if((t==='checkbox'||t==='radio')&&!el.checked)continue;if(t==='submit'||t==='button'||t==='reset'||t==='file'||t==='image')continue;this._p.push([n,String(el.value==null?'':el.value)]);}}}"
"['get','getAll','has','set','append','delete','forEach','toString'].forEach(function(k){FormData.prototype[k]=URLSearchParams.prototype[k];});"
"FormData.prototype.entries=function(){return this._p.map(function(p){return [p[0],p[1]];});};"
"FormData.prototype.keys=function(){return this._p.map(function(p){return p[0];});};"
"FormData.prototype.values=function(){return this._p.map(function(p){return p[1];});};"
"g.FormData=FormData;"
"function TextEncoder(){this.encoding='utf-8';}"
"TextEncoder.prototype.encode=function(s){s=String(s===undefined?'':s);var o=[];for(var i=0;i<s.length;i++){var c=s.charCodeAt(i);if(c>=0xD800&&c<0xDC00&&i+1<s.length){c=0x10000+((c-0xD800)<<10)+(s.charCodeAt(++i)-0xDC00);}"
" if(c<0x80)o.push(c);else if(c<0x800)o.push(0xC0|c>>6,0x80|c&63);else if(c<0x10000)o.push(0xE0|c>>12,0x80|c>>6&63,0x80|c&63);else o.push(0xF0|c>>18,0x80|c>>12&63,0x80|c>>6&63,0x80|c&63);}return o;};"
"function TextDecoder(){this.encoding='utf-8';}"
"TextDecoder.prototype.decode=function(b){var s='',i=0;b=b||[];while(i<b.length){var c=b[i++];if(c>=0xF0){c=(c&7)<<18|(b[i++]&63)<<12|(b[i++]&63)<<6|(b[i++]&63);}else if(c>=0xE0){c=(c&15)<<12|(b[i++]&63)<<6|(b[i++]&63);}else if(c>=0xC0){c=(c&31)<<6|(b[i++]&63);}s+=String.fromCodePoint(c);}return s;};"
"g.TextEncoder=TextEncoder;g.TextDecoder=TextDecoder;"
"g.structuredClone=function(v){return v===undefined?undefined:JSON.parse(JSON.stringify(v));};"
"g.customElements={define:function(){},get:function(){return undefined;},whenDefined:function(){return Promise.resolve();},upgrade:function(){}};"
"g.CSS={supports:function(){return false;},escape:function(s){return String(s).replace(/[^a-zA-Z0-9_ -￿-]/g,function(c){return '\\\\'+c;});}};"
"g.navigator.sendBeacon=function(){return false;};"
"document.createEvent=function(){var e=new Event('');e.initEvent=function(t,b,c){this.type=t;this.bubbles=!!b;this.cancelable=!!c;};e.initCustomEvent=function(t,b,c,d){this.initEvent(t,b,c);this.detail=d;};return e;};"
"var EP=Object.getPrototypeOf(document);"
"function Node(){throw new TypeError('Illegal constructor');}"
"Node.prototype=EP;Node.ELEMENT_NODE=1;Node.TEXT_NODE=3;Node.COMMENT_NODE=8;Node.DOCUMENT_NODE=9;Node.DOCUMENT_FRAGMENT_NODE=11;"
"g.Node=Node;"
"['Element','HTMLElement','HTMLDivElement','HTMLInputElement','HTMLFormElement','HTMLAnchorElement','HTMLButtonElement','HTMLSelectElement','HTMLTextAreaElement','HTMLImageElement','HTMLScriptElement','HTMLDocument','Document','Text','DocumentFragment'].forEach(function(n){var f=function(){throw new TypeError('Illegal constructor');};f.prototype=EP;g[n]=f;});"
"document.implementation={hasFeature:function(){return true;},createHTMLDocument:function(t){var h=document.createElement('html'),hd=document.createElement('head'),b=document.createElement('body');h.appendChild(hd);h.appendChild(b);return{nodeType:9,title:t||'',documentElement:h,head:hd,body:b,implementation:document.implementation,createElement:function(x){return document.createElement(x);},createTextNode:function(x){return document.createTextNode(x);},createComment:function(x){return document.createComment(x);},createDocumentFragment:function(){return document.createDocumentFragment();},getElementById:function(i){return h.querySelector('#'+i);},querySelector:function(q){return h.querySelector(q);},querySelectorAll:function(q){return h.querySelectorAll(q);}};}};"
"var pend={};"
"g.__flopos_import=function(spec,meta){return new Promise(function(ok,no){var t=__flopos_dynask(String(spec),meta&&meta.url?meta.url:'');"
" if(typeof t!=='number'||t<0){no(new TypeError('import() of '+spec+' was not allowed'));return;}pend[t]={ok:ok,no:no};});};"
"g.__flopos_dyndone=function(t,url,why){var p=pend[t];delete pend[t];if(!p)return;if(why)p.no(new TypeError(why));else p.ok(__flopos_modns_url(url));};"
"})(this);";

static void defs(Vm *vm)
{
    Obj *g = vm->global, *d, *e, *s, *c, *ev;
    static const struct { const char *n; u32 p; int set; } props[] = {
        { "tagName", DP_TAG, 0 }, { "nodeName", DP_TAG, 0 }, { "id", DP_ID, 1 }, { "className", DP_CLASS, 1 },
        { "textContent", DP_TEXT, 1 }, { "innerText", DP_TEXT, 1 }, { "innerHTML", DP_HTML, 1 }, { "outerHTML", DP_OUTER, 1 },
        { "value", DP_VALUE, 1 }, { "checked", DP_CHECKED, 1 }, { "disabled", DP_DISABLED, 1 }, { "selected", DP_SELECTED, 1 },
        { "selectedIndex", DP_SELINDEX, 1 }, { "href", DP_HREF, 1 }, { "src", DP_SRC, 1 }, { "name", DP_NAME, 1 },
        { "type", DP_TYPE, 1 }, { "parentNode", DP_PARENT, 0 }, { "parentElement", DP_PARENT, 0 }, { "firstChild", DP_FIRST, 0 },
        { "lastChild", DP_LAST, 0 }, { "nextSibling", DP_NEXT, 0 }, { "previousSibling", DP_PREV, 0 },
        { "children", DP_CHILDREN, 0 }, { "childNodes", DP_NODES, 0 }, { "firstElementChild", DP_FIRSTEL, 0 },
        { "lastElementChild", DP_LASTEL, 0 }, { "nextElementSibling", DP_NEXTEL, 0 }, { "previousElementSibling", DP_PREVEL, 0 },
        { "title", DP_TITLE, 1 }, { "placeholder", DP_PLACEHOLDER, 1 }, { "action", DP_ACTION, 1 }, { "method", DP_METHOD, 1 },
        { "form", DP_FORM, 0 }, { "nodeType", DP_NODETYPE, 0 }, { "data", DP_DATA, 1 }, { "nodeValue", DP_DATA, 1 },
        { "hidden", DP_HIDDEN, 1 }, { "options", DP_OPTIONS, 0 }, { "childElementCount", DP_COUNTEL, 0 },
        { "readOnly", DP_READONLY, 1 }, { "alt", DP_ALT, 1 }, { "label", DP_LABEL, 1 }, { 0, 0, 0 } };
    static const char *ons[] = { "onclick", "onsubmit", "onchange", "oninput", "onkeyup", "onkeydown", "onkeypress",
        "onload", "onfocus", "onblur", "onmouseover", "onmouseout", "onmousedown", "onmouseup", "onerror", "onresize",
        "onscroll", "onDOMContentLoaded", "onreset", "onselect", 0 };
    int i;

    K_ev = atom("%ev"); K_type = atom("type"); K_target = atom("target"); K_current = atom("currentTarget");
    K_prevented = atom("defaultPrevented"); K_stopped = atom("%stopped");

    /* elements */
    e = vm->element_proto = obj_new(vm, vm->object_proto, C_OBJECT);
    for (i = 0; props[i].n; i++) def_accessor(vm, e, props[i].n, el_get, props[i].set ? el_set : 0, props[i].p);
    def_accessor(vm, e, "localName", el_get, 0, DP_TAG);
    def_accessor(vm, e, "style", el_style, 0, 0);
    def_accessor(vm, e, "classList", el_classlist, 0, 0);
    def_accessor(vm, e, "dataset", el_dataset, 0, 0);
    def_accessor(vm, e, "ownerDocument", el_document, 0, 0);
    def_accessor(vm, e, "offsetWidth", el_shown, 0, 0);
    def_accessor(vm, e, "offsetHeight", el_shown, 0, 1);
    def_accessor(vm, e, "clientWidth", el_shown, 0, 0);
    def_accessor(vm, e, "clientHeight", el_shown, 0, 1);
    def_accessor(vm, e, "scrollWidth", el_shown, 0, 0);
    def_accessor(vm, e, "scrollHeight", el_shown, 0, 1);
    def_accessor(vm, e, "offsetParent", el_shown, 0, 9);
    for (i = 0; ons[i]; i++) def_accessor(vm, e, ons[i], on_get, on_set, 0);
    def_native(vm, e, "getAttribute", el_attr, 1, 0);
    def_native(vm, e, "setAttribute", el_attr, 2, 1);
    def_native(vm, e, "removeAttribute", el_attr, 1, 2);
    def_native(vm, e, "hasAttribute", el_attr, 1, 3);
    def_native(vm, e, "appendChild", el_append, 1, 0);
    def_native(vm, e, "append", el_append, 1, 1);
    def_native(vm, e, "prepend", el_append, 1, 2);
    def_native(vm, e, "insertBefore", el_insertbefore, 2, 0);
    def_native(vm, e, "removeChild", el_removechild, 1, 0);
    def_native(vm, e, "remove", el_removechild, 0, 1);
    def_native(vm, e, "replaceChild", el_replacechild, 2, 0);
    def_native(vm, e, "before", el_beforeafter, 1, 0);
    def_native(vm, e, "after", el_beforeafter, 1, 1);
    def_native(vm, e, "replaceWith", el_beforeafter, 1, 2);
    def_native(vm, e, "querySelector", el_query, 1, 0);
    def_native(vm, e, "querySelectorAll", el_query, 1, 1);
    def_native(vm, e, "getElementsByTagName", el_bytag, 1, 0);
    def_native(vm, e, "getElementsByClassName", el_bytag, 1, 1);
    def_native(vm, e, "contains", el_contains, 1, 0);
    def_native(vm, e, "matches", el_matches, 1, 0);
    def_native(vm, e, "closest", el_matches, 1, 1);
    def_native(vm, e, "cloneNode", el_clone, 1, 0);
    def_native(vm, e, "insertAdjacentHTML", el_insertadjacent, 2, 0);
    def_native(vm, e, "insertAdjacentElement", el_insertadjacent, 2, 1);
    def_native(vm, e, "insertAdjacentText", el_insertadjacent, 2, 1);
    def_native(vm, e, "addEventListener", ev_add, 2, 0);
    def_native(vm, e, "removeEventListener", ev_remove, 2, 0);
    def_native(vm, e, "dispatchEvent", ev_dispatch, 1, 0);
    def_native(vm, e, "click", ev_dispatch, 0, 1);
    def_native(vm, e, "focus", el_nothing, 0, 0);
    def_native(vm, e, "blur", el_nothing, 0, 0);
    def_native(vm, e, "select", el_nothing, 0, 0);
    def_native(vm, e, "scrollIntoView", el_nothing, 0, 0);
    def_native(vm, e, "getBoundingClientRect", el_rect, 0, 0);
    def_native(vm, e, "hasChildNodes", el_nothing, 0, 0);

    /* el.style */
    s = vm->style_proto = obj_new(vm, vm->object_proto, C_OBJECT);
    def_native(vm, s, "setProperty", style_setprop, 2, 0);
    def_native(vm, s, "removeProperty", style_setprop, 1, 1);
    def_native(vm, s, "getPropertyValue", style_setprop, 1, 2);
    c = vm->classlist_proto = obj_new(vm, vm->object_proto, C_OBJECT);
    def_native(vm, c, "add", classlist_op, 1, 0);
    def_native(vm, c, "remove", classlist_op, 1, 1);
    def_native(vm, c, "toggle", classlist_op, 1, 2);
    def_native(vm, c, "contains", classlist_op, 1, 3);
    def_native(vm, c, "replace", classlist_op, 2, 0);
    {
        Obj *f = native_new(vm, "replace", classlist_replace, 2, 0);
        if (f) prop_put(vm, c, atom("replace"), v_obj(f), P_HIDDEN);
    }

    /* events */
    ev = vm->event_proto = obj_new(vm, vm->object_proto, C_OBJECT);
    def_native(vm, ev, "preventDefault", ev_prevent, 0, 0);
    def_native(vm, ev, "stopPropagation", ev_prevent, 0, 1);
    def_native(vm, ev, "stopImmediatePropagation", ev_prevent, 0, 2);
    def_val(vm, g, "__flopos_event_proto", v_obj(ev));

    /* document */
    d = vm->document_obj = obj_new(vm, e, C_HOST);
    d->u.host.node = 1;
    def_val(vm, g, "document", v_obj(d));
    def_native(vm, d, "getElementById", w_getid, 1, 0);
    def_native(vm, d, "createElement", w_create, 1, 0);
    def_native(vm, d, "createElementNS", w_create, 2, 0);
    def_native(vm, d, "createTextNode", w_create, 1, 1);
    def_native(vm, d, "createDocumentFragment", w_create, 0, 2);
    def_native(vm, d, "createComment", w_create, 1, 3);
    def_native(vm, d, "getElementsByName", w_byname, 1, 0);
    def_native(vm, d, "write", w_write, 1, 0);
    def_native(vm, d, "writeln", w_write, 1, 1);
    def_native(vm, d, "hasFocus", el_nothing, 0, 0);
    def_accessor(vm, d, "body", el_get, 0, DP_BODY);
    def_accessor(vm, d, "head", el_get, 0, DP_HEAD);
    def_accessor(vm, d, "documentElement", el_get, 0, DP_ROOT);
    def_accessor(vm, d, "URL", w_href, 0, 0);
    def_accessor(vm, d, "currentScript", w_curscript, 0, 0);
    def_accessor(vm, d, "documentURI", w_href, 0, 0);
    def_accessor(vm, d, "cookie", doc_cookie, el_nothing, 0);
    def_val(vm, d, "readyState", v_str(atom("complete")));
    def_val(vm, d, "referrer", v_str(S_empty));
    def_val(vm, d, "visibilityState", v_str(atom("visible")));
    def_val(vm, d, "hidden", v_bool(0));
    def_val(vm, d, "defaultView", v_obj(g));
    def_val(vm, d, "nodeType", v_num(9));
    def_val(vm, d, "characterSet", v_str(atom("UTF-8")));
    def_val(vm, d, "compatMode", v_str(atom("CSS1Compat")));
    def_val(vm, d, "activeElement", v_null());

    /* window */
    {
        Obj *con = obj_new(vm, vm->object_proto, C_OBJECT);
        def_val(vm, g, "console", v_obj(con));
        def_native(vm, con, "log", w_log, 1, 0);
        def_native(vm, con, "info", w_log, 1, 0);
        def_native(vm, con, "debug", w_log, 1, 0);
        def_native(vm, con, "warn", w_log, 1, 1);
        def_native(vm, con, "error", w_log, 1, 2);
        def_native(vm, con, "trace", w_log, 1, 0);
        def_native(vm, con, "group", el_nothing, 0, 0);
        def_native(vm, con, "groupEnd", el_nothing, 0, 0);
        def_native(vm, con, "time", el_nothing, 0, 0);
        def_native(vm, con, "timeEnd", el_nothing, 0, 0);
        def_native(vm, con, "table", w_log, 1, 0);
        def_native(vm, con, "assert", el_nothing, 0, 0);
    }
    def_native(vm, g, "alert", w_alert, 1, 0);
    def_native(vm, g, "confirm", w_alert, 1, 1);
    def_native(vm, g, "prompt", w_alert, 1, 2);
    def_native(vm, g, "addEventListener", ev_add, 2, 0);
    def_native(vm, g, "removeEventListener", ev_remove, 2, 0);
    def_native(vm, g, "dispatchEvent", ev_dispatch, 1, 0);
    def_native(vm, g, "btoa", w_btoa, 1, 0);
    def_native(vm, g, "atob", w_btoa, 1, 1);
    def_native(vm, g, "__flopos_fetch", w_fetch, 4, 0);
    def_native(vm, g, "__flopos_href", w_href, 0, 0);
    def_native(vm, g, "__flopos_go", w_go, 1, 0);
    def_native(vm, g, "__flopos_shown", w_shown, 1, 0);
    def_native(vm, g, "__flopos_now", w_now, 0, 0);
    def_native(vm, g, "__flopos_dynask", w_dynask, 2, 0);
    def_native(vm, g, "__flopos_modns_url", w_modns_url, 1, 0);
    def_val(vm, g, "__flopos_ua", v_str(atom("Mozilla/5.0 (compatible; FLOPOS; a 1996 Toshiba)")));
    for (i = 0; ons[i]; i++) def_accessor(vm, g, ons[i], on_get, on_set, 0);
}

void host_init(Vm *vm)
{
    char why[200];
    defs(vm);
    if (engine_run(vm, s_prelude, (u32)js_strlen(s_prelude), "prelude", 5000, why, sizeof why) != ENGINE_OK) {
        rq(D_LOG); rq_u32(2); rq_cstr("the room's prelude failed: "); knock(vm);
        rq(D_LOG); rq_u32(2); rq_bytes(why, (u32)js_strlen(why)); knock(vm);
    }
}
