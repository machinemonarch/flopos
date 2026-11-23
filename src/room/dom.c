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

/* A fetch's answer: [status, type, body, headers].  A body longer than the
 * door holds comes in pieces (D_FETCHMORE), gathered as bytes in one block
 * and made a string once, at the end -- no sweep comes between, as nothing
 * here runs a script.  Past FETCH_MAX it fails: the scripts have 2 MB in
 * all, and a string takes two bytes a character. */
#define FETCH_MAX (512u << 10)
static const u8 *rp_raw(u32 *n)
{
    u32 k = rp_u32();
    const u8 *p = s_rep + s_rp;
    if (s_rp + k > s_rl) k = s_rl - s_rp;
    s_rp += k;
    *n = k;
    return p;
}

NATIVE(w_fetch)                             /* __flopos_fetch(method, url, body, type) */
{
    Obj *r;
    u32 st, n, len = 0, cap = 0, more;
    Str *type, *heads, *body = S_empty;
    const u8 *p;
    char *buf = 0;
    UNUSED;
    rq(D_FETCH); rq_str(vm, ARG(0)); rq_str(vm, ARG(1));
    if (IS_NULLISH(ARG(2))) rq_cstr(""); else rq_str(vm, ARG(2));
    if (IS_NULLISH(ARG(3))) rq_cstr(""); else rq_str(vm, ARG(3));
    rq_u32(0);                                   /* the whole body is here */
    if (!knock(vm)) return v_undef();
    st = rp_u32(); type = rp_str(); heads = rp_str();
    for (;;) {
        p = rp_raw(&n);
        more = rp_u32();
        if (!more && !len) { body = str_utf8((const char *)p, n); break; }    /* one piece: as it is */
        if (len + n > FETCH_MAX) { st = 0; body = S_empty; break; }         /* too long for the room */
        if (len + n > cap) {
            u32 nc = cap ? cap * 2 : 128u << 10;
            char *nb;
            while (nc < len + n) nc *= 2;
            if (nc > FETCH_MAX) nc = FETCH_MAX;
            nb = gc_alloc(nc + 8, K_BYTES);
            if (!nb) { vm->out_of_room = 1; return v_undef(); }
            if (len) memcpy(nb + 8, buf + 8, len);
            buf = nb; cap = nc;
        }
        memcpy(buf + 8 + len, p, n);
        len += n;
        if (!more) { body = str_utf8(buf + 8, len); break; }
        rq(D_FETCHMORE);
        if (!knock(vm)) return v_undef();
    }
    if (!body) body = S_empty;
    r = arr_new(vm, 0);
    if (!r) return v_undef();
    TEMP(vm, v_obj(r));
    arr_push(vm, r, v_num(st));
    arr_push(vm, r, v_str(type ? type : S_empty));
    arr_push(vm, r, v_str(body));
    arr_push(vm, r, v_str(heads ? heads : S_empty));
    UNTEMP(vm, 1);
    return v_obj(r);
}

/* XMLHttpRequest's stream: started with its body's first piece and the
 * whole length (the rest sent with __flopos_xsend, then __flopos_xdone),
 * read a piece at a time (__flopos_xmore), given up (__flopos_xend).
 * Bytes go both ways as Uint8Arrays. */
static void rq_view(Val v)
{
    if (v.t == T_OBJ && v.u.o->cls == C_TARRAY && v.u.o->pad == 1) rq_bytes((const char *)v.u.o->u.ta.data, v.u.o->u.ta.len);
    else rq_bytes("", 0);
}

static Val x_answer(Vm *vm)                    /* [status, type, headers, bytes, more] */
{
    Obj *r, *b;
    u32 st = rp_u32(), n, more;
    Str *type = rp_str(), *heads = rp_str();
    const u8 *p = rp_raw(&n);
    more = rp_u32();
    r = arr_new(vm, 0);
    if (!r) return v_undef();
    TEMP(vm, v_obj(r));
    arr_push(vm, r, v_num(st));
    arr_push(vm, r, v_str(type ? type : S_empty));
    arr_push(vm, r, v_str(heads ? heads : S_empty));
    b = ta_bytes(vm, p, n);
    arr_push(vm, r, b ? v_obj(b) : v_undef());
    arr_push(vm, r, v_num(more));
    UNTEMP(vm, 1);
    return v_obj(r);
}

NATIVE(w_xopen)                              /* method, url, type, whole length, first piece */
{
    UNUSED;
    rq(D_FETCH); rq_str(vm, ARG(0)); rq_str(vm, ARG(1)); rq_view(ARG(4));
    if (IS_NULLISH(ARG(2))) rq_cstr(""); else rq_str(vm, ARG(2));
    rq_u32((u32)to_num(vm, ARG(3)));
    if (!knock(vm)) return v_undef();
    return x_answer(vm);
}

NATIVE(w_xsend)                              /* a piece of the body */
{
    UNUSED;
    rq(D_SENDMORE); rq_view(ARG(0));
    if (!knock(vm)) return v_undef();
    return v_bool(rp_u32() != 0);
}

NATIVE(w_xdone)
{
    UNUSED;
    rq(D_SENDDONE);
    if (!knock(vm)) return v_undef();
    return x_answer(vm);
}

NATIVE(w_xmore)                              /* -> [bytes, more] */
{
    Obj *r, *b;
    u32 n, more;
    const u8 *p;
    UNUSED;
    rq(D_FETCHMORE);
    if (!knock(vm)) return v_undef();
    p = rp_raw(&n);
    more = rp_u32();
    r = arr_new(vm, 0);
    if (!r) return v_undef();
    TEMP(vm, v_obj(r));
    b = ta_bytes(vm, p, n);
    arr_push(vm, r, b ? v_obj(b) : v_undef());
    arr_push(vm, r, v_num(more));
    UNTEMP(vm, 1);
    return v_obj(r);
}

/* The rest of a long answer, after its first piece: gathered in one block,
 * handed over as one Uint8Array without a copy; null past FETCH_MAX. */
NATIVE(w_xrest)
{
    Val f = ARG(0);
    u32 n = 0, cap = 128u << 10, more = 1, k;
    const u8 *p;
    char *buf;
    UNUSED;
    buf = gc_alloc(cap + sizeof(GcHdr), K_BYTES);
    if (!buf) { vm->out_of_room = 1; return v_undef(); }
    if (f.t == T_OBJ && f.u.o->cls == C_TARRAY && f.u.o->pad == 1) { n = f.u.o->u.ta.len; if (n > cap) n = cap; memcpy(buf + sizeof(GcHdr), f.u.o->u.ta.data, n); }
    while (more) {
        rq(D_FETCHMORE);
        if (!knock(vm)) return v_undef();
        p = rp_raw(&k);
        more = rp_u32();
        if (n + k > FETCH_MAX) { rq(D_FETCHEND); knock(vm); return v_null(); }
        if (n + k > cap) {
            char *nb;
            while (cap < n + k) cap *= 2;
            if (cap > FETCH_MAX) cap = FETCH_MAX;
            nb = gc_alloc(cap + sizeof(GcHdr), K_BYTES);
            if (!nb) { vm->out_of_room = 1; return v_undef(); }
            memcpy(nb + sizeof(GcHdr), buf + sizeof(GcHdr), n);
            buf = nb;
        }
        memcpy(buf + sizeof(GcHdr) + n, p, k);
        n += k;
    }
    {
        Obj *o = ta_adopt(vm, (u8 *)buf + sizeof(GcHdr), n);
        return o ? v_obj(o) : v_undef();
    }
}

NATIVE(w_xend)
{
    UNUSED;
    rq(D_FETCHEND);
    if (!knock(vm)) return v_undef();
    return v_undef();
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
"function parts(h){var m=/^([a-z]+:)\\/\\/([^\\/?#:]*)(:\\d+)?([^?#]*)(\\?[^#]*)?(#.*)?$/i.exec(h)||[];"
" return{href:h,protocol:m[1]||'',hostname:m[2]||'',port:(m[3]||'').slice(1),host:(m[2]||'')+(m[3]||''),pathname:m[4]||'/',search:m[5]||'',hash:m[6]||'',origin:(m[1]||'')+'//'+(m[2]||'')+(m[3]||'')};}"
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
"var SP={'http:':'80','https:':'443','ws:':'80','wss:':'443','ftp:':'21','file:':''};"
"function uenc(s,re){return s.replace(re,function(c){return encodeURIComponent(c);});}"
"function unorm(p,sp){p=uenc(p,/[\\u0000- \"<>`{}\\u007f-￿]/g);var o=[],a=p.split('/');for(var i=0;i<a.length;i++){var x=a[i].toLowerCase();if(x==='..'||x==='%2e%2e'||x==='.%2e'||x==='%2e.'){if(o.length>1)o.pop();if(i===a.length-1)o.push('');}else if(x==='.'||x==='%2e'){if(i===a.length-1)o.push('');}else o.push(a[i]);}return o.join('/')||(sp?'/':'');}"
"function uhier(pr,rest){var m=/^\\/\\/([^\\/?#]*)([^?#]*)(\\?[^#]*)?(#.*)?$/.exec(rest);if(!m)return null;var a=m[1],u='',pw='',at=a.lastIndexOf('@');"
" if(at>=0){var ui=a.slice(0,at),c=ui.indexOf(':');a=a.slice(at+1);u=c<0?ui:ui.slice(0,c);pw=c<0?'':ui.slice(c+1);}"
" var pm=/^(\\[[^\\]]*\\]|[^:]*)(?::(\\d*))?$/.exec(a);if(!pm)return null;var host=pm[1].toLowerCase(),port=pm[2]||'';"
" if(port&&+port>65535)return null;if(port)port=String(+port);if(SP[pr]===port)port='';"
" if((pr in SP)&&pr!=='file:'&&!host)return null;if(/[\\s<>^|]/.test(host))return null;"
" return{protocol:pr,username:u,password:pw,hostname:host,port:port,pathname:unorm(m[2]||((pr in SP)?'/':''),pr in SP),search:m[3]&&m[3]!=='?'?m[3]:'',hash:m[4]&&m[4]!=='#'?m[4]:''};}"
"function urel(s,b){var r={protocol:b.protocol,username:b.username,password:b.password,hostname:b.hostname,port:b.port,pathname:b.pathname,search:b.search,hash:''};"
" if(b.opaque)return null;if(b.protocol in SP)s=s.replace(/\\\\/g,'/');if(/^\\/\\//.test(s))return uhier(b.protocol,s);"
" var m=/^([^?#]*)(\\?[^#]*)?(#.*)?$/.exec(s),p=m[1];"
" if(p){r.pathname=unorm(p.charAt(0)==='/'?p:b.pathname.replace(/[^\\/]*$/,'')+p,1);r.search=m[2]||'';}else if(m[2]!==undefined)r.search=m[2];"
" r.hash=m[3]||'';if(r.search==='?')r.search='';if(r.hash==='#')r.hash='';return r;}"
"function upar(s,b){s=String(s).replace(/^[\\u0000- ]+|[\\u0000- ]+$/g,'').replace(/[\\t\\n\\r]/g,'');var m=/^([a-z][a-z0-9+.\\-]*):/i.exec(s);"
" if(m){var pr=m[1].toLowerCase()+':',rest=s.slice(m[0].length);"
"  if(pr in SP){rest=rest.replace(/\\\\/g,'/');if(!/^\\/\\//.test(rest)){if(b&&b.protocol===pr)return urel(rest,b);rest='//'+rest.replace(/^\\/*/,'');}return uhier(pr,rest);}"
"  if(/^\\/\\//.test(rest))return uhier(pr,rest);"
"  var q=/^([^?#]*)(\\?[^#]*)?(#.*)?$/.exec(rest);return{protocol:pr,username:'',password:'',hostname:'',port:'',pathname:q[1],search:q[2]||'',hash:q[3]||'',opaque:1};}"
" return b?urel(s,b):null;}"
"function uhref(r){return r.protocol+(r.opaque?'':'//'+(r.username?r.username+(r.password?':'+r.password:'')+'@':'')+r.hostname+(r.port?':'+r.port:''))+r.pathname+r.search+r.hash;}"
"function URL(u,b){var base=null;if(b!==undefined){base=b instanceof URL?b._r:upar(b,null);if(!base)throw new TypeError('Invalid base URL: '+b);}"
" var r=upar(u instanceof URL?u.href:u,base);if(!r)throw new TypeError('Invalid URL: '+u);this._r=r;this._q=null;}"
"var UP=URL.prototype;function uacc(k,g,s){Object.defineProperty(UP,k,{get:g,set:s,enumerable:true,configurable:true});}"
"uacc('href',function(){return uhref(this._r);},function(v){var r=upar(v,null);if(!r)throw new TypeError('Invalid URL: '+v);this._r=r;if(this._q)this._q._set(r.search);});"
"uacc('origin',function(){var r=this._r;return (r.protocol in SP)&&r.protocol!=='file:'?r.protocol+'//'+r.hostname+(r.port?':'+r.port:''):'null';});"
"uacc('protocol',function(){return this._r.protocol;},function(v){v=String(v).replace(/:.*$/,'').toLowerCase()+':';if(/^[a-z][a-z0-9+.\\-]*:$/.test(v)&&((v in SP)===(this._r.protocol in SP))){this._r.protocol=v;if(SP[v]===this._r.port)this._r.port='';}});"
"uacc('username',function(){return this._r.username;},function(v){if(this._r.hostname)this._r.username=uenc(String(v),/[^\\w.~!$&'()*+,;=-]/g);});"
"uacc('password',function(){return this._r.password;},function(v){if(this._r.hostname)this._r.password=uenc(String(v),/[^\\w.~!$&'()*+,;=-]/g);});"
"uacc('host',function(){var r=this._r;return r.hostname+(r.port?':'+r.port:'');},function(v){var m=/^([^:\\/?#]*)(?::(\\d*))?/.exec(String(v));if(m&&m[1]&&!this._r.opaque){this._r.hostname=m[1].toLowerCase();if(m[2]!==undefined)this._r.port=SP[this._r.protocol]===m[2]?'':m[2];}});"
"uacc('hostname',function(){return this._r.hostname;},function(v){v=String(v);if(v&&!/[\\s\\/?#:]/.test(v)&&!this._r.opaque)this._r.hostname=v.toLowerCase();});"
"uacc('port',function(){return this._r.port;},function(v){v=String(v).replace(/\\D.*$/,'');if(!v||+v<=65535)this._r.port=v===''||SP[this._r.protocol]===String(+v)?'':String(+v);});"
"uacc('pathname',function(){return this._r.pathname;},function(v){if(!this._r.opaque){v=String(v);this._r.pathname=unorm(v.charAt(0)==='/'?v:'/'+v,1);}});"
"uacc('search',function(){return this._r.search.length>1?this._r.search:'';},function(v){v=String(v);this._r.search=v===''||v==='?'?'':(v.charAt(0)==='?'?v:'?'+v);if(this._q)this._q._set(this._r.search);});"
"uacc('hash',function(){return this._r.hash.length>1?this._r.hash:'';},function(v){v=String(v);this._r.hash=v===''||v==='#'?'':(v.charAt(0)==='#'?v:'#'+v);});"
"uacc('searchParams',function(){if(!this._q){this._q=new URLSearchParams(this._r.search);this._q._u=this;}return this._q;});"
"UP.toString=UP.toJSON=function(){return this.href;};"
"URL.canParse=function(u,b){try{new URL(u,b);return true;}catch(e){return false;}};"
"URL.parse=function(u,b){try{return new URL(u,b);}catch(e){return null;}};"
"URL.createObjectURL=function(){return 'blob:null/'+(++sid);};URL.revokeObjectURL=function(){};g.URL=URL;"
"function udec(s){try{return decodeURIComponent(s.replace(/\\+/g,' '));}catch(e){return s;}}"
"function ufenc(s){return encodeURIComponent(s).replace(/%20/g,'+').replace(/[!'()~]/g,function(c){return '%'+c.charCodeAt(0).toString(16).toUpperCase();});}"
"function URLSearchParams(q){this._p=[];this._u=null;if(q instanceof URLSearchParams)q=q.toString();"
" if(typeof q==='string')this._set(q);else if(q&&typeof q==='object'){if(Array.isArray(q)||typeof q[Symbol.iterator]==='function')Array.from(q).forEach(function(x){x=Array.from(x);if(x.length!==2)throw new TypeError('Each pair needs a name and a value');this._p.push([String(x[0]),String(x[1])]);},this);"
"  else for(var k in q)if(Object.prototype.hasOwnProperty.call(q,k))this._p.push([k,String(q[k])]);}else if(q!=null)this._set(String(q));}"
"var QP=URLSearchParams.prototype;"
"QP._set=function(s){this._p=[];s=String(s).replace(/^\\?/,'');if(s)s.split('&').forEach(function(kv){if(!kv)return;var i=kv.indexOf('=');this._p.push([udec(i<0?kv:kv.slice(0,i)),udec(i<0?'':kv.slice(i+1))]);},this);};"
"QP._up=function(){if(this._u){var s=this.toString();this._u._r.search=s?'?'+s:'';}};"
"QP.get=function(k){k=String(k);for(var i=0;i<this._p.length;i++)if(this._p[i][0]===k)return this._p[i][1];return null;};"
"QP.getAll=function(k){k=String(k);return this._p.filter(function(p){return p[0]===k;}).map(function(p){return p[1];});};"
"QP.has=function(k,v){k=String(k);return this._p.some(function(p){return p[0]===k&&(v===undefined||p[1]===String(v));});};"
"QP.set=function(k,v){k=String(k);v=String(v);var i,done=false,o=[];for(i=0;i<this._p.length;i++){if(this._p[i][0]!==k)o.push(this._p[i]);else if(!done){o.push([k,v]);done=true;}}if(!done)o.push([k,v]);this._p=o;this._up();};"
"QP.append=function(k,v){this._p.push([String(k),String(v)]);this._up();};"
"QP['delete']=function(k,v){k=String(k);this._p=this._p.filter(function(p){return !(p[0]===k&&(v===undefined||p[1]===String(v)));});this._up();};"
"QP.forEach=function(f,t){this._p.slice().forEach(function(p){f.call(t,p[1],p[0],this);},this);};"
"QP.sort=function(){this._p=this._p.map(function(p,i){return [p,i];}).sort(function(a,b){return a[0][0]<b[0][0]?-1:a[0][0]>b[0][0]?1:a[1]-b[1];}).map(function(x){return x[0];});this._up();};"
"QP.toString=function(){return this._p.map(function(p){return ufenc(p[0])+'='+ufenc(p[1]);}).join('&');};"
"QP.entries=QP[Symbol.iterator]=function(){return this._p.map(function(p){return [p[0],p[1]];}).values();};"
"QP.keys=function(){return this._p.map(function(p){return p[0];}).values();};"
"QP.values=function(){return this._p.map(function(p){return p[1];}).values();};"
"Object.defineProperty(QP,'size',{get:function(){return this._p.length;},configurable:true});"
"g.URLSearchParams=URLSearchParams;"
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
"function bytesOf(x){if(x instanceof Uint8Array)return x;if(x instanceof ArrayBuffer)return new Uint8Array(x);if(ArrayBuffer.isView(x))return new Uint8Array(x.buffer,x.byteOffset,x.byteLength);if(x instanceof Blob)return blobBytes(x);return new TextEncoder().encode(String(x));}"
"function blobBytes(b){if(b._p.length===1)return b._p[0];var o=new Uint8Array(b.size),at=0;b._p.forEach(function(u){o.set(u,at);at+=u.length;});return o;}"
"function Blob(parts,o){this._p=[];this.size=0;this.type=o&&o.type?String(o.type).toLowerCase():'';"
" Array.from(parts||[]).forEach(function(x){(x instanceof Blob?x._p:[bytesOf(x)]).forEach(function(u){this._p.push(u);this.size+=u.length;},this);},this);}"
"var BP=Blob.prototype;"
"BP.slice=function(s,e,t){var n=this.size,r=new Blob([],{type:t||''}),at=0;s=s===undefined?0:s<0?Math.max(n+s,0):Math.min(s,n);e=e===undefined?n:e<0?Math.max(n+e,0):Math.min(e,n);"
" this._p.forEach(function(u){var a=Math.max(s-at,0),z=Math.min(e-at,u.length);if(z>a){r._p.push(u.subarray(a,z));r.size+=z-a;}at+=u.length;});return r;};"
"BP.arrayBuffer=function(){return Promise.resolve(blobBytes(this).slice().buffer);};"
"BP.bytes=function(){return Promise.resolve(blobBytes(this).slice());};"
"BP.text=function(){return Promise.resolve(new TextDecoder().decode(blobBytes(this)));};"
"BP[Symbol.toStringTag]='Blob';g.Blob=Blob;"
"function File(parts,name,o){Blob.call(this,parts,o);this.name=String(name);this.lastModified=o&&o.lastModified||Date.now();}"
"File.prototype=Object.create(BP);File.prototype.constructor=File;File.prototype[Symbol.toStringTag]='File';g.File=File;"
"function bodyParts(b,m){if(b==null||m==='GET'||m==='HEAD')return [[],0,''];if(b instanceof Blob)return [b._p,b.size,b.type];"
" if(b instanceof ArrayBuffer||ArrayBuffer.isView(b)){var u=bytesOf(b);return [[u],u.length,''];}var x=bodyOf(b),e=new TextEncoder().encode(x[0]);return [[e],e.length,x[1]];}"
"function pieces(parts,size){var o=[];parts.forEach(function(u){for(var a=0;a<u.length;a+=size)o.push(u.subarray(a,Math.min(a+size,u.length)));});return o;}"
"function joinBytes(l,n){if(l.length===1)return l[0];var o=new Uint8Array(n),at=0;l.forEach(function(u){o.set(u,at);at+=u.length;});return o;}"
"function __flopos_sync(m,u,t,b){var bp=bodyParts(b,m),p=pieces(bp[0],16384),i=0,r=__flopos_xopen(m,u,t||bp[2],bp[1],p[i++]||new Uint8Array(0));"
" if(r[4]===2){while(i<p.length)if(!__flopos_xsend(p[i++])){__flopos_xend();return null;}r=__flopos_xdone();}"
" if(!r[0])return null;var b=r[4]?__flopos_xrest(r[3]):r[3];if(b===null)throw new RangeError('the answer is more than the room holds');return [r[0],r[1],r[2],b];}"
"function MessageEvent(t,i){i=i||{};this.type=String(t);this.data=i.data;this.origin=i.origin||'';this.lastEventId='';this.source=null;this.ports=[];this.target=this.currentTarget=null;}"
"MessageEvent.prototype.preventDefault=MessageEvent.prototype.stopPropagation=function(){};g.MessageEvent=MessageEvent;"
"function sclone(d,seen){if(d===null||typeof d!=='object'){if(typeof d==='function'||typeof d==='symbol')throw new TypeError('could not be cloned');return d;}"
" seen=seen||new Map();if(seen.has(d))return seen.get(d);var o;"
" if(d instanceof Date)o=new Date(d.getTime());else if(d instanceof RegExp)o=new RegExp(d.source,d.flags);else if(d instanceof ArrayBuffer)o=d.slice(0);"
" else if(ArrayBuffer.isView(d))o=d.slice?d.slice():new DataView(d.buffer.slice(0),d.byteOffset,d.byteLength);else if(d instanceof Blob)o=d;"
" else if(d instanceof Map){o=new Map();seen.set(d,o);d.forEach(function(v,k){o.set(sclone(k,seen),sclone(v,seen));});return o;}"
" else if(d instanceof Set){o=new Set();seen.set(d,o);d.forEach(function(v){o.add(sclone(v,seen));});return o;}"
" else{o=Array.isArray(d)?[]:{};seen.set(d,o);for(var k in d)if(Object.prototype.hasOwnProperty.call(d,k))o[k]=sclone(d[k],seen);return o;}"
" seen.set(d,o);return o;}"
"g.structuredClone=function(v){return sclone(v);};"
"function evts(o){o.addEventListener=function(t,f){if(f)(this._ls[t]=this._ls[t]||[]).push(f);};o.removeEventListener=function(t,f){var l=this._ls[t];if(l)this._ls[t]=l.filter(function(x){return x!==f;});};"
" o._emit=function(e,on){e.target=e.currentTarget=on||this;var h=this['on'+e.type];if(typeof h==='function')h.call(on||this,e);(this._ls[e.type]||[]).slice().forEach(function(f){typeof f==='function'?f.call(on||this,e):f.handleEvent(e);},this);};"
" o.dispatchEvent=function(e){this._emit(e);return true;};}"
"var wk={};"
"function Worker(u,o){if(!(this instanceof Worker))throw new TypeError('Worker needs new');if(o&&o.type==='module')throw new TypeError('module Workers are not supported here');"
" var w=this,url=new URL(String(u),__flopos_href()).href,t;w._q=[];w._ls={};w._dead=0;w._ready=0;w._sc={_ls:{},get:null,self:null};"
" t=__flopos_dynask('\\u0001'+url,'');if(typeof t!=='number'||t<0)throw new TypeError('the Worker '+url+' was not allowed');"
" wk[t]=w;pend[t]={w:1,ok:function(){},no:function(e){w._fail(e);}};}"
"var WP=Worker.prototype;evts(WP);WP.onmessage=WP.onerror=WP.onmessageerror=null;"
"WP.postMessage=function(d){if(this._dead)return;var w=this,c=sclone(d);setTimeout(function(){w._in(c);},0);};"
"WP.terminate=function(){this._dead=1;this._q=[];};"
"WP._in=function(c){if(this._dead)return;if(!this._ready){this._q.push(c);return;}__flopos_long();var sc=this._sc,e=new MessageEvent('message',{data:c}),h=sc.get&&sc.get();"
" e.target=e.currentTarget=sc.self;try{if(typeof h==='function')h.call(sc.self,e);else if(typeof sc.self.onmessage==='function')sc.self.onmessage(e);"
"  (sc._ls.message||[]).slice().forEach(function(f){f.call(sc.self,e);});}catch(x){this._fail(x);}};"
"WP._fail=function(x){var e={type:'error',message:String(x&&x.message||x),filename:'',lineno:0,colno:0,error:x,preventDefault:function(){}};this._emit(e);};"
"g.Worker=Worker;"
"g.__flopos_wrun=function(t,fn){var w=wk[t],sc,self;if(!w)return;sc=w._sc;self={};sc.self=self;self.self=self;self.onmessage=self.onerror=null;"
" ['location','navigator','console','performance','crypto','fetch','XMLHttpRequest','setTimeout','setInterval','clearTimeout','clearInterval','queueMicrotask',"
"  'TextEncoder','TextDecoder','Blob','File','URL','URLSearchParams','atob','btoa','structuredClone','Headers','Request','Response','Promise'].forEach(function(n){if(g[n]!==undefined)self[n]=g[n];});"
" self.postMessage=function(d){if(w._dead)return;var c=sclone(d);setTimeout(function(){if(!w._dead)w._emit(new MessageEvent('message',{data:c}));},0);};"
" self.close=function(){w._dead=1;};self.importScripts=function(){throw new TypeError('importScripts is not supported here');};"
" self.addEventListener=function(ty,f){if(f)(sc._ls[ty]=sc._ls[ty]||[]).push(f);};self.removeEventListener=function(ty,f){var l=sc._ls[ty];if(l)sc._ls[ty]=l.filter(function(x){return x!==f;});};"
" try{sc.get=fn.call(self,self,self.postMessage,self.close,self.importScripts,null,null,self.addEventListener,self.removeEventListener);}catch(x){w._fail(x);return;}"
" w._ready=1;var q=w._q;w._q=[];q.forEach(function(c){w._in(c);});};"
"function Headers(h){this._l=[];if(h instanceof Headers)h=h._l;"
" if(h&&typeof h==='object'){if(Array.isArray(h)||typeof h[Symbol.iterator]==='function')Array.from(h).forEach(function(p){p=Array.from(p);if(p.length!==2)throw new TypeError('Each header needs a name and a value');this.append(p[0],p[1]);},this);"
"  else for(var k in h)if(Object.prototype.hasOwnProperty.call(h,k))this.append(k,h[k]);}}"
"var HP=Headers.prototype;"
"function hname(k){k=String(k).toLowerCase();if(!/^[!#$%&'*+.^_`|~0-9a-z-]+$/.test(k))throw new TypeError('Invalid header name: '+k);return k;}"
"HP.append=function(k,v){k=hname(k);v=String(v).trim();for(var i=0;i<this._l.length;i++)if(this._l[i][0]===k){this._l[i][1]+=', '+v;return;}this._l.push([k,v]);};"
"HP.set=function(k,v){k=hname(k);this['delete'](k);this._l.push([k,String(v).trim()]);};"
"HP.get=function(k){k=String(k).toLowerCase();for(var i=0;i<this._l.length;i++)if(this._l[i][0]===k)return this._l[i][1];return null;};"
"HP.has=function(k){return this.get(k)!==null;};"
"HP['delete']=function(k){k=String(k).toLowerCase();this._l=this._l.filter(function(p){return p[0]!==k;});};"
"HP.getSetCookie=function(){return [];};"
"HP._sorted=function(){return this._l.slice().sort(function(a,b){return a[0]<b[0]?-1:a[0]>b[0]?1:0;});};"
"HP.forEach=function(f,t){this._sorted().forEach(function(p){f.call(t,p[1],p[0],this);},this);};"
"HP.entries=HP[Symbol.iterator]=function(){return this._sorted().map(function(p){return [p[0],p[1]];}).values();};"
"HP.keys=function(){return this._sorted().map(function(p){return p[0];}).values();};"
"HP.values=function(){return this._sorted().map(function(p){return p[1];}).values();};"
"g.Headers=Headers;"
"function __flopos_heads(t,h){var o=new Headers();if(t)o.set('content-type',t);String(h||'').split('\\n').forEach(function(l){var i=l.indexOf(':');if(i>0&&l.slice(0,i).trim().toLowerCase()!=='content-type')try{o.append(l.slice(0,i).trim(),l.slice(i+1).trim());}catch(e){}});return o;}"
"function bodyOf(b){if(b==null)return ['',''];if(typeof b==='string')return [b,'text/plain;charset=UTF-8'];if(b instanceof Blob)return [blobBytes(b),b.type];if(b instanceof ArrayBuffer||ArrayBuffer.isView(b))return [bytesOf(b),''];"
" if(b instanceof URLSearchParams)return [b.toString(),'application/x-www-form-urlencoded;charset=UTF-8'];"
" if(typeof FormData!=='undefined'&&b instanceof FormData)return [b.toString(),'application/x-www-form-urlencoded'];"
" if(typeof b==='object'&&Object.getPrototypeOf(b)===Object.prototype)return [JSON.stringify(b),'text/plain;charset=UTF-8'];"
" return [String(b),'text/plain;charset=UTF-8'];}"
"function bodyRaw(r){if(r.bodyUsed)throw new TypeError('Body has already been read');r.bodyUsed=true;var b=r._b;r._b=null;return typeof b==='string'?new TextEncoder().encode(b):b||new Uint8Array(0);}"
"function bodyText(){try{var b=bodyRaw(this);return Promise.resolve(new TextDecoder().decode(b));}catch(e){return Promise.reject(e);}}"
"function bodyJson(){return bodyText.call(this).then(function(t){return JSON.parse(t);});}"
"function Response(b,i){i=i||{};var s=i.status===undefined?200:+i.status;if(s<200||s>599)throw new RangeError('Response status must be 200 to 599');"
" var x=bodyOf(b);this._b=x[0];this.status=s;this.statusText=i.statusText===undefined?'':String(i.statusText);this.headers=new Headers(i.headers);"
" if(b!=null&&x[1]&&!this.headers.has('content-type'))this.headers.set('content-type',x[1]);this.url='';this.redirected=false;this.type='default';this.bodyUsed=false;}"
"var RP=Response.prototype;RP.status=0;RP.statusText='';RP.url='';RP.headers=null;RP.redirected=false;RP.type='default';RP.bodyUsed=false;RP.body=null;"
"Object.defineProperty(RP,'ok',{get:function(){return this.status>=200&&this.status<300;},configurable:true});"
"RP.text=bodyText;RP.json=bodyJson;"
"function bodyBuf(){try{return Promise.resolve(bodyRaw(this).slice().buffer);}catch(e){return Promise.reject(e);}}"
"function bodyBytes(){try{return Promise.resolve(bodyRaw(this).slice());}catch(e){return Promise.reject(e);}}"
"function bodyBlob(){var t=this.headers&&this.headers.get('content-type')||'';try{return Promise.resolve(new Blob([bodyRaw(this)],{type:t}));}catch(e){return Promise.reject(e);}}"
"RP.arrayBuffer=bodyBuf;RP.bytes=bodyBytes;RP.blob=bodyBlob;"
"RP.clone=function(){if(this.bodyUsed)throw new TypeError('Body has already been read');var r=Object.create(RP);for(var k in this)if(Object.prototype.hasOwnProperty.call(this,k))r[k]=this[k];r.headers=new Headers(this.headers);return r;};"
"Response.json=function(d,i){i=i||{};var h=new Headers(i.headers);if(!h.has('content-type'))h.set('content-type','application/json');return new Response(JSON.stringify(d),{status:i.status,statusText:i.statusText,headers:h});};"
"Response.error=function(){var r=Object.create(RP);r._b='';r.status=0;r.headers=new Headers();r.type='error';return r;};"
"Response.redirect=function(u,s){var r=new Response(null,{status:s||302});r.headers.set('location',new URL(u,__flopos_href()).href);return r;};"
"function __resp(st,t,b,u,h){var r=Object.create(RP);r._b=b;r.status=st;r.statusText=st===200?'OK':'';r.headers=__flopos_heads(t,h);r.url=u;r.type='basic';r.redirected=false;r.bodyUsed=false;return r;}"
"g.Response=Response;"
"function Request(u,i){i=i||{};if(u instanceof Request){this.url=u.url;this.method=u.method;this.headers=new Headers(u.headers);this._b=u._b;this.signal=u.signal;}"
" else{this.url=new URL(String(u),__flopos_href()).href;this.method='GET';this.headers=new Headers();this._b='';this.signal=null;}"
" if(i.method)this.method=String(i.method).toUpperCase();if(i.headers)this.headers=new Headers(i.headers);if(i.signal)this.signal=i.signal;"
" if(i.body!=null){if(this.method==='GET'||this.method==='HEAD')throw new TypeError('A GET or HEAD request has no body');var x=bodyOf(i.body);this._b=x[0];if(x[1]&&!this.headers.has('content-type'))this.headers.set('content-type',x[1]);}"
" this.credentials=i.credentials||'same-origin';this.mode=i.mode||'cors';this.cache=i.cache||'default';this.redirect=i.redirect||'follow';this.bodyUsed=false;}"
"var QR=Request.prototype;QR.url='';QR.method='GET';QR.headers=null;QR.signal=null;QR.credentials='same-origin';QR.mode='cors';QR.cache='default';QR.redirect='follow';"
"QR.referrer='about:client';QR.referrerPolicy='';QR.integrity='';QR.keepalive=false;QR.destination='';QR.bodyUsed=false;QR.body=null;QR.duplex='half';QR.isHistoryNavigation=false;"
"QR.text=bodyText;QR.json=bodyJson;QR.arrayBuffer=bodyBuf;QR.bytes=bodyBytes;QR.blob=bodyBlob;QR.clone=function(){return new Request(this);};"
"g.Request=Request;"
"g.fetch=function(u,o){var q;try{q=new Request(u,o);}catch(e){return Promise.reject(e);}"
" return new Promise(function(ok,no){var s=q.signal;if(s&&s.aborted){no(s.reason);return;}"
"  var r;try{r=__flopos_sync(q.method,q.url,q.headers.get('content-type')||'',q._b);}catch(e){no(e);return;}if(!r){no(new TypeError('Failed to fetch'));return;}ok(__resp(r[0],r[1],r[3],q.url,r[2]));});};"
"function ProgressEvent(t,i){i=i||{};this.type=String(t);this.lengthComputable=!!i.lengthComputable;this.loaded=i.loaded||0;this.total=i.total||0;this.target=this.currentTarget=null;}"
"var PE=ProgressEvent.prototype;PE.lengthComputable=false;PE.loaded=0;PE.total=0;PE.preventDefault=PE.stopPropagation=function(){};g.ProgressEvent=ProgressEvent;"
"function xon(o){o.addEventListener=function(t,f){if(typeof f==='function'||(f&&f.handleEvent))(this._ls[t]=this._ls[t]||[]).push(f);};"
" o.removeEventListener=function(t,f){var l=this._ls[t];if(l)this._ls[t]=l.filter(function(x){return x!==f;});};"
" o._fire=function(t,n){var e=new ProgressEvent(t,{lengthComputable:n>0,loaded:n,total:n});e.target=e.currentTarget=this;if(typeof this['on'+t]==='function')this['on'+t](e);"
"  (this._ls[t]||[]).slice().forEach(function(f){if(typeof f==='function')f.call(this,e);else f.handleEvent(e);},this);};"
" o.dispatchEvent=function(e){this._fire(e.type,0);return true;};}"
"function XMLHttpRequestUpload(){this._ls={};}xon(XMLHttpRequestUpload.prototype);g.XMLHttpRequestUpload=XMLHttpRequestUpload;"
"function XMLHttpRequest(){this._ls={};this._h={};this.upload=new XMLHttpRequestUpload();}"
"var XP=XMLHttpRequest.prototype;xon(XP);"
"['UNSENT','OPENED','HEADERS_RECEIVED','LOADING','DONE'].forEach(function(k,i){XMLHttpRequest[k]=XP[k]=i;});"
"var xd={readyState:0,status:0,statusText:'',responseText:'',response:'',responseType:'',responseURL:'',responseXML:null,timeout:0,withCredentials:false,upload:null};"
"for(var xk in xd)XP[xk]=xd[xk];"
"['readystatechange','loadstart','progress','load','error','abort','timeout','loadend'].forEach(function(t){XP['on'+t]=null;});"
"XP.open=function(m,u,a){this._m=String(m).toUpperCase();this._u=new URL(String(u),__flopos_href()).href;this._a=a!==false;this._h={};this._x=0;this.readyState=1;this.status=0;this.responseText='';this.response='';this._fire('readystatechange',0);};"
"XP.setRequestHeader=function(k,v){if(this.readyState!==1)throw new Error('InvalidStateError: open() it first');k=String(k).toLowerCase();this._h[k]=this._h[k]?this._h[k]+', '+v:String(v);};"
"XP.overrideMimeType=function(t){this._mt=String(t);};"
"XP.getResponseHeader=function(k){return this._hd?this._hd.get(k):null;};"
"XP.getAllResponseHeaders=function(){var s='';if(this._hd)this._hd.forEach(function(v,k){s+=k+': '+v+'\\r\\n';});return s;};"
"XP.abort=function(){this._x=1;};"
"XP.send=function(b){var x=this;if(x.readyState!==1)throw new Error('InvalidStateError: open() it first');"
" var bp=bodyParts(b,x._m),t=x._h['content-type']||bp[2],p=pieces(bp[0],16384),pi=0,sent=0,keep=[],kept=0,drop=false,loaded=0;"
" function next(f){if(x._a)setTimeout(f,0);else f();}"
" function take(u){if(!u||!u.length)return;loaded+=u.length;if(!drop){if(kept+u.length>524288){drop=true;keep=[];}else{keep.push(u);kept+=u.length;}}x._fire('progress',loaded);}"
" function stop(){__flopos_xend();keep=[];kept=0;x.readyState=4;x.status=0;x._fire('readystatechange',0);x._fire('abort',0);x._fire('loadend',0);x.readyState=0;}"
" function fail(){__flopos_xend();x.status=0;x.readyState=4;x._fire('readystatechange',0);x._fire('error',0);x._fire('loadend',0);}"
" function finish(){var all=joinBytes(keep.length?keep:[new Uint8Array(0)],kept),rt=x.responseType,s;"
"  if(drop){x.response=null;x.responseText='';}else if(rt==='arraybuffer')x.response=all.slice().buffer;else if(rt==='blob')x.response=new Blob([all],{type:x._hd.get('content-type')||''});"
"  else{s=new TextDecoder().decode(all);x.responseText=rt===''||rt==='text'?s:'';x.response=rt==='json'?(function(){try{return JSON.parse(s);}catch(e){return null;}})():rt===''||rt==='text'?s:null;}"
"  x.readyState=4;x._fire('readystatechange',0);x._fire(x.status?'load':'error',loaded);x._fire('loadend',loaded);}"
" function more(){if(x._x)return stop();var q=__flopos_xmore();take(q[0]);if(q[1])next(more);else finish();}"
" function got(r){if(!r[0])return fail();x._hd=__flopos_heads(r[1],r[2]);x.status=r[0];x.statusText=r[0]===200?'OK':'';x.responseURL=x._u;"
"  x.readyState=2;x._fire('readystatechange',0);x.readyState=3;x._fire('readystatechange',0);take(r[3]);if(r[4])next(more);else finish();}"
" function up(){if(x._x)return stop();if(pi<p.length){var u=p[pi++];if(!__flopos_xsend(u))return fail();sent+=u.length;x.upload._fire('progress',sent);next(up);return;}"
"  x.upload._fire('load',sent);x.upload._fire('loadend',sent);got(__flopos_xdone());}"
" function go(){if(x._x){x.readyState=4;x._fire('abort',0);x._fire('loadend',0);x.readyState=0;return;}"
"  var u=p[pi++]||new Uint8Array(0),r=__flopos_xopen(x._m,x._u,t,bp[1],u);sent=u.length;"
"  if(r[4]===2){x.upload._fire('loadstart',0);x.upload._fire('progress',sent);next(up);}else got(r);}"
" x._fire('loadstart',0);next(go);};"
"g.XMLHttpRequest=XMLHttpRequest;"
"function FormData(f){this._p=[];if(f&&f.querySelectorAll){var els=f.querySelectorAll('input,select,textarea');for(var i=0;i<els.length;i++){var el=els[i],n=el.name,t=(el.type||'').toLowerCase();if(!n||el.disabled)continue;if((t==='checkbox'||t==='radio')&&!el.checked)continue;if(t==='submit'||t==='button'||t==='reset'||t==='file'||t==='image')continue;this._p.push([n,String(el.value==null?'':el.value)]);}}}"
"['get','getAll','has','set','append','delete','forEach','toString'].forEach(function(k){FormData.prototype[k]=URLSearchParams.prototype[k];});"
"FormData.prototype.entries=function(){return this._p.map(function(p){return [p[0],p[1]];});};"
"FormData.prototype.keys=function(){return this._p.map(function(p){return p[0];});};"
"FormData.prototype.values=function(){return this._p.map(function(p){return p[1];});};"
"g.FormData=FormData;"
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
"g.__flopos_mresolve=function(s,u){s=String(s);if(/^[a-z][a-z0-9+.-]*:/i.test(s))return s;try{return new URL(s,u).href;}catch(e){throw new TypeError('cannot resolve '+s);}};"
"g.__flopos_import=function(spec,meta){return new Promise(function(ok,no){var t=__flopos_dynask(String(spec),meta&&meta.url?meta.url:'');"
" if(typeof t!=='number'||t<0){no(new TypeError('import() of '+spec+' was not allowed'));return;}pend[t]={ok:ok,no:no};});};"
"g.__flopos_dyndone=function(t,url,why){var p=pend[t];delete pend[t];if(!p)return;if(why)p.no(new TypeError(why));else if(!p.w)p.ok(__flopos_modns_url(url));};"
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
    def_native(vm, g, "__flopos_xopen", w_xopen, 5, 0);
    def_native(vm, g, "__flopos_xsend", w_xsend, 1, 0);
    def_native(vm, g, "__flopos_xdone", w_xdone, 0, 0);
    def_native(vm, g, "__flopos_xmore", w_xmore, 0, 0);
    def_native(vm, g, "__flopos_xend", w_xend, 0, 0);
    def_native(vm, g, "__flopos_xrest", w_xrest, 1, 0);
    def_native(vm, g, "__flopos_href", w_href, 0, 0);
    def_native(vm, g, "__flopos_go", w_go, 1, 0);
    def_native(vm, g, "__flopos_shown", w_shown, 1, 0);
    def_native(vm, g, "__flopos_now", w_now, 0, 0);
    def_native(vm, g, "__flopos_dynask", w_dynask, 2, 0);
    def_native(vm, g, "__flopos_modns_url", w_modns_url, 1, 0);
    def_val(vm, g, "__flopos_ua", v_str(atom("Mozilla/5.0 (compatible; FLOPOS; a 1996 Toshiba)")));
    for (i = 0; ons[i]; i++) def_accessor(vm, g, ons[i], on_get, on_set, 0);
}

/* An error no script caught, in a timer: on the wire, as browsers' consoles show it. */
static void dom_uncaught(Vm *vm, Val e)
{
    static char line[300];
    Str *s = to_str(vm, e);
    u32 n = 0;
    if (vm->throwing) { vm->throwing = 0; s = 0; }
    n = (u32)js_strlen("Uncaught ");
    memcpy(line, "Uncaught ", n);
    if (s) n += str_to_utf8(s, line + n, sizeof line - n - 1);
    rq(D_LOG); rq_u32(2); rq_bytes(line, n);
    knock(vm);
}

void host_init(Vm *vm)
{
    char why[200];
    defs(vm);
    vm->uncaught = dom_uncaught;
    if (engine_run(vm, s_prelude, (u32)js_strlen(s_prelude), "prelude", 5000, why, sizeof why) != ENGINE_OK) {
        rq(D_LOG); rq_u32(2); rq_cstr("the room's prelude failed: "); knock(vm);
        rq(D_LOG); rq_u32(2); rq_bytes(why, (u32)js_strlen(why)); knock(vm);
    }
}
