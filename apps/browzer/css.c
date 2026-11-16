/* ===========================================================================
 * css.c -- the part of CSS a text screen can show.
 *
 * A page's style sheets are read for what browzer can do with them:
 * hiding and showing, bold, italic, underline, colour (as a hint, in
 * FLOPOS's own palette), centring and right-aligning, indenting, lists
 * without bullets, capitals.  Everything else is passed over.
 *
 * Anything a page pins over its own content -- position: fixed or
 * sticky -- is never drawn.  That one rule takes care of newsletter
 * boxes, cookie walls, chat bubbles and sticky bars, whoever made them.
 *
 * Sites are told the screen is 800 pixels wide, as the laptop's is, so
 * they pick their desktop rules.  They are also told it is dark, which
 * it is: a site with a dark look gives it.
 *
 * Finding which rules apply to an element would be every rule against
 * every element -- too slow here.  So rules are filed by the last part of
 * their selector (an id, a class, a tag), and each element only looks in
 * the drawers its own id, classes and tag name.
 * ===========================================================================
 */
#include "dom.h"

#define CSS_RULES 3000
#define CSS_PARTS 6000
#define CSS_DECLS 6000
#define BUCKETS   1024

enum { P_DISPLAY, P_VISIBILITY, P_POSITION, P_COLOR, P_WEIGHT, P_STYLE, P_DECOR, P_ALIGN,
       P_XFORM, P_LIST, P_MLEFT, P_PLEFT, P_OPACITY, P_TINY, P_CLIP, P_COUNT };

typedef struct { u8 prop, important; s16 val; } Decl;
typedef struct { u16 part, nparts, decl, ndecl, next; u32 rank; } Rule;

static Rule s_rule[CSS_RULES];
static Part s_part[CSS_PARTS];
static Decl s_decl[CSS_DECLS];
static u32  s_nrule, s_npart, s_ndecl, s_order;
static u16  s_bucket[BUCKETS];           /* rule + 1                        */
static u16  s_any;                       /* rules that fit any element      */

int css_count(void) { return (int)s_nrule; }

void css_reset(void)
{
    u32 i;
    s_nrule = s_npart = s_ndecl = s_order = 0;
    for (i = 0; i < BUCKETS; i++) s_bucket[i] = 0;
    s_any = 0;
    g_selpool_used = 1;
}

static int eq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static char low(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }

static u32 hash(char kind, const char *s, u32 n)
{
    u32 h = 2166136261u ^ (u8)kind, i;
    for (i = 0; i < n; i++) { h ^= (u8)s[i]; h *= 16777619u; }
    return h & (BUCKETS - 1);
}
static u32 hash0(char kind, const char *s) { u32 n = 0; while (s[n]) n++; return hash(kind, s, n); }

/* ---- values --------------------------------------------------------------------- */
static int word(const char *v, u32 n, const char *w)
{
    u32 i;
    for (i = 0; i < n && w[i]; i++) if (low(v[i]) != w[i]) return 0;
    return i == n && !w[i];
}

static int has_word(const char *v, u32 n, const char *w)
{
    u32 i = 0;
    while (i < n) {
        u32 st;
        while (i < n && (v[i] == ' ' || v[i] == ',')) i++;
        st = i;
        while (i < n && v[i] != ' ' && v[i] != ',') i++;
        if (i > st && word(v + st, i - st, w)) return 1;
    }
    return 0;
}

/* A length in pixels: 12px, 1.5em, 2rem, 10% (of 800), 0.  -1 if not one. */
static s32 px(const char *v, u32 n)
{
    s32 whole = 0, frac = 0, fdiv = 1, neg = 0;
    u32 i = 0;
    if (i < n && v[i] == '-') { neg = 1; i++; }
    if (i >= n || !((v[i] >= '0' && v[i] <= '9') || v[i] == '.')) return word(v, n, "auto") ? -2 : -1;
    while (i < n && v[i] >= '0' && v[i] <= '9') whole = whole * 10 + (v[i++] - '0');
    if (i < n && v[i] == '.') { i++; while (i < n && v[i] >= '0' && v[i] <= '9') { if (fdiv < 1000) { frac = frac * 10 + (v[i] - '0'); fdiv *= 10; } i++; } }
    {
        s32 r = whole * 1000 + frac * (1000 / fdiv);         /* thousandths */
        const char *u = v + i;
        u32 un = n - i;
        if (un >= 2 && (word(u, 2, "em") || (un >= 3 && word(u, 3, "rem")))) r = r * 16;
        else if (un >= 1 && u[0] == '%') r = r * 8;          /* of 800 */
        else if (un >= 2 && word(u, 2, "vw")) r = r * 8;
        else if (un >= 2 && word(u, 2, "ch")) r = r * 8;
        else if (un >= 2 && word(u, 2, "pt")) r = r * 4 / 3;
        r /= 1000;
        return neg ? -r - 1000000 : r;                        /* negative: kept apart */
    }
}

/* ---- colours ------------------------------------------------------------------------ */
static int hexd(char c) { c = low(c); return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; }

static const struct { const char *n; u32 rgb; } s_names[] = {
    { "black", 0x000000 }, { "white", 0xFFFFFF }, { "red", 0xFF0000 }, { "green", 0x008000 },
    { "blue", 0x0000FF }, { "yellow", 0xFFFF00 }, { "orange", 0xFFA500 }, { "purple", 0x800080 },
    { "gray", 0x808080 }, { "grey", 0x808080 }, { "silver", 0xC0C0C0 }, { "maroon", 0x800000 },
    { "navy", 0x000080 }, { "teal", 0x008080 }, { "olive", 0x808000 }, { "lime", 0x00FF00 },
    { "aqua", 0x00FFFF }, { "fuchsia", 0xFF00FF }, { "crimson", 0xDC143C }, { "darkred", 0x8B0000 },
    { "darkgreen", 0x006400 }, { "darkblue", 0x00008B }, { "gold", 0xFFD700 }, { "tomato", 0xFF6347 },
    { "firebrick", 0xB22222 }, { "royalblue", 0x4169E1 }, { "steelblue", 0x4682B4 }, { "forestgreen", 0x228B22 },
    { "seagreen", 0x2E8B57 }, { "darkorange", 0xFF8C00 }, { "goldenrod", 0xDAA520 }, { "indigo", 0x4B0082 },
    { "violet", 0xEE82EE }, { "magenta", 0xFF00FF }, { "cyan", 0x00FFFF }, { "brown", 0xA52A2A },
    { "dodgerblue", 0x1E90FF }, { "limegreen", 0x32CD32 }, { "orangered", 0xFF4500 }, { "darkgray", 0xA9A9A9 },
    { "darkgrey", 0xA9A9A9 }, { "lightgray", 0xD3D3D3 }, { "lightgrey", 0xD3D3D3 }, { "dimgray", 0x696969 }, { 0, 0 } };

/* A colour, as one of FLOPOS's: red, gold, green or the accent; 0 for
 * the greys, near-blacks and near-whites, which stay plain text so they
 * read on a dark screen. */
static s16 colour(const char *v, u32 n)
{
    s32 r = -1, g = 0, b = 0, mx, mn, l, s, h;
    u32 i;
    if (n && v[0] == '#') {
        if (n == 4 || n == 5) { r = hexd(v[1]) * 17; g = hexd(v[2]) * 17; b = hexd(v[3]) * 17; }
        else if (n == 7 || n == 9) { r = hexd(v[1]) * 16 + hexd(v[2]); g = hexd(v[3]) * 16 + hexd(v[4]); b = hexd(v[5]) * 16 + hexd(v[6]); }
        if (r < 0 || g < 0 || b < 0) return 0;
    } else if (n > 4 && low(v[0]) == 'r' && low(v[1]) == 'g' && low(v[2]) == 'b') {
        s32 c[3] = { 0, 0, 0 }, k = 0;
        for (i = 3; i < n && v[i] != '('; i++) { }
        for (i++; i < n && k < 3; ) {
            s32 x = 0;
            while (i < n && (v[i] == ' ' || v[i] == ',')) i++;
            while (i < n && v[i] >= '0' && v[i] <= '9') x = x * 10 + (v[i++] - '0');
            if (i < n && v[i] == '%') x = x * 255 / 100;
            while (i < n && v[i] != ' ' && v[i] != ',' && v[i] != ')') i++;
            c[k++] = x > 255 ? 255 : x;
        }
        r = c[0]; g = c[1]; b = c[2];
    } else {
        for (i = 0; s_names[i].n; i++) if (word(v, n, s_names[i].n)) { r = (s32)(s_names[i].rgb >> 16); g = (s32)((s_names[i].rgb >> 8) & 255); b = (s32)(s_names[i].rgb & 255); break; }
        if (r < 0) return 0;
    }
    mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    l = (mx + mn) / 2;
    if (mx == mn) return 0;
    s = l < 128 ? (mx - mn) * 255 / (mx + mn) : (mx - mn) * 255 / (510 - mx - mn);
    if (s < 64 || l < 40 || l > 230) return 0;
    if (mx == r) h = 60 * (g - b) / (mx - mn);
    else if (mx == g) h = 120 + 60 * (b - r) / (mx - mn);
    else h = 240 + 60 * (r - g) / (mx - mn);
    if (h < 0) h += 360;
    if (h < 15 || h >= 345) return A_RED;
    if (h < 70) return A_GOLD;
    if (h < 170) return A_ON;
    return C_ACCENT;
}

/* ---- declarations ---------------------------------------------------------------------- */
static int decl_add(int prop, s32 val, int important)
{
    if (s_ndecl >= CSS_DECLS) return 0;
    s_decl[s_ndecl].prop = (u8)prop;
    s_decl[s_ndecl].val = (s16)val;
    s_decl[s_ndecl].important = (u8)important;
    s_ndecl++;
    return 1;
}

/* One "name: value" into the declarations it makes. */
static void declaration(const char *nm, u32 nn, const char *v, u32 vn)
{
    int imp = 0;
    while (vn && (v[vn - 1] == ' ' || v[vn - 1] == '\n' || v[vn - 1] == '\t' || v[vn - 1] == '\r')) vn--;
    if (vn >= 10 && v[vn - 10] == '!' && word(v + vn - 9, 9, "important")) { imp = 1; vn -= 10; while (vn && v[vn - 1] == ' ') vn--; }
    while (vn && (v[0] == ' ' || v[0] == '\t' || v[0] == '\n')) { v++; vn--; }
    if (word(nm, nn, "display")) {
        int d = -1;
        if (word(v, vn, "none")) d = D_NONE;
        else if (word(v, vn, "block") || word(v, vn, "flex") || word(v, vn, "grid") || word(v, vn, "flow-root") || word(v, vn, "table") ||
                 word(v, vn, "table-row") || word(v, vn, "table-cell") || word(v, vn, "table-caption")) d = D_BLOCK;
        else if (word(v, vn, "list-item")) d = D_ITEM;
        else if (word(v, vn, "inline") || word(v, vn, "inline-block") || word(v, vn, "inline-flex") || word(v, vn, "inline-grid") || word(v, vn, "contents")) d = D_INLINE;
        if (d >= 0) decl_add(P_DISPLAY, d, imp);
    } else if (word(nm, nn, "visibility")) decl_add(P_VISIBILITY, word(v, vn, "hidden") || word(v, vn, "collapse"), imp);
    else if (word(nm, nn, "position")) decl_add(P_POSITION, word(v, vn, "fixed") || word(v, vn, "sticky") ? 1 : word(v, vn, "absolute") ? 2 : 0, imp);
    else if (word(nm, nn, "color")) { if (!word(v, vn, "inherit") && !word(v, vn, "currentcolor")) decl_add(P_COLOR, colour(v, vn), imp); }
    else if (word(nm, nn, "font-weight")) decl_add(P_WEIGHT, word(v, vn, "bold") || word(v, vn, "bolder") || (vn == 3 && v[0] >= '6' && v[0] <= '9'), imp);
    else if (word(nm, nn, "font-style")) decl_add(P_STYLE, word(v, vn, "italic") || word(v, vn, "oblique"), imp);
    else if (word(nm, nn, "font")) {
        if (has_word(v, vn, "bold")) decl_add(P_WEIGHT, 1, imp);
        if (has_word(v, vn, "italic")) decl_add(P_STYLE, 1, imp);
    }
    else if (word(nm, nn, "text-decoration") || word(nm, nn, "text-decoration-line")) decl_add(P_DECOR, has_word(v, vn, "underline"), imp);
    else if (word(nm, nn, "text-align")) decl_add(P_ALIGN, word(v, vn, "center") ? 1 : word(v, vn, "right") || word(v, vn, "end") ? 2 : 0, imp);
    else if (word(nm, nn, "text-transform")) decl_add(P_XFORM, word(v, vn, "uppercase") ? 1 : word(v, vn, "lowercase") ? 2 : 0, imp);
    else if (word(nm, nn, "list-style") || word(nm, nn, "list-style-type")) decl_add(P_LIST, has_word(v, vn, "none"), imp);
    else if (word(nm, nn, "opacity")) decl_add(P_OPACITY, vn >= 1 && v[0] == '0' && (vn == 1 || (v[1] == '.' && vn > 2 && v[2] == '0' && (vn == 3 || v[3] == '0'))), imp);
    else if (word(nm, nn, "margin-left") || word(nm, nn, "padding-left") || word(nm, nn, "margin") || word(nm, nn, "padding") || word(nm, nn, "margin-inline-start") || word(nm, nn, "padding-inline-start")) {
        /* the left one: 1 value: all; 2: v h; 3: t h b; 4: t r b l */
        s32 vals[4], k = 0, left;
        u32 i = 0;
        int pad = nm[0] == 'p';
        while (i < vn && k < 4) {
            u32 st;
            while (i < vn && v[i] == ' ') i++;
            st = i;
            while (i < vn && v[i] != ' ') i++;
            if (i > st) vals[k++] = px(v + st, i - st);
        }
        if (!k) return;
        left = (nn > 7 && (nm[nn - 4] == 'l' || nm[nn - 5] == 's')) || k == 1 ? vals[0] : k == 4 ? vals[3] : vals[1];
        if (left == -1) return;
        decl_add(pad ? P_PLEFT : P_MLEFT, left < 0 ? 0 : left > 3000 ? 3000 : left, imp);
    }
    else if (word(nm, nn, "width") || word(nm, nn, "height") || word(nm, nn, "max-width") || word(nm, nn, "max-height")) {
        s32 p = px(v, vn);
        if (p >= 0 && p <= 1) decl_add(P_TINY, 1, imp);
    }
    else if (word(nm, nn, "clip") || word(nm, nn, "clip-path")) {
        /* rect(0 0 0 0), rect(1px, 1px, 1px, 1px), inset(50%): the screen-reader-only trick */
        decl_add(P_CLIP, (vn > 4 && (word(v, 4, "rect") || word(v, 5, "inset"))), imp);
    }
}

/* "a: b; c: d" -> declarations; returns how many were added. */
static u32 declarations(const char *s, u32 len)
{
    u32 i = 0, before = s_ndecl;
    while (i < len) {
        u32 ns, ne, vs, depth = 0;
        char q = 0;
        while (i < len && (s[i] == ' ' || s[i] == '\n' || s[i] == '\t' || s[i] == '\r' || s[i] == ';')) i++;
        ns = i;
        while (i < len && s[i] != ':' && s[i] != ';' && s[i] != '{') i++;
        ne = i;
        while (ne > ns && (s[ne - 1] == ' ' || s[ne - 1] == '\t')) ne--;
        if (i < len && s[i] == '{') {          /* a nested rule: passed over */
            int d = 0;
            while (i < len) { if (s[i] == '{') d++; if (s[i] == '}') { d--; if (!d) { i++; break; } } i++; }
            continue;
        }
        if (i >= len || s[i] != ':') { while (i < len && s[i] != ';') i++; continue; }
        i++;
        vs = i;
        while (i < len) {
            char c = s[i];
            if (q) { if (c == q) q = 0; }
            else if (c == '"' || c == '\'') q = c;
            else if (c == '(') depth++;
            else if (c == ')' && depth) depth--;
            else if (c == ';' && !depth) break;
            i++;
        }
        declaration(s + ns, ne - ns, s + vs, i - vs);
    }
    return s_ndecl - before;
}

/* ---- rules ---------------------------------------------------------------------------------- */
static void file_rule(u32 r)
{
    const Part *p = &s_part[s_rule[r].part];   /* p[0] is the element itself */
    u32 b;
    if (p->id) b = hash0('#', g_selpool + p->id);
    else if (p->ncls) b = hash('.', g_selpool + p->cls[0], p->clen[0]);
    else if (p->tag) b = hash0('t', g_selpool + p->tag);
    else { s_rule[r].next = s_any; s_any = (u16)(r + 1); return; }
    s_rule[r].next = s_bucket[b];
    s_bucket[b] = (u16)(r + 1);
}

static void rule(const char *sel, u32 sl, const char *body, u32 bl)
{
    u32 d0 = s_ndecl, nd, i = 0;
    nd = declarations(body, bl);
    if (!nd) return;
    while (i < sl) {
        u32 st = i;
        int depth = 0, n, spec;
        while (i < sl && !(sel[i] == ',' && !depth)) { if (sel[i] == '(' || sel[i] == '[') depth++; if (sel[i] == ')' || sel[i] == ']') depth--; i++; }
        if (s_nrule >= CSS_RULES || s_npart + SEL_PARTS > CSS_PARTS) return;
        n = sel_parse(sel + st, i - st, &s_part[s_npart], SEL_PARTS, &spec);
        if (n > 0) {
            Rule *r = &s_rule[s_nrule];
            r->part = (u16)s_npart; r->nparts = (u16)n;
            r->decl = (u16)d0; r->ndecl = (u16)nd;
            r->rank = ((u32)(spec > 4095 ? 4095 : spec) << 16) | (s_order++ & 0xFFFF);
            s_npart += (u32)n;
            file_rule(s_nrule);
            s_nrule++;
        }
        if (i < sl) i++;
    }
}

/* Does an @media prelude fit an 800-pixel, dark, still screen? */
static int media_fits(const char *s, u32 len)
{
    u32 i = 0;
    while (i < len) {                        /* any of a comma list        */
        u32 st = i, e;
        int ok = 1, neg = 0;
        int depth = 0;
        while (i < len && !(s[i] == ',' && !depth)) { if (s[i] == '(') depth++; if (s[i] == ')') depth--; i++; }
        e = i;
        {
            u32 k = st;
            while (k < e) {
                u32 ws;
                while (k < e && (s[k] == ' ' || s[k] == '\n')) k++;
                ws = k;
                if (k < e && s[k] == '(') {
                    u32 nm, ne, vs, ve;
                    k++;
                    while (k < e && s[k] == ' ') k++;
                    nm = k;
                    while (k < e && s[k] != ':' && s[k] != ')') k++;
                    ne = k;
                    while (ne > nm && s[ne - 1] == ' ') ne--;
                    vs = k < e && s[k] == ':' ? k + 1 : k;
                    while (vs < e && s[vs] == ' ') vs++;
                    while (k < e && s[k] != ')') k++;
                    ve = k;
                    while (ve > vs && s[ve - 1] == ' ') ve--;
                    if (k < e) k++;
                    {
                        s32 v = px(s + vs, ve - vs);
                        if (word(s + nm, ne - nm, "min-width")) ok &= v >= 0 && 800 >= v;
                        else if (word(s + nm, ne - nm, "max-width")) ok &= v >= 0 && 800 <= v;
                        else if (word(s + nm, ne - nm, "min-height")) ok &= v >= 0 && 600 >= v;
                        else if (word(s + nm, ne - nm, "max-height")) ok &= v >= 0 && 600 <= v;
                        else if (word(s + nm, ne - nm, "orientation")) ok &= word(s + vs, ve - vs, "landscape");
                        else if (word(s + nm, ne - nm, "prefers-color-scheme")) ok &= word(s + vs, ve - vs, "dark");
                        else if (word(s + nm, ne - nm, "prefers-reduced-motion")) ok &= word(s + vs, ve - vs, "reduce");
                        else if (word(s + nm, ne - nm, "hover") || word(s + nm, ne - nm, "pointer")) ok &= word(s + vs, ve - vs, "none");
                        else if (word(s + nm, ne - nm, "print")) ok = 0;
                    }
                    continue;
                }
                while (k < e && s[k] != ' ' && s[k] != '(') k++;
                if (word(s + ws, k - ws, "not")) neg = 1;
                else if (word(s + ws, k - ws, "print") || word(s + ws, k - ws, "speech")) ok = 0;
            }
        }
        if (neg) ok = !ok;
        if (ok) return 1;
        if (i < len) i++;
    }
    return 0;
}

static void sheet(const char *s, u32 len, int depth)
{
    u32 i = 0;
    while (i < len) {
        u32 ps, pe, bs, be;
        int d = 0;
        char q = 0;
        /* skip spaces and comments */
        for (;;) {
            while (i < len && (s[i] == ' ' || s[i] == '\n' || s[i] == '\t' || s[i] == '\r' || s[i] == '}' || s[i] == ';')) i++;
            if (i + 1 < len && s[i] == '/' && s[i + 1] == '*') {
                i += 2;
                while (i + 1 < len && !(s[i] == '*' && s[i + 1] == '/')) i++;
                i += 2;
                continue;
            }
            if (i + 3 < len && s[i] == '<' && s[i + 1] == '!' && s[i + 2] == '-' && s[i + 3] == '-') { i += 4; continue; }
            if (i + 2 < len && s[i] == '-' && s[i + 1] == '-' && s[i + 2] == '>') { i += 3; continue; }
            break;
        }
        if (i >= len) return;
        ps = i;
        while (i < len && s[i] != '{' && s[i] != ';') {
            if (s[i] == '"' || s[i] == '\'') { char qq = s[i++]; while (i < len && s[i] != qq) i++; }
            i++;
        }
        pe = i;
        if (i >= len) return;
        if (s[i] == ';') { i++; continue; }  /* @import, @charset: passed over */
        i++;
        bs = i;
        d = 1;
        while (i < len && d) {
            char c = s[i];
            if (q) { if (c == q) q = 0; }
            else if (c == '"' || c == '\'') q = c;
            else if (c == '/' && i + 1 < len && s[i + 1] == '*') { i += 2; while (i + 1 < len && !(s[i] == '*' && s[i + 1] == '/')) i++; i++; }
            else if (c == '{') d++;
            else if (c == '}') d--;
            i++;
        }
        be = d ? i : i - 1;
        if (s[ps] == '@') {
            u32 k = ps + 1, ns = ps + 1;
            while (k < pe && s[k] != ' ' && s[k] != '(') k++;
            if (depth < 4 && (word(s + ns, k - ns, "media"))) { if (media_fits(s + k, pe - k)) sheet(s + bs, be - bs, depth + 1); }
            else if (depth < 4 && (word(s + ns, k - ns, "supports") || word(s + ns, k - ns, "layer") || word(s + ns, k - ns, "container") || word(s + ns, k - ns, "document"))) sheet(s + bs, be - bs, depth + 1);
            continue;                            /* @font-face, @keyframes, @page... */
        }
        while (pe > ps && (s[pe - 1] == ' ' || s[pe - 1] == '\n' || s[pe - 1] == '\t' || s[pe - 1] == '\r')) pe--;
        rule(s + ps, pe - ps, s + bs, be - bs);
    }
}

void css_add(const char *text, u32 len) { sheet(text, len, 0); }

void css_inline(u32 n, const char *decls, u32 len) { (void)n; (void)decls; (void)len; }

/* ---- the cascade ----------------------------------------------------------------------------- */
static u32 s_best[P_COUNT];
static s16 s_val[P_COUNT];

static void take(const Decl *d, u32 rank)
{
    u32 r = rank | (d->important ? 0x80000000u : 0);
    if (s_best[d->prop] && s_best[d->prop] > r) return;
    s_best[d->prop] = r ? r : 1;
    s_val[d->prop] = d->val;
}

static void consider(u32 list, u32 n)
{
    u32 r;
    int guard = 0;
    for (r = list; r && guard++ < CSS_RULES; r = s_rule[r - 1].next) {
        const Rule *x = &s_rule[r - 1];
        u32 k;
        if (!sel_match(&s_part[x->part], x->nparts, n)) continue;
        for (k = 0; k < x->ndecl; k++) take(&s_decl[x->decl + k], x->rank);
    }
}

static const char *s_blocks[] = { "p", "div", "ul", "ol", "table", "section", "article", "header", "footer", "nav",
    "pre", "blockquote", "dt", "dd", "form", "main", "center", "aside", "figure", "h1", "h2", "h3", "h4", "h5", "h6",
    "hr", "address", "details", "summary", "dl", "fieldset", "figcaption", "menu", "tr", "tbody", "thead", "tfoot",
    "body", "html", "legend", "caption", "noscript", 0 };
static int is_block(const char *t) { int i; for (i = 0; s_blocks[i]; i++) if (eq(t, s_blocks[i])) return 1; return 0; }

static void compute(u32 n, const Node *parent, int depth)
{
    Node *x = &g_node[n];
    const char *t;
    u32 c;
    if (depth > 200) return;
    if (x->type == N_TEXT) { x->flags &= (u8)~NF_HIDDEN; return; }
    if (x->type != N_ELEM) { for (c = x->first; c; c = g_node[c].next) compute(c, parent, depth + 1); return; }
    t = dom_tag(n);
    {
        u32 k;
        for (k = 0; k < P_COUNT; k++) s_best[k] = 0;
    }
    /* the drawers this element looks in */
    {
        const char *id = dom_attr(n, "id"), *cl = dom_attr(n, "class");
        if (id && *id) consider(s_bucket[hash0('#', id)], n);
        if (cl) {
            u32 i = 0, seen[8], ns = 0;
            while (cl[i]) {
                u32 st, b, k;
                while (cl[i] == ' ' || cl[i] == '\t' || cl[i] == '\n' || cl[i] == '\r') i++;
                st = i;
                while (cl[i] && cl[i] != ' ' && cl[i] != '\t' && cl[i] != '\n' && cl[i] != '\r') i++;
                if (i == st) continue;
                b = hash('.', cl + st, i - st);
                for (k = 0; k < ns && seen[k] != b; k++) { }
                if (k < ns) continue;              /* that drawer was looked in */
                if (ns < 8) seen[ns++] = b;
                consider(s_bucket[b], n);
            }
        }
        consider(s_bucket[hash0('t', t)], n);
        consider(s_any, n);
    }
    /* style="..." beats the sheets */
    {
        const char *st = dom_attr(n, "style");
        if (st) {
            u32 d0 = s_ndecl, nd, k, len = 0;
            while (st[len]) len++;
            nd = declarations(st, len);
            for (k = 0; k < nd; k++) take(&s_decl[d0 + k], 0x7FFF0000u);
            s_ndecl = d0;
        }
    }
    /* what it is to start with, then what the sheets said */
    x->disp = (u8)(eq(t, "li") ? D_ITEM : is_block(t) ? D_BLOCK : D_INLINE);
    x->col = parent ? parent->col : 0;
    x->at = parent ? parent->at : 0;
    x->align = parent ? parent->align : 0;
    x->xform = parent ? parent->xform : 0;
    x->nobullet = parent ? parent->nobullet : 0;
    x->indent = 0;
    x->flags &= (u8)~NF_HIDDEN;
    if (parent && (parent->flags & NF_HIDDEN)) x->flags |= NF_HIDDEN;
    if (s_best[P_DISPLAY]) x->disp = (u8)s_val[P_DISPLAY];
    if (s_best[P_COLOR]) x->col = (u8)s_val[P_COLOR];
    if (s_best[P_WEIGHT]) x->at = (u8)(s_val[P_WEIGHT] ? x->at | AT_BOLD : x->at & ~AT_BOLD);
    if (s_best[P_STYLE]) x->at = (u8)(s_val[P_STYLE] ? x->at | AT_ITALIC : x->at & ~AT_ITALIC);
    if (s_best[P_DECOR]) x->at = (u8)(s_val[P_DECOR] ? x->at | AT_UNDERLINE : x->at & ~AT_UNDERLINE);
    if (s_best[P_ALIGN]) x->align = (u8)s_val[P_ALIGN];
    if (s_best[P_XFORM]) x->xform = (u8)s_val[P_XFORM];
    if (s_best[P_LIST]) x->nobullet = (u8)s_val[P_LIST];
    if (x->disp == D_BLOCK || x->disp == D_ITEM) {
        s32 ind = (s_best[P_MLEFT] ? s_val[P_MLEFT] : 0) + (s_best[P_PLEFT] ? s_val[P_PLEFT] : 0);
        ind /= 10;                               /* 10 pixels a character  */
        x->indent = (u8)(ind > 12 ? 12 : ind < 0 ? 0 : ind);
    }
    /* what is never drawn */
    if (x->disp == D_NONE || (s_best[P_VISIBILITY] && s_val[P_VISIBILITY]) || (s_best[P_OPACITY] && s_val[P_OPACITY]) ||
        (s_best[P_POSITION] && s_val[P_POSITION] == 1) ||
        (s_best[P_POSITION] && s_val[P_POSITION] == 2 && ((s_best[P_TINY] && s_val[P_TINY]) || (s_best[P_CLIP] && s_val[P_CLIP]))) ||
        dom_attr(n, "hidden") || (eq(t, "input") && dom_attr(n, "type") && eq(dom_attr(n, "type"), "hidden") && 0) ||
        (eq(t, "dialog") && !dom_attr(n, "open")))
        x->flags |= NF_HIDDEN;
    if (s_best[P_VISIBILITY] && !s_val[P_VISIBILITY] && parent && !(parent->flags & NF_HIDDEN) && x->disp != D_NONE) {
        /* visibility: visible under a hidden parent shows again -- only for visibility */
    }
    for (c = x->first; c; c = g_node[c].next) compute(c, x, depth + 1);
}

void css_apply(void)
{
    compute(1, 0, 0);
}
