/* ===========================================================================
 * dom.c -- a page as a tree of numbered elements.
 *
 * While a page streams in, it is drawn straight away as before, and at
 * the same time built into a tree here: every element and stretch of text
 * a numbered node, with its parent, its children and its attributes.
 * Scripts and styles work on the tree; when they are done, the page is
 * drawn again from it (dom_render), through the same page reader, with
 * what is hidden left out and the styles turned into marks the reader
 * understands.
 *
 * Everything lives in fixed tables: 6144 nodes, their attributes, and one
 * pool of text.  A page that does not fit is drawn as it streamed in,
 * without styles or scripts.
 *
 * The tree is built the forgiving way browsers build it: unclosed
 * paragraphs and list items close themselves, stray end tags are ignored,
 * and <script>, <style>, <textarea> and <title> hold plain text.
 * ===========================================================================
 */
#include "dom.h"

Node g_node[DOM_NODES];
u32  g_nodes;
char g_pool[DOM_POOL];
u32  g_pool_used;
int  g_dom_full;
static Attr s_attr[DOM_ATTRS];
static u32  s_nattr;

/* ---- the pool ----------------------------------------------------------------- */
static u32 pool_put(const char *s, u32 n)
{
    u32 at = g_pool_used, i;
    if (g_pool_used + n + 1 > DOM_POOL) { g_dom_full = 1; return 0; }
    for (i = 0; i < n; i++) g_pool[at + i] = s[i];
    g_pool[at + n] = 0;
    g_pool_used += n + 1;
    return at;
}

static int eq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static int eqn(const char *a, const char *b, u32 n) { u32 i; for (i = 0; i < n; i++) if (a[i] != b[i]) return 0; return b[n] == 0; }
static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }

/* ---- tags --------------------------------------------------------------------- */
static const char *s_void[] = { "area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta",
    "source", "track", "wbr", "param", "keygen", 0 };
static const char *s_raw[] = { "script", "style", "textarea", "title", "xmp", "noembed", "noframes", 0 };
/* elements whose start closes an open <p> */
static const char *s_closes_p[] = { "address", "article", "aside", "blockquote", "div", "dl", "fieldset", "footer",
    "form", "h1", "h2", "h3", "h4", "h5", "h6", "header", "hr", "main", "nav", "ol", "p", "pre", "section",
    "table", "ul", "figure", "details", "menu", 0 };

static int in_list(const char *t, const char **l) { int i; for (i = 0; l[i]; i++) if (eq(t, l[i])) return 1; return 0; }

const char *dom_tag(u32 n) { return n && n < g_nodes && g_node[n].type == N_ELEM ? g_pool + g_node[n].name : ""; }

/* ---- nodes -------------------------------------------------------------------- */
static u32 node_new(int type)
{
    Node *x;
    if (g_nodes >= DOM_NODES) { g_dom_full = 1; return 0; }
    x = &g_node[g_nodes];
    memset(x, 0, sizeof *x);
    x->type = (u8)type;
    x->value = 0xFFFFFFFFu;
    return g_nodes++;
}

static void link_child(u32 p, u32 c, u32 before)
{
    Node *P = &g_node[p], *C = &g_node[c];
    C->parent = (u16)p;
    if (before && g_node[before].parent == p) {
        C->next = (u16)before;
        C->prev = g_node[before].prev;
        if (C->prev) g_node[C->prev].next = (u16)c; else P->first = (u16)c;
        g_node[before].prev = (u16)c;
        return;
    }
    C->prev = P->last;
    C->next = 0;
    if (P->last) g_node[P->last].next = (u16)c; else P->first = (u16)c;
    P->last = (u16)c;
}

static void unlink(u32 c)
{
    Node *C = &g_node[c];
    u32 p = C->parent;
    if (!p) return;
    if (C->prev) g_node[C->prev].next = C->next; else g_node[p].first = C->next;
    if (C->next) g_node[C->next].prev = C->prev; else g_node[p].last = C->prev;
    C->parent = C->prev = C->next = 0;
}

u32 dom_create(const char *tag)
{
    u32 n = node_new(N_ELEM), k;
    char t[32];
    if (!n) return 0;
    for (k = 0; tag[k] && k < 31; k++) t[k] = lower(tag[k]);
    t[k] = 0;
    g_node[n].name = pool_put(t, k);
    return g_dom_full ? 0 : n;
}

u32 dom_text_node(const char *t, u32 len)
{
    u32 n = node_new(N_TEXT);
    if (!n) return 0;
    g_node[n].name = pool_put(t, len);
    g_node[n].len = len;
    return g_dom_full ? 0 : n;
}

static int is_ancestor(u32 a, u32 n)
{
    int guard = 0;
    for (; n && guard++ < 4096; n = g_node[n].parent) if (n == a) return 1;
    return 0;
}

int dom_append(u32 parent, u32 child, u32 before)
{
    if (!parent || parent >= g_nodes || !child || child >= g_nodes || child == 1) return 0;
    if (g_node[parent].type != N_ELEM && g_node[parent].type != N_DOC) return 0;
    if (is_ancestor(child, parent)) return 0;            /* into itself: no */
    unlink(child);
    link_child(parent, child, before);
    return 1;
}

void dom_remove(u32 n) { if (n > 1 && n < g_nodes) unlink(n); }

/* cloneNode: the element, its attributes and what was typed into it, and
 * when deep, everything inside -- loose, with no parent.  What scripts
 * listen for is not copied, as in any browser. */
static u32 clone_rec(u32 n, int deep, int depth)
{
    const Node *x = &g_node[n];
    u32 c, k;
    u16 a;
    if (depth > 100 || g_dom_full) return 0;
    if (x->type == N_TEXT || x->type == N_COMMENT) {
        c = dom_text_node(g_pool + x->name, x->len);
        if (c) g_node[c].type = x->type;
        return c;
    }
    if (x->type != N_ELEM) return 0;
    c = dom_create(g_pool + x->name);
    if (!c) return 0;
    for (a = x->attr; a && !g_dom_full; a = s_attr[a].next)
        dom_set_attr(c, g_pool + s_attr[a].name, g_pool + s_attr[a].val, s_attr[a].vlen);
    if (x->flags & NF_VALUE) { g_node[c].value = x->value; g_node[c].vlen = x->vlen; g_node[c].flags |= NF_VALUE; }
    if (deep)
        for (k = x->first; k && !g_dom_full; k = g_node[k].next) {
            u32 cc = clone_rec(k, 1, depth + 1);
            if (cc) link_child(c, cc, 0);
        }
    return g_dom_full ? 0 : c;
}

u32 dom_clone(u32 n, int deep) { return n > 1 && n < g_nodes ? clone_rec(n, deep, 0) : 0; }

/* ---- attributes ---------------------------------------------------------------- */
static Attr *attr_find(u32 n, const char *name)
{
    u16 a;
    for (a = g_node[n].attr; a; a = s_attr[a].next)
        if (eq(g_pool + s_attr[a].name, name)) return &s_attr[a];
    return 0;
}

const char *dom_attr(u32 n, const char *name)
{
    Attr *a;
    if (!n || n >= g_nodes || g_node[n].type != N_ELEM) return 0;
    a = attr_find(n, name);
    return a ? g_pool + a->val : 0;
}

int dom_set_attr(u32 n, const char *name, const char *val, u32 vlen)
{
    Attr *a;
    char nm[48];
    u32 k;
    if (!n || n >= g_nodes || g_node[n].type != N_ELEM) return 0;
    for (k = 0; name[k] && k < 47; k++) nm[k] = lower(name[k]);
    nm[k] = 0;
    a = attr_find(n, nm);
    if (!a) {
        if (s_nattr + 1 >= DOM_ATTRS) { g_dom_full = 1; return 0; }
        if (!s_nattr) s_nattr = 1;
        a = &s_attr[s_nattr];
        a->name = pool_put(nm, k);
        a->next = 0;
        /* at the end, so attributes keep their order */
        if (!g_node[n].attr) g_node[n].attr = (u16)s_nattr;
        else { u16 x = g_node[n].attr; while (s_attr[x].next) x = s_attr[x].next; s_attr[x].next = (u16)s_nattr; }
        s_nattr++;
    }
    a->val = pool_put(val, vlen);
    a->vlen = vlen;
    return !g_dom_full;
}

void dom_del_attr(u32 n, const char *name)
{
    u16 *p;
    if (!n || n >= g_nodes) return;
    for (p = &g_node[n].attr; *p; p = &s_attr[*p].next)
        if (eq(g_pool + s_attr[*p].name, name)) { *p = s_attr[*p].next; return; }
}

int dom_has_class(u32 n, const char *cls, u32 len)
{
    const char *c = dom_attr(n, "class");
    u32 i = 0;
    if (!c || !len) return 0;
    while (c[i]) {
        u32 s;
        while (c[i] == ' ' || c[i] == '\t' || c[i] == '\n' || c[i] == '\r') i++;
        s = i;
        while (c[i] && c[i] != ' ' && c[i] != '\t' && c[i] != '\n' && c[i] != '\r') i++;
        if (i - s == len) { u32 k; for (k = 0; k < len && c[s + k] == cls[k]; k++) { } if (k == len) return 1; }
    }
    return 0;
}

/* ---- text --------------------------------------------------------------------------- */
static u32 text_into(u32 n, char *out, u32 cap, u32 at, int depth)
{
    u32 c;
    if (depth > 200) return at;
    if (g_node[n].type == N_TEXT) {
        u32 k;
        for (k = 0; k < g_node[n].len && at + 1 < cap; k++) out[at++] = g_pool[g_node[n].name + k];
        return at;
    }
    for (c = g_node[n].first; c; c = g_node[c].next) at = text_into(c, out, cap, at, depth + 1);
    return at;
}

u32 dom_text(u32 n, char *out, u32 cap)
{
    u32 r;
    if (!n || n >= g_nodes || !cap) return 0;
    r = text_into(n, out, cap, 0, 0);
    out[r] = 0;
    return r;
}

int dom_set_text(u32 n, const char *t, u32 len)
{
    u32 c;
    if (!n || n >= g_nodes) return 0;
    if (g_node[n].type == N_TEXT) {
        g_node[n].name = pool_put(t, len);
        g_node[n].len = len;
        return !g_dom_full;
    }
    while ((c = g_node[n].first)) unlink(c);
    if (!len) return 1;
    c = dom_text_node(t, len);
    if (!c) return 0;
    link_child(n, c, 0);
    return 1;
}

int dom_value(u32 n, char *out, u32 cap)
{
    u32 k, len;
    const char *v;
    if (!cap) return 0;
    out[0] = 0;
    if (!n || n >= g_nodes) return 0;
    if (g_node[n].flags & NF_VALUE) { v = g_pool + g_node[n].value; len = g_node[n].vlen; }
    else if (eq(dom_tag(n), "textarea")) return (int)dom_text(n, out, cap);
    else if (eq(dom_tag(n), "select")) {
        /* the chosen option's value */
        u32 c, first = 0;
        for (c = g_node[n].first; c; c = g_node[c].next) {
            if (!eq(dom_tag(c), "option")) continue;
            if (!first) first = c;
            if (dom_attr(c, "selected")) { first = c; break; }
        }
        if (!first) return 0;
        v = dom_attr(first, "value");
        if (!v) return (int)dom_text(first, out, cap);
        for (len = 0; v[len]; len++) { }
    } else { v = dom_attr(n, "value"); if (!v) return 0; for (len = 0; v[len]; len++) { } }
    for (k = 0; k < len && k + 1 < cap; k++) out[k] = v[k];
    out[k] = 0;
    return (int)k;
}

/* ---- the builder ------------------------------------------------------------------ */
enum { B_TEXT, B_TAGOPEN, B_TAGNAME, B_ATTRS, B_ANAME, B_AEQ, B_AVAL, B_QVAL, B_COMMENT, B_BANG, B_RAW, B_ENT, B_SELFCLOSE };

static int  s_state, s_closing, s_ent_from;
static char s_tname[32]; static u32 s_tlen;
static char s_aname[48]; static u32 s_alen;
static char s_aval[2048]; static u32 s_avlen;
static char s_quote;
static char s_ent[12]; static u32 s_entlen;
static char s_raw_tag[16];
static u32  s_raw_match;
static u32  s_cur;                       /* the element being added to      */
static u32  s_pending;                   /* the tag being read, made already */
static char s_text[1024]; static u32 s_textlen;
static int  s_cmt_dash;

static void flush_text(void)
{
    u32 p = s_cur, last, n;
    if (!s_textlen) return;
    last = g_node[p].last;
    if (last && g_node[last].type == N_TEXT && g_node[last].name + g_node[last].len + 1 == g_pool_used) {
        /* the same text goes on: grow it in place */
        u32 k;
        if (g_pool_used + s_textlen + 1 > DOM_POOL) { g_dom_full = 1; s_textlen = 0; return; }
        g_pool_used--;                   /* over its zero                   */
        for (k = 0; k < s_textlen; k++) g_pool[g_pool_used++] = s_text[k];
        g_pool[g_pool_used++] = 0;
        g_node[last].len += s_textlen;
    } else {
        n = dom_text_node(s_text, s_textlen);
        if (n) link_child(p, n, 0);
    }
    s_textlen = 0;
}

static void text_byte(char c)
{
    if (s_textlen == sizeof s_text) flush_text();
    s_text[s_textlen++] = c;
}

static void text_utf8(u32 cp)
{
    if (cp < 0x80) text_byte((char)cp);
    else if (cp < 0x800) { text_byte((char)(0xC0 | (cp >> 6))); text_byte((char)(0x80 | (cp & 0x3F))); }
    else if (cp < 0x10000) { text_byte((char)(0xE0 | (cp >> 12))); text_byte((char)(0x80 | ((cp >> 6) & 0x3F))); text_byte((char)(0x80 | (cp & 0x3F))); }
    else { text_byte((char)(0xF0 | (cp >> 18))); text_byte((char)(0x80 | ((cp >> 12) & 0x3F))); text_byte((char)(0x80 | ((cp >> 6) & 0x3F))); text_byte((char)(0x80 | (cp & 0x3F))); }
}

static void aval_utf8(u32 cp)
{
    char b[4]; int n = 0, i;
    if (cp < 0x80) b[n++] = (char)cp;
    else if (cp < 0x800) { b[n++] = (char)(0xC0 | (cp >> 6)); b[n++] = (char)(0x80 | (cp & 0x3F)); }
    else { b[n++] = (char)(0xE0 | (cp >> 12)); b[n++] = (char)(0x80 | ((cp >> 6) & 0x3F)); b[n++] = (char)(0x80 | (cp & 0x3F)); }
    for (i = 0; i < n; i++) if (s_avlen < sizeof s_aval - 1) s_aval[s_avlen++] = b[i];
}

static void close_to(const char *tag)
{
    u32 n;
    for (n = s_cur; n > 1; n = g_node[n].parent) {
        if (eq(dom_tag(n), tag)) { s_cur = g_node[n].parent ? g_node[n].parent : 1; return; }
    }
}

static int open_has(const char *tag, const char *stop)
{
    u32 n;
    for (n = s_cur; n > 1; n = g_node[n].parent) {
        if (eq(dom_tag(n), tag)) return 1;
        if (stop && eq(dom_tag(n), stop)) return 0;
    }
    return 0;
}

static void start_tag(void)
{
    u32 n;
    const char *t;
    s_tname[s_tlen] = 0;
    t = s_tname;
    flush_text();
    if (eq(t, "html")) { s_pending = dom_root(); return; }
    if (eq(t, "head") || eq(t, "body")) {
        u32 r = dom_find(t);
        if (r) { s_cur = r; s_pending = r; return; }
    }
    /* what a start tag closes on its own */
    if (in_list(t, s_closes_p) && open_has("p", "div")) close_to("p");
    if (eq(t, "li") && open_has("li", "ul") && open_has("li", "ol")) close_to("li");
    if ((eq(t, "dt") || eq(t, "dd")) && (open_has("dd", "dl") || open_has("dt", "dl"))) { close_to("dd"); close_to("dt"); }
    if (eq(t, "option") && open_has("option", "select")) close_to("option");
    if (eq(t, "tr") && open_has("tr", "table")) close_to("tr");
    if ((eq(t, "td") || eq(t, "th")) && (open_has("td", "tr") || open_has("th", "tr"))) { close_to("td"); close_to("th"); }
    n = dom_create(t);
    s_pending = n;
    if (!n) return;
    link_child(s_cur, n, 0);
    if (!in_list(t, s_void)) s_cur = n;
}

static void end_tag(void)
{
    s_tname[s_tlen] = 0;
    flush_text();
    if (eq(s_tname, "html") || eq(s_tname, "body") || eq(s_tname, "head")) {
        if (eq(s_tname, "head")) { u32 b = dom_find("body"); s_cur = b ? b : dom_root(); }
        return;
    }
    if (in_list(s_tname, s_void)) return;
    close_to(s_tname);
}

static void attr_done(void)
{
    if (!s_alen || !s_pending) { s_alen = s_avlen = 0; return; }
    s_aname[s_alen] = 0;
    if (!attr_find(s_pending, s_aname)) dom_set_attr(s_pending, s_aname, s_aval, s_avlen);
    s_alen = s_avlen = 0;
}

static void tag_done(int self_close)
{
    if (s_closing) end_tag();
    else {
        const char *t = s_tname;
        if (s_pending && in_list(t, s_raw) && !self_close) {
            u32 i;
            for (i = 0; t[i] && i < 15; i++) s_raw_tag[i] = t[i];
            s_raw_tag[i] = 0;
            s_raw_match = 0;
            s_state = B_RAW;
            return;
        }
        if (self_close && s_pending && s_cur == s_pending && !in_list(t, s_void)) s_cur = g_node[s_cur].parent;
    }
    s_state = B_TEXT;
}

u32 dom_root(void) { return g_nodes > 2 ? 2 : 0; }

u32 dom_find(const char *tag)
{
    u32 n;
    for (n = 2; n < g_nodes; n++) if (g_node[n].type == N_ELEM && eq(dom_tag(n), tag) && (n == 2 || g_node[n].parent)) return n;
    return 0;
}

void dom_begin(void)
{
    u32 h, b;
    g_nodes = 1;
    g_pool_used = 1;
    g_pool[0] = 0;
    s_nattr = 1;
    g_dom_full = 0;
    node_new(N_DOC);                     /* 1: the document                 */
    h = dom_create("html");              /* 2: <html>                       */
    link_child(1, h, 0);
    b = dom_create("head"); link_child(h, b, 0);
    b = dom_create("body"); link_child(h, b, 0);
    s_cur = b;
    s_state = B_TEXT;
    s_textlen = 0;
    s_pending = 0;
}

/* <head>'s elements go in the head while the body has nothing yet. */
static void head_or_body(void)
{
    static const char *head_only[] = { "title", "meta", "link", "style", "base", 0 };
    u32 body = dom_find("body"), head = dom_find("head");
    s_tname[s_tlen] = 0;
    if (s_cur == body && !g_node[body].first && in_list(s_tname, head_only)) s_cur = head;
    else if (s_cur == head && !in_list(s_tname, head_only) && !eq(s_tname, "script") && !eq(s_tname, "noscript")) s_cur = body;
}

void dom_byte(u8 b)
{
    char c = (char)b;
    if (g_dom_full) return;
    switch (s_state) {
    case B_TEXT:
        if (c == '<') { s_state = B_TAGOPEN; return; }
        if (c == '&') { s_state = B_ENT; s_entlen = 0; s_ent_from = B_TEXT; return; }
        if (c == '\r') return;
        text_byte(c);
        return;
    case B_ENT: {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (c == '#' && !s_entlen)) {
            if (s_entlen < sizeof s_ent - 1) { s_ent[s_entlen++] = c; return; }
        }
        s_ent[s_entlen] = 0;
        {
            u32 cp = s_entlen ? web_entity(s_ent) : 0;
            if (s_ent_from == B_TEXT) {
                if (cp) text_utf8(cp);
                else { u32 k; text_byte('&'); for (k = 0; k < s_entlen; k++) text_byte(s_ent[k]); if (c == ';') { text_byte(';'); } }
                s_state = B_TEXT;
                if (c != ';') dom_byte(b);
            } else {
                if (cp) aval_utf8(cp);
                else { u32 k; if (s_avlen < sizeof s_aval - 1) s_aval[s_avlen++] = '&'; for (k = 0; k < s_entlen && s_avlen < sizeof s_aval - 1; k++) s_aval[s_avlen++] = s_ent[k]; if (c == ';' && s_avlen < sizeof s_aval - 1) s_aval[s_avlen++] = ';'; }
                s_state = s_ent_from;
                if (c != ';') dom_byte(b);
            }
        }
        return;
    }
    case B_TAGOPEN:
        if (c == '!') { s_state = B_BANG; s_cmt_dash = 0; return; }
        if (c == '?') { s_state = B_COMMENT; s_cmt_dash = -1; return; }
        s_closing = c == '/';
        s_tlen = 0;
        s_pending = 0;
        if (s_closing) { s_state = B_TAGNAME; return; }
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) { text_byte('<'); s_state = B_TEXT; dom_byte(b); return; }
        s_state = B_TAGNAME;
        /* fall through */
    case B_TAGNAME:
        if (c == '>' || c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '/') {
            if (!s_tlen) { s_state = c == '>' ? B_TEXT : B_TAGNAME; return; }
            s_tname[s_tlen] = 0;
            if (!s_closing) { head_or_body(); start_tag(); }
            s_alen = s_avlen = 0;
            if (c == '>') { tag_done(0); return; }
            s_state = c == '/' && !s_closing ? B_SELFCLOSE : B_ATTRS;
            return;
        }
        if (s_tlen < sizeof s_tname - 1) s_tname[s_tlen++] = lower(c);
        return;
    case B_SELFCLOSE:
        if (c == '>') { tag_done(1); return; }
        s_state = B_ATTRS;
        /* fall through */
    case B_ATTRS:
        if (c == '>') { attr_done(); tag_done(0); return; }
        if (c == '/') { attr_done(); s_state = B_SELFCLOSE; return; }
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') return;
        s_alen = s_avlen = 0;
        s_state = B_ANAME;
        /* fall through */
    case B_ANAME:
        if (c == '=') { s_state = B_AEQ; return; }
        if (c == '>' || c == '/' || c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            attr_done();
            s_state = B_ATTRS;
            dom_byte(b);
            return;
        }
        if (s_alen < sizeof s_aname - 1) s_aname[s_alen++] = lower(c);
        return;
    case B_AEQ:
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') return;
        if (c == '"' || c == '\'') { s_quote = c; s_state = B_QVAL; return; }
        if (c == '>') { attr_done(); tag_done(0); return; }
        s_state = B_AVAL;
        /* fall through */
    case B_AVAL:
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { attr_done(); s_state = B_ATTRS; return; }
        if (c == '>') { attr_done(); tag_done(0); return; }
        if (c == '&') { s_state = B_ENT; s_entlen = 0; s_ent_from = B_AVAL; return; }
        if (s_avlen < sizeof s_aval - 1) s_aval[s_avlen++] = c;
        return;
    case B_QVAL:
        if (c == s_quote) { attr_done(); s_state = B_ATTRS; return; }
        if (c == '&') { s_state = B_ENT; s_entlen = 0; s_ent_from = B_QVAL; return; }
        if (s_avlen < sizeof s_aval - 1) s_aval[s_avlen++] = c;
        return;
    case B_BANG:
        if (c == '-' && s_cmt_dash >= 0) { if (++s_cmt_dash == 2) { s_state = B_COMMENT; s_cmt_dash = 0; } return; }
        s_cmt_dash = -1;                 /* <!DOCTYPE ...> and the like     */
        s_state = B_COMMENT;
        /* fall through */
    case B_COMMENT:
        if (s_cmt_dash < 0) { if (c == '>') s_state = B_TEXT; return; }
        if (c == '-') { s_cmt_dash++; return; }
        if (c == '>' && s_cmt_dash >= 2) { s_state = B_TEXT; return; }
        s_cmt_dash = 0;
        return;
    case B_RAW: {
        /* plain text until </tag> */
        static const char *end = "</";
        u32 tl;
        for (tl = 0; s_raw_tag[tl]; tl++) { }
        if (s_raw_match < 2) {
            if (c == end[s_raw_match]) { s_raw_match++; return; }
        } else if (s_raw_match < 2 + tl) {
            if (lower(c) == s_raw_tag[s_raw_match - 2]) { s_raw_match++; return; }
        } else if (c == '>' || c == ' ' || c == '\n' || c == '\t') {
            u32 k;
            (void)k;
            flush_text();
            s_tlen = 0;
            for (k = 0; s_raw_tag[k]; k++) s_tname[s_tlen++] = s_raw_tag[k];
            end_tag();
            s_state = c == '>' ? B_TEXT : B_COMMENT;
            s_cmt_dash = -1;
            return;
        }
        {   /* not the end after all: what was held back is text */
            u32 k;
            for (k = 0; k < s_raw_match; k++) text_byte(k < 2 ? end[k] : s_raw_tag[k - 2]);
            s_raw_match = 0;
            if (c == '<') { s_raw_match = 1; return; }
            text_byte(c);
        }
        return;
    }
    }
}

void dom_end(void)
{
    if (s_state == B_TEXT) flush_text();
    s_state = B_TEXT;
}

u32 dom_by_id(const char *id)
{
    u32 n;
    for (n = 2; n < g_nodes; n++) {
        const char *v;
        if (g_node[n].type != N_ELEM || !g_node[n].parent) continue;
        v = dom_attr(n, "id");
        if (v && eq(v, id) && is_ancestor(1, n)) return n;
    }
    return 0;
}

/* ---- innerHTML ------------------------------------------------------------------------ */
int dom_set_html(u32 n, const char *html, u32 len)
{
    u32 keep_cur = s_cur, keep_state = (u32)s_state, c, i;
    if (!n || n >= g_nodes || g_node[n].type != N_ELEM) return 0;
    while ((c = g_node[n].first)) unlink(c);
    s_cur = n;
    s_state = B_TEXT;
    s_textlen = 0;
    for (i = 0; i < len && !g_dom_full; i++) dom_byte((u8)html[i]);
    if (s_state == B_TEXT) flush_text();
    s_cur = keep_cur;
    s_state = (int)keep_state;
    return !g_dom_full;
}

static u32 put(char *o, u32 at, u32 cap, const char *s, u32 n)
{
    u32 i;
    for (i = 0; i < n && at + 1 < cap; i++) o[at++] = s[i];
    return at;
}
static u32 puts_(char *o, u32 at, u32 cap, const char *s) { u32 n = 0; while (s[n]) n++; return put(o, at, cap, s, n); }

static u32 html_of(u32 n, char *o, u32 at, u32 cap, int outer, int depth)
{
    u32 c;
    if (depth > 200 || at + 1 >= cap) return at;
    if (g_node[n].type == N_TEXT) {
        u32 k;
        for (k = 0; k < g_node[n].len && at + 6 < cap; k++) {
            char ch = g_pool[g_node[n].name + k];
            if (ch == '<') at = puts_(o, at, cap, "&lt;");
            else if (ch == '&') at = puts_(o, at, cap, "&amp;");
            else o[at++] = ch;
        }
        return at;
    }
    if (g_node[n].type != N_ELEM) return at;
    if (outer) {
        u16 a;
        at = put(o, at, cap, "<", 1);
        at = puts_(o, at, cap, dom_tag(n));
        for (a = g_node[n].attr; a; a = s_attr[a].next) {
            u32 k;
            at = put(o, at, cap, " ", 1);
            at = puts_(o, at, cap, g_pool + s_attr[a].name);
            at = put(o, at, cap, "=\"", 2);
            for (k = 0; k < s_attr[a].vlen && at + 7 < cap; k++) {
                char ch = g_pool[s_attr[a].val + k];
                if (ch == '"') at = puts_(o, at, cap, "&quot;");
                else if (ch == '&') at = puts_(o, at, cap, "&amp;");
                else o[at++] = ch;
            }
            at = put(o, at, cap, "\"", 1);
        }
        at = put(o, at, cap, ">", 1);
        if (in_list(dom_tag(n), s_void)) return at;
    }
    for (c = g_node[n].first; c; c = g_node[c].next) at = html_of(c, o, at, cap, 1, depth + 1);
    if (outer) {
        at = put(o, at, cap, "</", 2);
        at = puts_(o, at, cap, dom_tag(n));
        at = put(o, at, cap, ">", 1);
    }
    return at;
}

u32 dom_html(u32 n, char *out, u32 cap, int outer)
{
    u32 r;
    if (!n || n >= g_nodes || !cap) return 0;
    r = html_of(n, out, 0, cap, outer, 0);
    out[r] = 0;
    return r;
}

/* ---- selectors -------------------------------------------------------------------------- */
/* A selector, compiled: its compounds from right to left (p[0] is the
 * element itself), each saying what it needs and how it joins the one to
 * its left.  Names and values live in one small pool. */
char g_selpool[65000];
u32  g_selpool_used = 1;

static u16 selpool(const char *s, u32 n, int low)
{
    u32 at = g_selpool_used, i;
    if (g_selpool_used + n + 1 > sizeof g_selpool) return 0;
    for (i = 0; i < n; i++) g_selpool[at + i] = low ? lower(s[i]) : s[i];
    g_selpool[at + n] = 0;
    g_selpool_used += n + 1;
    return (u16)at;
}

static int ident_c(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || (u8)c >= 0x80 || c == '\\'; }
static void ws_skip(const char *s, u32 len, u32 *i) { while (*i < len && (s[*i] == ' ' || s[*i] == '\n' || s[*i] == '\t' || s[*i] == '\r')) (*i)++; }
static void paren_skip(const char *s, u32 len, u32 *i)
{
    int depth = 0;
    if (*i < len && s[*i] == '(') { depth = 1; (*i)++; }
    while (*i < len && depth) { if (s[*i] == '(') depth++; if (s[*i] == ')') depth--; (*i)++; }
}

/* One compound (div.a#b[x=y]:first-child); i moves past it.  -1: one
 * that can never be a page's text (::before). */
static int compound(const char *s, u32 len, u32 *i, Part *p, int *spec)
{
    int any = 0;
    memset(p, 0, sizeof *p);
    if (*i < len && s[*i] == '*') { (*i)++; any = 1; }
    else if (*i < len && ident_c(s[*i])) {
        u32 st = *i;
        while (*i < len && ident_c(s[*i])) (*i)++;
        p->tag = selpool(s + st, *i - st, 1);
        *spec += 1;
        any = 1;
    }
    while (*i < len) {
        char c = s[*i];
        u32 st;
        if (c == '#') {
            (*i)++; st = *i;
            while (*i < len && ident_c(s[*i])) (*i)++;
            p->id = selpool(s + st, *i - st, 0);
            *spec += 100;
        } else if (c == '.') {
            (*i)++; st = *i;
            while (*i < len && ident_c(s[*i])) (*i)++;
            if (p->ncls < 3 && *i > st) { p->cls[p->ncls] = selpool(s + st, *i - st, 0); p->clen[p->ncls] = (u8)(*i - st > 255 ? 255 : *i - st); p->ncls++; }
            *spec += 10;
        } else if (c == '[') {
            int k = p->nat < 2 ? p->nat++ : -1;
            (*i)++;
            ws_skip(s, len, i);
            st = *i;
            while (*i < len && ident_c(s[*i])) (*i)++;
            if (k >= 0) { p->an[k] = selpool(s + st, *i - st, 1); p->aop[k] = 0; }
            ws_skip(s, len, i);
            if (*i < len && s[*i] != ']') {
                char op = s[*i];
                if (op != '=') (*i)++;
                if (*i < len && s[*i] == '=') (*i)++;
                ws_skip(s, len, i);
                if (k >= 0) p->aop[k] = (u8)op;
                if (*i < len && (s[*i] == '"' || s[*i] == '\'')) {
                    char q = s[(*i)++];
                    st = *i;
                    while (*i < len && s[*i] != q) (*i)++;
                    if (k >= 0) { p->av[k] = selpool(s + st, *i - st, 0); p->alen[k] = (u8)(*i - st > 255 ? 255 : *i - st); }
                    if (*i < len) (*i)++;
                } else {
                    st = *i;
                    while (*i < len && s[*i] != ']' && s[*i] != ' ') (*i)++;
                    if (k >= 0) { p->av[k] = selpool(s + st, *i - st, 0); p->alen[k] = (u8)(*i - st > 255 ? 255 : *i - st); }
                }
                while (*i < len && s[*i] != ']') (*i)++;
            }
            if (*i < len) (*i)++;
            *spec += 10;
        } else if (c == ':') {
            char nm[24];
            u32 k = 0;
            (*i)++;
            if (*i < len && s[*i] == ':') return -1;
            while (*i < len && ident_c(s[*i])) { if (k < 23) nm[k++] = lower(s[*i]); (*i)++; }
            nm[k] = 0;
            *spec += 10;
            if (eq(nm, "before") || eq(nm, "after") || eq(nm, "first-line") || eq(nm, "first-letter")) return -1;
            if (eq(nm, "first-child") || eq(nm, "first-of-type")) p->pseudo |= PS_FIRST;
            else if (eq(nm, "last-child") || eq(nm, "last-of-type")) p->pseudo |= PS_LAST;
            else if (eq(nm, "only-child")) p->pseudo |= PS_FIRST | PS_LAST;
            else if (eq(nm, "root")) p->pseudo |= PS_ROOT;
            else if (eq(nm, "empty")) p->pseudo |= PS_EMPTY;
            else if (eq(nm, "checked")) p->pseudo |= PS_CHECKED;
            else if (eq(nm, "disabled")) p->pseudo |= PS_DISABLED;
            else if (eq(nm, "enabled")) p->pseudo |= PS_ENABLED;
            else if (eq(nm, "link") || eq(nm, "any-link") || eq(nm, "is") || eq(nm, "where")) { if (eq(nm, "is") || eq(nm, "where")) { p->pseudo |= PS_NEVER; paren_skip(s, len, i); } }
            else if (eq(nm, "nth-child") || eq(nm, "nth-of-type")) {
                s32 a = 0, b = 0, sign = 1, num = 0, have = 0;
                p->pseudo |= PS_NTH;
                if (*i < len && s[*i] == '(') (*i)++;
                ws_skip(s, len, i);
                if (*i + 2 < len && s[*i] == 'o' && s[*i + 1] == 'd') { a = 2; b = 1; }
                else if (*i + 3 < len && s[*i] == 'e' && s[*i + 1] == 'v') { a = 2; b = 0; }
                else {
                    while (*i < len && s[*i] != ')') {
                        char d = s[*i];
                        if (d == '-') sign = -1;
                        else if (d == '+') sign = 1;
                        else if (d >= '0' && d <= '9') { num = num * 10 + (d - '0'); have = 1; }
                        else if (d == 'n') { a = have ? sign * num : sign; num = 0; have = 0; sign = 1; }
                        (*i)++;
                    }
                    b = sign * num;
                }
                p->nth_a = (s8)a; p->nth_b = (s8)b;
                while (*i < len && s[*i] != ')') (*i)++;
                if (*i < len) (*i)++;
            } else if (eq(nm, "not")) {
                u32 j = *i;
                p->pseudo |= PS_NOT;
                if (j < len && s[j] == '(') j++;
                ws_skip(s, len, &j);
                if (j < len && (s[j] == '.' || s[j] == '#')) { p->notk = (u8)s[j]; j++; }
                else p->notk = 't';
                st = j;
                while (j < len && ident_c(s[j])) j++;
                p->notv = selpool(s + st, j - st, p->notk == 't');
                p->notl = (u8)(j - st > 255 ? 255 : j - st);
                paren_skip(s, len, i);
            } else {
                /* :hover :focus :active :visited and the rest: never, so menus
                 * that open on hover stay closed */
                p->pseudo |= PS_NEVER;
                if (*i < len && s[*i] == '(') paren_skip(s, len, i);
            }
        } else break;
        any = 1;
    }
    return any ? 1 : 0;
}

int sel_parse(const char *s, u32 len, Part *out, int max, int *specificity)
{
    Part parts[SEL_PARTS];
    u32 i = 0;
    int n = 0, k, spec = 0;
    ws_skip(s, len, &i);
    for (;;) {
        int r;
        u8 comb = C_DESC;
        if (n >= SEL_PARTS || n >= max) return 0;
        r = compound(s, len, &i, &parts[n], &spec);
        if (r <= 0) return 0;
        n++;
        {
            u32 before = i;
            ws_skip(s, len, &i);
            if (i >= len) break;
            if (s[i] == '>') comb = C_CHILD;
            else if (s[i] == '+') comb = C_ADJ;
            else if (s[i] == '~') comb = C_SIB;
            else if (i == before) return 0;
            if (comb != C_DESC) { i++; ws_skip(s, len, &i); }
        }
        parts[n - 1].comb = comb;
    }
    for (k = 0; k < n; k++) {
        out[k] = parts[n - 1 - k];
        out[k].comb = k + 1 < n ? parts[n - 2 - k].comb : C_NONE;
    }
    if (specificity) *specificity = spec;
    return n;
}

static int attr_test(u32 n, const Part *p, int k)
{
    const char *v = dom_attr(n, g_selpool + p->an[k]), *w = g_selpool + p->av[k];
    u32 vl = 0, j, L = p->alen[k];
    if (!v) return 0;
    if (!p->aop[k]) return 1;
    while (v[vl]) vl++;
    switch (p->aop[k]) {
    case '=': return vl == L && eqn(v, w, L);
    case '~': {
        u32 i = 0;
        while (i < vl) {
            u32 st;
            while (i < vl && v[i] == ' ') i++;
            st = i;
            while (i < vl && v[i] != ' ') i++;
            if (i - st == L) { for (j = 0; j < L && v[st + j] == w[j]; j++) { } if (j == L) return 1; }
        }
        return 0;
    }
    case '^': if (vl < L) return 0; for (j = 0; j < L; j++) if (v[j] != w[j]) return 0; return 1;
    case '$': if (vl < L) return 0; for (j = 0; j < L; j++) if (v[vl - L + j] != w[j]) return 0; return 1;
    case '*': {
        u32 i;
        if (!L) return 0;
        for (i = 0; i + L <= vl; i++) { for (j = 0; j < L && v[i + j] == w[j]; j++) { } if (j == L) return 1; }
        return 0;
    }
    case '|': if (vl < L) return 0; for (j = 0; j < L; j++) if (v[j] != w[j]) return 0; return vl == L || v[L] == '-';
    }
    return 0;
}

static u32 prev_el(u32 n) { for (n = g_node[n].prev; n && g_node[n].type != N_ELEM; n = g_node[n].prev) { } return n; }
static u32 next_el(u32 n) { for (n = g_node[n].next; n && g_node[n].type != N_ELEM; n = g_node[n].next) { } return n; }

static int part_match(const Part *p, u32 n)
{
    u32 i;
    const char *t;
    if (!n || n >= g_nodes || g_node[n].type != N_ELEM) return 0;
    t = dom_tag(n);
    if (p->tag && !eq(g_selpool + p->tag, t)) return 0;
    if (p->id) { const char *id = dom_attr(n, "id"); if (!id || !eq(id, g_selpool + p->id)) return 0; }
    for (i = 0; i < p->ncls; i++) if (!dom_has_class(n, g_selpool + p->cls[i], p->clen[i])) return 0;
    for (i = 0; i < p->nat; i++) if (!attr_test(n, p, (int)i)) return 0;
    if (p->pseudo) {
        if (p->pseudo & PS_NEVER) return 0;
        if ((p->pseudo & PS_FIRST) && prev_el(n)) return 0;
        if ((p->pseudo & PS_LAST) && next_el(n)) return 0;
        if ((p->pseudo & PS_ROOT) && n != dom_root()) return 0;
        if ((p->pseudo & PS_EMPTY) && g_node[n].first) return 0;
        if ((p->pseudo & PS_CHECKED) && !dom_attr(n, "checked") && !dom_attr(n, "selected")) return 0;
        if ((p->pseudo & PS_DISABLED) && !dom_attr(n, "disabled")) return 0;
        if ((p->pseudo & PS_ENABLED) && dom_attr(n, "disabled")) return 0;
        if (p->pseudo & PS_NTH) {
            s32 idx = 1, a = p->nth_a, b = p->nth_b;
            u32 x;
            for (x = prev_el(n); x; x = prev_el(x)) idx++;
            if (a == 0) { if (idx != b) return 0; }
            else if ((idx - b) % a != 0 || (idx - b) / a < 0) return 0;
        }
        if (p->pseudo & PS_NOT) {
            if (p->notk == '.' && dom_has_class(n, g_selpool + p->notv, p->notl)) return 0;
            if (p->notk == 't' && p->notl && eq(g_selpool + p->notv, t)) return 0;
            if (p->notk == '#') { const char *id = dom_attr(n, "id"); if (id && eq(id, g_selpool + p->notv)) return 0; }
        }
    }
    return 1;
}

static int match_from(const Part *s, int count, int k, u32 n)
{
    const Part *p = &s[k];
    if (!part_match(p, n)) return 0;
    if (k + 1 >= count) return 1;
    switch (p->comb) {
    case C_CHILD: return match_from(s, count, k + 1, g_node[n].parent);
    case C_ADJ: return match_from(s, count, k + 1, prev_el(n));
    case C_SIB: { u32 x; for (x = prev_el(n); x; x = prev_el(x)) if (match_from(s, count, k + 1, x)) return 1; return 0; }
    default: { u32 x; int guard = 0; for (x = g_node[n].parent; x > 1 && guard++ < 256; x = g_node[x].parent) if (match_from(s, count, k + 1, x)) return 1; return 0; }
    }
}

int sel_match(const Part *s, int count, u32 n) { return count > 0 && match_from(s, count, 0, n); }

/* querySelector(All): in document order below `from`, comma lists too. */
int dom_query(u32 from, const char *sel, u32 len, u16 *out, int max)
{
    static Part list[8][SEL_PARTS];
    int nn[8], nsel = 0, found = 0;
    u32 i = 0, keep = g_selpool_used;
    while (i < len && nsel < 8) {
        u32 st = i;
        int depth = 0;
        while (i < len && !(sel[i] == ',' && !depth)) { if (sel[i] == '(' || sel[i] == '[') depth++; if (sel[i] == ')' || sel[i] == ']') depth--; i++; }
        nn[nsel] = sel_parse(sel + st, i - st, list[nsel], SEL_PARTS, 0);
        if (!nn[nsel]) { g_selpool_used = keep; return -1; }
        nsel++;
        if (i < len) i++;
    }
    if (from && from < g_nodes) {
        u32 n = g_node[from].first;
        int guard = 0;
        while (n && found < max && guard++ < DOM_NODES * 2) {
            if (g_node[n].type == N_ELEM) {
                int k;
                for (k = 0; k < nsel; k++) if (sel_match(list[k], nn[k], n)) { out[found++] = (u16)n; break; }
            }
            if (g_node[n].first) n = g_node[n].first;
            else {
                while (n && n != from && !g_node[n].next) n = g_node[n].parent;
                if (!n || n == from) break;
                n = g_node[n].next;
            }
        }
    }
    g_selpool_used = keep;
    return found;
}

/* ---- drawing, through the page reader --------------------------------------------------- */
static void out(const char *s) { while (*s) web_html_byte((u8)*s++); }
static void out_num(u32 v) { char b[12]; int k = 0; do { b[k++] = (char)('0' + v % 10); v /= 10; } while (v); while (k) web_html_byte((u8)b[--k]); }
static void out_attr(const char *name, const char *v, u32 vlen)
{
    u32 k;
    web_html_byte(' '); out(name); out("=\"");
    for (k = 0; k < vlen; k++) {
        char c = v[k];
        if (c == '"') out("&quot;");
        else if (c == '&') out("&amp;");
        else web_html_byte((u8)c);
    }
    web_html_byte('"');
}

static int drawn_block(const char *t)
{
    static const char *b[] = { "p", "div", "tr", "ul", "ol", "table", "section", "article", "header", "footer", "nav",
        "pre", "blockquote", "dt", "dd", "form", "main", "center", "aside", "figure", "li", "h1", "h2", "h3", "h4",
        "h5", "h6", "hr", "br", "address", "details", "summary", "dl", "fieldset", "figcaption", "menu", 0 };
    return in_list(t, b);
}

static const char *s_keep_attrs[] = { "href", "action", "method", "type", "name", "value", "checked", "selected",
    "size", "rows", "cols", "alt", "width", "height", "src", "maxlength", "multiple", 0 };

/* A player: an element that carries a sound's address in an attribute
 * (radio pages put it in url="...", data-stream="..." and the like, for a
 * script to play), or <audio>/<video> with a source.  Returns the address,
 * or 0.  browzer makes it a link where it stands, so the page's own
 * "Listen live" button plays, scripts or not. */
static int has_ext(const char *v, const char **ext)
{
    int n = 0, k, e;
    while (v[n] && v[n] != '?' && v[n] != '#') n++;
    for (k = 0; ext[k]; k++) {
        for (e = 0; ext[k][e]; e++) { }
        if (n > e) { int i; for (i = 0; i < e && lower(v[n - e + i]) == ext[k][i]; i++) { } if (i == e) return 1; }
    }
    return 0;
}

static const char *player_of(u32 n)
{
    static const char *names[] = { "url", "data-url", "data-stream", "data-stream-url", "data-audio",
        "data-mp3", "data-radio", "stream", "data-src-audio", "data-listen", 0 };
    static const char *ext[] = { ".mp3", ".aac", ".aacp", ".ogg", ".oga", ".opus", ".flac", ".m3u", ".m3u8", ".pls", 0 };
    const char *t = dom_tag(n), *v;
    int i;
    if (eq(t, "audio") || eq(t, "video")) {
        u32 c;
        if ((v = dom_attr(n, "src")) && v[0]) return v;
        for (c = g_node[n].first; c; c = g_node[c].next)
            if (eq(dom_tag(c), "source") && (v = dom_attr(c, "src")) && v[0]) return v;
        return 0;
    }
    if (eq(t, "a") || eq(t, "img") || eq(t, "script") || eq(t, "link") || eq(t, "source")) return 0;
    for (i = 0; names[i]; i++) {
        v = dom_attr(n, names[i]);
        if (v && (lower(v[0]) == 'h' || v[0] == '/') && (v[0] == '/' || (lower(v[1]) == 't' && lower(v[2]) == 't' && lower(v[3]) == 'p'))) return v;
    }
    if ((v = dom_attr(n, "data-src")) && has_ext(v, ext)) return v;
    return 0;
}

/* A "Listen live" (or "Play") that only a script makes play: its whole
 * text is one of these.  browzer makes it a link to the first stream the
 * page names. */
static int listen_words(u32 n)
{
    static const char *says[] = { "listen live", "listen now", "listen", "play", "play live", "play now",
        "listen online", "listen to the radio", "listen to live radio", "live stream", "play stream", 0 };
    char b[48], w[48];
    u32 len = dom_text(n, b, sizeof b), i, k = 0;
    int j, sp = 0;
    if (len == 0 || len >= sizeof b - 1) return 0;
    for (i = 0; i < len && k < sizeof w - 1; i++) {
        char ch = lower(b[i]);
        if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) { if (sp && k) w[k++] = ' '; sp = 0; w[k++] = ch; }
        else sp = 1;
    }
    w[k] = 0;
    for (j = 0; says[j]; j++) if (eq(w, says[j])) return 1;
    return 0;
}
static int s_playwrap;                   /* inside one already             */

static int in_link(u32 n)
{
    for (n = g_node[n].parent; n > 1; n = g_node[n].parent) if (eq(dom_tag(n), "a")) return 1;
    return 0;
}

/* ---- tables --------------------------------------------------------------------------
 * A table laid out as a table: its cells side by side in columns, rather
 * than run together one after another.  The idea of fitting CSS's tables
 * to a grid of characters, rounding every size to whole cells, comes from
 * Chawan, a text-mode browser by bptato (sr.ht/~bptato/chawan); this is
 * written from scratch, and much smaller.
 *
 * Each column is measured: the longest word in it (narrower than that,
 * words break) and its longest line (wider than that, nothing gains).
 * The page's width is shared out between those two, the extra going
 * where it is wanted most.  The reader (web.c) then lays out each cell at
 * its column's width and puts the cells of a row side by side.
 *
 * A table that would not fit -- too many columns, a cell holding a whole
 * article, a table inside a table -- is drawn as before, one cell after
 * another. */
#define TAB_COLS  12
#define TAB_ROWS  400
#define TAB_CELL  3000                   /* letters in one cell, at most     */
#define TAB_GAP   2

static int s_in_table, s_page_w;

typedef struct { u32 len, word, cur; int sp; } Measure;

static void measure(u32 n, Measure *m, int depth)
{
    u32 c;
    const char *t;
    if (depth > 60 || m->len > TAB_CELL) return;
    if (g_node[n].type == N_TEXT) {
        u32 k;
        for (k = 0; k < g_node[n].len; k++) {
            char ch = g_pool[g_node[n].name + k];
            if (ch == ' ' || ch == '\n' || ch == '\t' || ch == '\r') { if (!m->sp && m->len) { m->len++; m->sp = 1; } m->cur = 0; }
            else if (((u8)ch & 0xC0) != 0x80) { m->len++; m->sp = 0; if (++m->cur > m->word) m->word = m->cur; }
        }
        return;
    }
    if (g_node[n].type != N_ELEM || (g_node[n].flags & NF_HIDDEN)) return;
    t = dom_tag(n);
    if (eq(t, "script") || eq(t, "style") || eq(t, "template") || eq(t, "head")) return;
    if (eq(t, "table")) { m->len += TAB_CELL; return; }   /* a table in a table: not laid out */
    if (eq(t, "br")) { m->cur = 0; m->sp = 1; m->len++; return; }
    if (eq(t, "img")) { const char *a = dom_attr(n, "alt"); u32 w = 10; if (a) { while (a[w - 10] && w < 30) w++; } m->len += w; if (w > m->word) m->word = w; return; }
    if (eq(t, "input") || eq(t, "select") || eq(t, "textarea")) {
        const char *ty = dom_attr(n, "type");
        u32 w = eq(t, "select") ? 14 : 26;
        if (ty && (eq(ty, "hidden"))) return;
        if (ty && (eq(ty, "checkbox") || eq(ty, "radio"))) w = 3;
        if (ty && (eq(ty, "submit") || eq(ty, "button"))) { const char *v = dom_attr(n, "value"); w = 6; if (v) while (v[w - 6] && w < 30) w++; }
        m->len += w + 1; if (w > m->word) m->word = w;
        return;
    }
    for (c = g_node[n].first; c; c = g_node[c].next) measure(c, m, depth + 1);
}

static int cell_tag(u32 n) { const char *t = dom_tag(n); return eq(t, "td") || eq(t, "th"); }

static int span_of(u32 n)
{
    const char *s = dom_attr(n, "colspan");
    int v = 0;
    while (s && *s >= '0' && *s <= '9' && v < 100) v = v * 10 + (*s++ - '0');
    return v < 1 ? 1 : v;
}

/* The rows: <tr>s straight under the table, or under its thead, tbody
 * and tfoot, in that order of appearance. */
static int table_rows(u32 tab, u32 *rows, int max)
{
    u32 c, r;
    int n = 0;
    for (c = g_node[tab].first; c && n < max; c = g_node[c].next) {
        const char *t = dom_tag(c);
        if (g_node[c].type != N_ELEM || (g_node[c].flags & NF_HIDDEN)) continue;
        if (eq(t, "tr")) rows[n++] = c;
        else if (eq(t, "thead") || eq(t, "tbody") || eq(t, "tfoot")) {
            for (r = g_node[c].first; r && n < max; r = g_node[r].next)
                if (g_node[r].type == N_ELEM && !(g_node[r].flags & NF_HIDDEN) && eq(dom_tag(r), "tr")) rows[n++] = r;
        }
    }
    return n;
}

static void render(u32 n, u8 col, u8 at, int depth);

static int table_layout(u32 tab, u8 col, u8 at, int depth)
{
    static u32 rows[TAB_ROWS];
    u32 lo[TAB_COLS], hi[TAB_COLS], w[TAB_COLS], c;
    int nrows, ncols = 0, r, i, avail, slo = 0, shi = 0;

    nrows = table_rows(tab, rows, TAB_ROWS);
    if (nrows < 1) return 0;
    for (i = 0; i < TAB_COLS; i++) lo[i] = hi[i] = 1;
    for (r = 0; r < nrows; r++) {
        int x = 0;
        for (c = g_node[rows[r]].first; c; c = g_node[c].next) {
            Measure m;
            int sp;
            if (g_node[c].type != N_ELEM || (g_node[c].flags & NF_HIDDEN) || !cell_tag(c)) continue;
            sp = span_of(c);
            if (x + sp > TAB_COLS) return 0;
            m.len = m.word = m.cur = 0; m.sp = 1;
            measure(c, &m, 0);
            if (m.len >= TAB_CELL) return 0;
            if (sp == 1) {
                if (m.word > lo[x]) lo[x] = m.word;
                if (m.len > hi[x]) hi[x] = m.len;
            }
            x += sp;
        }
        if (x > ncols) ncols = x;
    }
    if (ncols < 2) return 0;
    avail = s_page_w - TAB_GAP * (ncols - 1);
    if (avail < 4 * ncols) return 0;
    for (i = 0; i < ncols; i++) {
        if (lo[i] > 30) lo[i] = 30;              /* a long address breaks */
        if (hi[i] < lo[i]) hi[i] = lo[i];
        if (hi[i] <= 15) lo[i] = hi[i];          /* a short column: never broken */
        slo += (int)lo[i]; shi += (int)hi[i];
    }
    if (shi <= avail) for (i = 0; i < ncols; i++) w[i] = hi[i];
    else if (slo <= avail) {
        int extra = avail - slo, want = shi - slo, given = 0;
        for (i = 0; i < ncols; i++) { w[i] = lo[i] + (u32)(extra * (int)(hi[i] - lo[i]) / (want ? want : 1)); given += (int)w[i]; }
        for (i = 0; given < avail && i < ncols; i++) if (w[i] < hi[i]) { w[i]++; given++; }
    } else {
        for (i = 0; i < ncols; i++) { w[i] = (u32)(avail * (int)lo[i] / slo); if (w[i] < 3) w[i] = 3; }
    }

    /* Out, as marks for the reader: a row, and each cell's place in it. */
    {
        u32 cap;
        for (cap = g_node[tab].first; cap; cap = g_node[cap].next)
            if (g_node[cap].type == N_ELEM && eq(dom_tag(cap), "caption")) render(cap, col, at, depth + 1);
    }
    s_in_table = 1;
    out("<fl-tab>");
    for (r = 0; r < nrows; r++) {
        int x = 0, cx = 0;
        out("<fl-tr>");
        for (c = g_node[rows[r]].first; c; c = g_node[c].next) {
            const Node *cn = &g_node[c];
            int sp, k;
            u32 cw = 0;
            u32 kid;
            u8 ccol = cn->col ? cn->col : col;
            if (cn->type != N_ELEM || (cn->flags & NF_HIDDEN) || !cell_tag(c)) continue;
            sp = span_of(c);
            for (k = 0; k < sp && x + k < ncols; k++) cw += w[x + k] + (k ? TAB_GAP : 0);
            out("<fl-td tx=\""); out_num((u32)cx); out("\" tw=\""); out_num(cw); out("\">");
            if (cn->align) { out("<fl-b al=\""); out_num(cn->align); out("\">"); }
            if (eq(dom_tag(c), "th")) {          /* a heading cell: bright and bold */
                out("<fl-s c=\""); out_num(C_BRIGHT); out("\" a=\""); out_num((u32)(cn->at | AT_BOLD)); out("\">");
            }
            for (kid = cn->first; kid; kid = g_node[kid].next) render(kid, ccol, cn->at, depth + 1);
            if (eq(dom_tag(c), "th")) out("</fl-s>");
            if (cn->align) out("</fl-b>");
            out("</fl-td>");
            cx += (int)cw + TAB_GAP;
            x += sp;
        }
        out("</fl-tr>");
    }
    out("</fl-tab>");
    s_in_table = 0;
    return 1;
}

static void render(u32 n, u8 col, u8 at, int depth)
{
    const Node *x = &g_node[n];
    const char *t;
    u32 c;
    int styled, blocked = 0, clickable = 0, tag_out = 1;
    if (depth > 200) return;
    if (x->type == N_TEXT) {
        u32 k;
        for (k = 0; k < x->len; k++) {
            char ch = g_pool[x->name + k];
            if (x->xform == 1 && ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
            else if (x->xform == 2 && ch >= 'A' && ch <= 'Z') ch = (char)(ch + 32);
            if (ch == '<') out("&lt;");
            else if (ch == '&') out("&amp;");
            else web_html_byte((u8)ch);
        }
        return;
    }
    if (x->type == N_DOC) { for (c = x->first; c; c = g_node[c].next) render(c, col, at, depth + 1); return; }
    if (x->type != N_ELEM || (x->flags & NF_HIDDEN)) return;
    t = dom_tag(n);
    if (eq(t, "head") || eq(t, "script") || eq(t, "style") || eq(t, "template") || eq(t, "title")) return;
    if (eq(t, "noscript") && script_site_on(0)) return;
    if (eq(t, "table") && !s_in_table && s_page_w && table_layout(n, x->col ? x->col : col, x->at, depth)) return;
    styled = (x->col && x->col != col) || (x->at != at);
    /* how it sits: as a block, or in the line */
    if ((x->disp == D_BLOCK || x->disp == D_ITEM) && (!drawn_block(t) || x->align || x->indent)) blocked = 1;
    if (x->disp == D_INLINE && drawn_block(t) && !eq(t, "br") && !eq(t, "hr")) tag_out = 0;   /* a menu's li, inline */
    if ((x->flags & NF_CLICK) && !eq(t, "a") && !eq(t, "button") && !eq(t, "input")) clickable = 1;
    if (!s_playwrap && !in_link(n) && !player_of(n) && web_first_sound() && listen_words(n)) {
        const char *snd = web_first_sound(), *h = dom_attr(n, "href");
        u32 k;
        int dead = !h || !h[0] || h[0] == '#' || lower(h[0]) == 'j';     /* "#", "javascript:..." */
        if (!(eq(t, "a") && !dead)) {
            for (k = 0; snd[k]; k++) { }
            if (eq(t, "a") || eq(t, "button")) {
                out("<a"); out_attr("href", snd, k); out(">");         /* the words alone: no button, no dead link */
                char b[48];
                u32 len = dom_text(n, b, sizeof b);
                for (k = 0; k < len; k++) { if (b[k] == '<') out("&lt;"); else if (b[k] == '&') out("&amp;"); else web_html_byte((u8)b[k]); }
                out("</a>");
                return;
            }
            clickable = 3;                       /* the link goes inside it: a heading would end it */
            s_playwrap = 1;
        }
    }
    {
        const char *play = clickable == 3 ? 0 : player_of(n);
        if (play && !in_link(n)) {
            u32 k;
            for (k = 0; play[k]; k++) { }
            out("<a"); out_attr("href", play, k); out(">");
            if (eq(t, "audio") || eq(t, "video")) {
                out("[listen]");
                out("</a>");
                return;                          /* its <source>s say nothing more */
            }
            clickable = 2;                       /* closed with </a> below      */
            s_playwrap = 1;                      /* nothing inside is linked again */
        }
    }
    if (blocked) {
        out("<fl-b");
        if (x->align) { out(" al=\""); out_num(x->align); out("\""); }
        if (x->indent) { out(" in=\""); out_num(x->indent); out("\""); }
        out(">");
    }
    if (styled) {
        out("<fl-s c=\""); out_num(x->col ? x->col : col); out("\" a=\""); out_num(x->at); out("\">");
    }
    if (clickable == 1) { out("<fl-c n=\""); out_num(n); out("\">"); }
    if (tag_out) {
        u16 a;
        const char *name = t;
        if (eq(t, "li") && x->nobullet) name = "fl-li";
        web_html_byte('<'); out(name);
        for (a = x->attr; a; a = s_attr[a].next) {
            const char *an = g_pool + s_attr[a].name;
            if (!in_list(an, s_keep_attrs)) continue;
            if (eq(an, "value") && (x->flags & NF_VALUE)) continue;
            if (eq(an, "type") && eq(t, "button") && (x->flags & NF_CLICK)) continue;
            out_attr(an, g_pool + s_attr[a].val, s_attr[a].vlen);
        }
        if (x->flags & NF_VALUE) out_attr("value", g_pool + x->value, x->vlen);
        if (eq(t, "button") && (x->flags & NF_CLICK)) out(" type=\"flopos\"");
        if (eq(t, "a") || eq(t, "input") || eq(t, "select") || eq(t, "textarea") || eq(t, "button") || eq(t, "form")) {
            out(" fl=\""); out_num(n); out("\"");
        }
        web_html_byte('>');
    }
    if (clickable == 3) {
        const char *snd = web_first_sound();
        u32 k;
        for (k = 0; snd[k]; k++) { }
        out("<a"); out_attr("href", snd, k); out(">");
    }
    if (!in_list(t, s_void)) {
        if (eq(t, "textarea") && (x->flags & NF_VALUE)) {
            u32 k;
            for (k = 0; k < x->vlen; k++) { char ch = g_pool[x->value + k]; if (ch == '<') out("&lt;"); else if (ch == '&') out("&amp;"); else web_html_byte((u8)ch); }
        } else {
            for (c = x->first; c; c = g_node[c].next) {
                if (g_node[c].type == N_TEXT) { ((Node *)&g_node[c])->xform = x->xform; }
                render(c, x->col ? x->col : col, x->at, depth + 1);
            }
        }
        if (clickable == 3) { out("</a>"); s_playwrap = 0; }
        if (tag_out) { out("</"); out(eq(t, "li") && x->nobullet ? "fl-li" : t); out(">"); }
    }
    if (clickable == 1) out("</fl-c>");
    if (clickable == 2) { out("</a>"); s_playwrap = 0; }
    if (styled) out("</fl-s>");
    if (blocked) out("</fl-b>");
}

void dom_render(Page *pg)
{
    s_page_w = pg ? pg->width : 0;
    s_in_table = 0;
    render(1, 0, 0, 0);
    web_html_flush();
}
