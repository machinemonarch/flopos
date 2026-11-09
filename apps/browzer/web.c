/* ===========================================================================
 * web.c -- asking a web server for a page, and making it readable.
 *
 * Part of browzer, the app.  The connection itself -- the lookup, TCP,
 * and for a secure page the keys and the proof of who the server is -- is
 * the kernel's (conn_open); everything from the request on is here.
 *
 * HTTP is text over a TCP (or TLS) conversation: a line saying what you
 * want, a few lines about yourself, a blank line.  The answer is the same
 * shape the other way, then the page.  This asks in HTTP/1.0 on purpose:
 * a 1.0 server sends the page plainly and hangs up when it is finished,
 * so "the page" is simply everything until the line goes quiet.
 *
 * A page is mostly HTML, which is not for reading, so it goes through a
 * small reader on the way: tags dropped, scripts and styles skipped whole,
 * entities decoded, UTF-8 and Latin-1 both understood with the Swedish
 * letters drawn properly, whitespace squeezed, a break where a paragraph
 * or heading or list item would be, words wrapped to the width available.
 * Headings come out bright, links in the accent colour.
 *
 * The reader writes to one of two places.  `fetch` sends it to the
 * terminal, to scroll past like anything else.  browzer hands it a Page:
 * a grid of cells that keeps which link each character belongs to, the
 * links' addresses, and the page's title, so it can be scrolled and
 * clicked around in.
 * ===========================================================================
 */
#include "web.h"

/* A line down the serial cable, if there is one. */
static void wire(const char *a, const char *b)
{
    char line[200];
    fmt_str(line, sizeof line, "%s%s\n", a, b);
    A->wire(line);
}

/* ---- where the words go ---------------------------------------------- */
static Page *s_pg;                       /* a page, or 0 for the terminal  */

static void note(const char *t, u8 col)
{
    int i;
    if (!s_pg) { term_colour(col); term_print(t); term_newline(); return; }
    wire("[web] ", t);                 /* browzer's status, for the wire  */
    for (i = 0; t[i] && i < (int)sizeof s_pg->note - 1; i++) s_pg->note[i] = t[i];
    s_pg->note[i] = 0;
    s_pg->note_col = col;
    if (s_pg->progress) s_pg->progress();
}

static int out_width(void) { return s_pg ? s_pg->width : term_cols() - 1; }

/* What the handshake says while it checks a certificate, which on this
 * machine takes a moment worth knowing about. */
static void tls_note(const char *t)
{
    note(t, C_DIM);
    if (!s_pg) main_pump(1);
}

static void page_line_clear(int line)
{
    int i;
    for (i = 0; i < s_pg->width; i++) {
        PageCell *c = &s_pg->cells[line * s_pg->width + i];
        c->ch = ' '; c->col = C_TEXT; c->link = 0;
    }
}

static void out_newline(void)
{
    if (!s_pg) { term_newline(); return; }
    if (s_pg->lines < s_pg->max_lines) {
        page_line_clear(s_pg->lines);
        s_pg->lines++;
    } else {
        s_pg->full = 1;
    }
    s_pg->col = 0;
}

static void out_char(u8 ch, u8 col, u16 link)
{
    PageCell *c;
    if (!s_pg) { term_colour(col); term_putc(ch); return; }
    if (s_pg->full || s_pg->col >= s_pg->width) return;
    c = &s_pg->cells[(s_pg->lines - 1) * s_pg->width + s_pg->col++];
    c->ch = ch; c->col = col; c->link = link;
}

void page_clear(Page *pg)
{
    Page *keep = s_pg;
    s_pg = pg;
    pg->lines = 1;
    pg->col = 0;
    pg->full = 0;
    page_line_clear(0);
    pg->nlinks = 0;
    pg->nforms = 0;
    pg->nfields = 0;
    pg->pool_used = 0;
    pg->title[0] = 0;
    pg->note[0] = 0;
    pg->bytes = 0;
    pg->status = 0;
    pg->secure = 0;
    pg->pq = 0;
    s_pg = keep;
}

/* ---- turning HTML into lines --------------------------------------------- */
enum { H_TEXT, H_TAG, H_SKIP, H_ENT };
enum { A_NONE, A_NAME, A_EQ, A_VAL, A_QVAL };

static int  s_html;
static int  s_hstate;
static char s_tag[12];
static int  s_tag_len, s_tag_close, s_tag_done;
static char s_skip_until[10];
static char s_ent[10];
static int  s_ent_len;
static u8   s_word[80], s_word_col[80];
static u16  s_word_link[80];
static int  s_word_len, s_col, s_newlines, s_space;
static int  s_bold, s_link, s_dim, s_in_title;
static u16  s_linkid;
static u32  s_cp, s_cp_left;

/* Attributes: only href and alt are kept. */
static int  s_astate;
static char s_aname[8];
static int  s_aname_len;
static char s_quote;
static char s_href[PAGE_HREF_MAX];

/* Forms: the attributes a field needs, the form being read, and the text
 * area or button being read, if any.  A value can be long -- a page may
 * keep a whole conversation in a hidden field -- so it has room. */
#define VAL_MAX 32768
static char s_action[PAGE_HREF_MAX], s_method[8], s_type[12], s_fname[64], s_size[6];
static char s_val[VAL_MAX];
static int  s_has_val;
static int  s_form = -1, s_area = -1, s_area_w, s_button = -1, s_area_fresh;
static char s_alt[60];
static char s_wattr[8], s_hattr[8];      /* an image's width and height    */
static char *s_aval;
static int  s_aval_len, s_aval_max;

static u8 colour_now(void)
{
    return s_dim ? C_DIM : s_bold ? C_BRIGHT : s_link ? C_ACCENT : C_TEXT;
}

static void flush_word(void)
{
    int i, w = out_width();

    if (!s_word_len) return;
    if (s_col && s_col + 1 + s_word_len > w) { out_newline(); s_col = 0; }
    else if (s_col && s_space) {
        /* A space inside a link belongs to the link, so a highlighted link
         * is one unbroken bar rather than words with gaps. */
        out_char(' ', C_TEXT, (s_linkid && s_word_link[0] == s_linkid) ? s_linkid : 0);
        s_col++;
    }
    for (i = 0; i < s_word_len; i++) {
        if (s_col >= w) { out_newline(); s_col = 0; }
        out_char(s_word[i], s_word_col[i], s_word_link[i]);
        s_col++;
    }
    s_word_len = 0;
    s_space = 0;
    s_newlines = 0;
}

static void newline(int want)
{
    flush_word();
    while (s_newlines < want) {
        if (s_col || s_newlines || want > 1) out_newline();
        s_col = 0;
        s_newlines++;
        if (want == 1) break;
    }
    s_space = 0;
}

static void push(u8 g, u8 col)
{
    if (s_word_len < (int)sizeof s_word) {
        s_word[s_word_len] = g;
        s_word_col[s_word_len] = col;
        s_word_link[s_word_len] = s_linkid;
        s_word_len++;
    }
    if (s_word_len == (int)sizeof s_word) flush_word();
}

static u8 glyph_for(u32 c)
{
    switch (c) {
    case 0xE5: return CH_ARING_L;        case 0xC5: return CH_ARING;
    case 0xE4: return CH_ADIA_L;         case 0xC4: return CH_ADIA;
    case 0xF6: return CH_ODIA_L;         case 0xD6: return CH_ODIA;
    case 0xE9: case 0xE8: case 0xEA: case 0xEB: return 'e';
    case 0xC9: return 'E';
    case 0xFC: return 'u';               case 0xDC: return 'U';
    case 0xE0: case 0xE1: case 0xE2: case 0xE6: return 'a';
    case 0xF8: return 'o';               case 0xD8: return 'O';
    case 0x2018: case 0x2019: return '\'';
    case 0x201C: case 0x201D: return '"';
    case 0x2013: case 0x2014: return '-';
    case 0x2026: return '.';
    case 0xA9: return 'c';
    case 0x2022: case 0xB7: return '*';
    default: return (c >= 32 && c < 127) ? (u8)c : '?';
    }
}

static int put_utf8(char *o, u32 c)
{
    if (c < 0x80)    { o[0] = (char)c; return 1; }
    if (c < 0x800)   { o[0] = (char)(0xC0 | (c >> 6)); o[1] = (char)(0x80 | (c & 0x3F)); return 2; }
    if (c < 0x10000) { o[0] = (char)(0xE0 | (c >> 12)); o[1] = (char)(0x80 | ((c >> 6) & 0x3F));
                       o[2] = (char)(0x80 | (c & 0x3F)); return 3; }
    o[0] = (char)(0xF0 | (c >> 18)); o[1] = (char)(0x80 | ((c >> 12) & 0x3F));
    o[2] = (char)(0x80 | ((c >> 6) & 0x3F)); o[3] = (char)(0x80 | (c & 0x3F));
    return 4;
}

/* One character of readable text, already decoded. */
static void emit(u32 c)
{
    /* Inside a text area, the text is the box's value, not the page's. */
    if (s_area >= 0 && s_pg) {
        PageField *f = &s_pg->fields[s_area];
        char b[4];
        int n;
        if (s_area_fresh && c == '\n') { s_area_fresh = 0; return; }   /* HTML drops it */
        s_area_fresh = 0;
        if (c == '\r') return;
        n = put_utf8(b, c);
        if (f->vlen + (u32)n <= f->vcap) {
            int k;
            for (k = 0; k < n; k++) s_pg->pool[f->val + f->vlen + (u32)k] = b[k];
            f->vlen += (u32)n;
        }
        return;
    }
    if (s_in_title) {
        if (s_pg) {
            int n = 0;
            while (s_pg->title[n]) n++;
            if (c == ' ' || c == '\n' || c == '\t' || c == '\r' || c == 0xA0) {
                if (n && s_pg->title[n - 1] != ' ' && n < 62) { s_pg->title[n] = ' '; s_pg->title[n + 1] = 0; }
            } else if (n < 62) {
                s_pg->title[n] = (char)glyph_for(c);
                s_pg->title[n + 1] = 0;
            }
            return;
        }
    }
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == 0xA0) {
        flush_word();
        s_space = 1;
        return;
    }
    push(glyph_for(c), colour_now());
}

static void emit_text(const char *t, u8 col)
{
    for (; *t; t++) {
        if (*t == ' ') { flush_word(); s_space = 1; continue; }
        push((u8)*t, col);
    }
}

/* A rule across the page, drawn with the frame pieces so it joins up. */
static void rule(void)
{
    int i, w = out_width();
    newline(1);
    for (i = 0; i < w; i++) out_char(FR_H, C_GREY, 0);
    out_newline();
    s_col = 0;
    s_newlines = 1;
}

/* A number of pixels out of a width= or height=, or -1 for none or for a
 * percentage, which says nothing about size on its own. */
static int pixels(const char *a)
{
    int v = 0, i;
    if (!a[0]) return -1;
    for (i = 0; a[i] >= '0' && a[i] <= '9'; i++) v = v * 10 + (a[i] - '0');
    if (i == 0 || a[i] == '%') return -1;
    return v;
}

/* An image, as a box the size it would have been, saying what it is.
 * Width and height come from the tag where it gives them, turned from
 * pixels into character cells; tiny ones -- icons, tracking pixels --
 * are not worth a box, and get their description in brackets, or
 * nothing at all if they have none. */
static void image(void)
{
    int pw = pixels(s_wattr), ph = pixels(s_hattr), w = out_width();
    int cw, rows, x, y, tl, i;
    char label[80];

    if ((pw >= 0 && pw < 48) || (ph >= 0 && ph < 32)) {
        if (s_alt[0]) {
            s_dim = 1;
            emit_text("[", C_DIM); emit_text(s_alt, C_DIM); emit_text("]", C_DIM);
            flush_word();
            s_dim = 0;
            s_space = 1;
        }
        return;
    }

    if (s_alt[0]) fmt_str(label, sizeof label, "Image of %s", s_alt);
    else          fmt_str(label, sizeof label, "Image");
    for (tl = 0; label[tl]; tl++) { }

    cw = pw > 0 ? pw / 12 : tl + 6;      /* a cell is about 12 pixels wide */
    if (cw < tl + 4) cw = tl + 4;
    if (cw < 16) cw = 16;
    if (cw > w) cw = w;
    rows = ph > 0 ? ph / 16 : 3;         /* and 16 high                    */
    if (rows < 3) rows = 3;
    if (rows > 9) rows = 9;
    if (tl > cw - 4) tl = cw - 4;

    newline(1);
    for (y = 0; y < rows; y++) {
        for (x = 0; x < cw; x++) {
            u8 ch;
            if (y == 0)             ch = x == 0 ? FR_TL : x == cw - 1 ? FR_TR : FR_H;
            else if (y == rows - 1) ch = x == 0 ? FR_BL : x == cw - 1 ? FR_BR : FR_H;
            else                    ch = (x == 0 || x == cw - 1) ? FR_V : ' ';
            if (y == rows / 2 && x >= (cw - tl) / 2 && x < (cw - tl) / 2 + tl) {
                i = x - (cw - tl) / 2;
                out_char((u8)label[i], C_DIM, s_linkid);
            } else {
                out_char(ch, C_GREY, s_linkid);
            }
        }
        out_newline();
    }
    s_col = 0;
    s_newlines = 1;
    s_space = 0;
}

static int tag_is(const char *t)
{
    int i;
    for (i = 0; t[i]; i++) if (s_tag[i] != t[i]) return 0;
    return s_tag[i] == 0;
}

/* &amp; inside an address is the one entity that matters there. */
static void decode_amp(char *s)
{
    char *r = s, *w = s;
    while (*r) {
        if (r[0] == '&' && r[1] == 'a' && r[2] == 'm' && r[3] == 'p' && r[4] == ';') {
            *w++ = '&'; r += 5;
        } else {
            *w++ = *r++;
        }
    }
    *w = 0;
}

static u16 add_link(void)
{
    int i;
    if (!s_pg || !s_href[0] || s_pg->nlinks >= PAGE_LINKS) return 0;
    decode_amp(s_href);
    for (i = 0; s_href[i] && i < PAGE_HREF_MAX - 1; i++) s_pg->href[s_pg->nlinks][i] = s_href[i];
    s_pg->href[s_pg->nlinks][i] = 0;
    ++s_pg->nlinks;
    if (s_pg->link_field) s_pg->link_field[s_pg->nlinks] = 0;
    return (u16)s_pg->nlinks;
}

/* ---- forms ------------------------------------------------------------- */
static u32 entity_code(const char *n);
/* An attribute's value with its entities turned into the characters they
 * stand for, as UTF-8, in place: that is what a form sends back, so it
 * has to be exact.  (It only ever gets shorter.) */
static void decode_entities(char *s)
{
    char *r = s, *w = s;
    while (*r) {
        if (*r == '&') {
            char name[12];
            int n = 0;
            const char *q = r + 1;
            while (n < 11 && *q && *q != ';' && *q != '&' && *q != ' ') name[n++] = *q++;
            name[n] = 0;
            if (*q == ';') {
                u32 c = entity_code(name);
                if (c) { w += put_utf8(w, c); r = (char *)q + 1; continue; }
            }
        }
        *w++ = *r++;
    }
    *w = 0;
}

static int eq_ci(const char *a, const char *b)
{
    while (*a && *b) {
        char x = *a++, y = *b++;
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return 0;
    }
    return *a == *b;
}

static int number(const char *t)
{
    int v = 0;
    while (*t >= '0' && *t <= '9') v = v * 10 + (*t++ - '0');
    return v;
}

/* A field of the form being read, its name and value into the pool with
 * room for vcap bytes of value.  -1 if there is no form, or no room. */
static int add_field(int kind, const char *val, int vlen, int vcap)
{
    Page *p = s_pg;
    PageField *f;
    int nlen = 0, i;

    if (!p || !p->fields || s_form < 0 || p->nfields >= PAGE_FIELDS) return -1;
    while (s_fname[nlen]) nlen++;
    if (vcap < vlen) vcap = vlen;
    if (p->pool_used + (u32)nlen + (u32)vcap > PAGE_POOL) return -1;
    f = &p->fields[p->nfields];
    f->form = (u8)s_form; f->kind = (u8)kind; f->link = 0;
    f->line = f->col = f->width = 0;
    f->name = p->pool_used; f->nlen = (u32)nlen;
    for (i = 0; i < nlen; i++) p->pool[p->pool_used++] = s_fname[i];
    f->val = p->pool_used; f->vlen = (u32)vlen; f->vcap = (u32)vcap;
    for (i = 0; i < vlen; i++) p->pool[p->pool_used + (u32)i] = val[i];
    p->pool_used += (u32)vcap;
    return p->nfields++;
}

/* A link number for a field, so tab can reach it and enter acts on it. */
static u16 field_link(int fi)
{
    Page *p = s_pg;
    if (p->nlinks >= PAGE_LINKS) return 0;
    p->href[p->nlinks][0] = 0;
    p->nlinks++;
    p->link_field[p->nlinks] = (u16)(fi + 1);
    p->fields[fi].link = (u16)p->nlinks;
    return (u16)p->nlinks;
}

/* Room on this line for n more cells, or a new line. */
static void make_room(int n)
{
    int w = out_width();
    flush_word();
    if (s_col && s_col + 1 + n > w) { out_newline(); s_col = 0; }
    else if (s_col && s_space) { out_char(' ', C_TEXT, 0); s_col++; }
}

/* A box to type into: [________], its value shown in it. */
static void field_box(int fi, int width)
{
    Page *p = s_pg;
    PageField *f = &p->fields[fi];
    u16 id = field_link(fi);
    int w = out_width(), i;

    if (width > w - 4) width = w - 4;
    if (width < 6) width = 6;
    make_room(width + 2);
    out_char('[', C_GREY, id); s_col++;
    f->line = (u16)(p->lines - 1);
    f->col = (u16)p->col;
    f->width = (u16)width;
    for (i = 0; i < width; i++) { out_char('_', C_DIM, id); s_col++; }
    out_char(']', C_GREY, id); s_col++;
    s_space = 1;
    s_newlines = 0;
    page_field_show(p, fi);
}

/* What goes into a box: the tail of the value, as glyphs, then
 * underscores.  A password is stars. */
void page_field_show(Page *pg, int fi)
{
    PageField *f = &pg->fields[fi];
    PageCell *c;
    const u8 *v = (const u8 *)pg->pool + f->val;
    u32 k = 0;
    int n = 0, skip, i;

    if (!f->width) return;
    for (k = 0; k < f->vlen; k++) if ((v[k] & 0xC0) != 0x80) n++;     /* characters */
    skip = n > f->width - 1 ? n - (f->width - 1) : 0;                 /* room for the caret */
    c = &pg->cells[f->line * pg->width + f->col];
    for (i = 0; i < f->width; i++) { c[i].ch = '_'; c[i].col = C_DIM; }
    for (k = 0, i = 0; k < f->vlen; ) {
        u32 cp = v[k++];
        if (cp >= 0xC0) {
            int more = cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : 1;
            cp &= more == 3 ? 0x07 : more == 2 ? 0x0F : 0x1F;
            while (more-- && k < f->vlen) cp = (cp << 6) | (v[k++] & 0x3F);
        }
        if (skip) { skip--; continue; }
        if (i >= f->width) break;
        c[i].ch = f->kind == FK_PASSWORD ? '*' : cp == '\n' ? ' ' : glyph_for(cp);
        c[i].col = C_BRIGHT;
        i++;
    }
}

/* [ Send ], a button that sends the form. */
static void submit_button(const char *label)
{
    int fi, i, n = 0;
    u16 id;
    char lab[40];

    decode_entities(s_val);
    for (n = 0; s_val[n]; n++) { }
    fi = add_field(FK_SUBMIT, s_val, n, n);
    if (fi < 0) return;
    id = field_link(fi);
    for (i = 0; label[i] && i < 36; i++) lab[i] = label[i];
    lab[i] = 0;
    make_room(i + 4);
    out_char('[', C_GREY, id); out_char(' ', C_ACCENT, id); s_col += 2;
    for (i = 0; lab[i]; i++) {
        u32 cp = (u8)lab[i];
        if (cp >= 0xC0 && (lab[i + 1] & 0xC0) == 0x80) { cp = ((cp & 0x1F) << 6) | (lab[i + 1] & 0x3F); i++; }
        out_char(glyph_for(cp), C_ACCENT, id); s_col++;
    }
    out_char(' ', C_ACCENT, id); out_char(']', C_GREY, id); s_col += 2;
    s_space = 1;
    s_newlines = 0;
}

static void input_tag(void)
{
    int n;
    decode_entities(s_val);
    for (n = 0; s_val[n]; n++) { }
    if (eq_ci(s_type, "hidden")) {
        (void)add_field(FK_HIDDEN, s_val, n, n);
    } else if (eq_ci(s_type, "submit") || eq_ci(s_type, "image")) {
        if (s_has_val && s_val[0]) {
            static char lab[40];
            int i;
            for (i = 0; s_val[i] && i < 39; i++) lab[i] = s_val[i];
            lab[i] = 0;
            submit_button(lab);
        } else
            submit_button("Send");
    } else if (!s_type[0] || eq_ci(s_type, "text") || eq_ci(s_type, "search") ||
               eq_ci(s_type, "email") || eq_ci(s_type, "url") || eq_ci(s_type, "tel") ||
               eq_ci(s_type, "number") || eq_ci(s_type, "password")) {
        int fi = add_field(eq_ci(s_type, "password") ? FK_PASSWORD : FK_TEXT, s_val, n, n + 512);
        int w = s_size[0] ? number(s_size) : 24;
        if (fi >= 0) field_box(fi, w);
    }
    /* Checkboxes, radio buttons, files, and buttons that only do
     * something with JavaScript: not yet. */
}

static void tag_ended(void)
{
    static const char *block[] = { "p", "div", "tr", "ul", "ol", "table",
        "section", "article", "header", "footer", "nav", "pre", "blockquote",
        "dt", "dd", "form", "main", "center", "aside", "figure", 0 };
    int i;

    if (tag_is("script") || tag_is("style") || tag_is("svg") || tag_is("template")) {
        if (!s_tag_close) {
            for (i = 0; s_tag[i]; i++) s_skip_until[i] = s_tag[i];
            s_skip_until[i] = 0;
            s_hstate = H_SKIP;
            return;
        }
    }
    if (tag_is("form")) {
        newline(2);
        if (s_tag_close) { s_form = -1; return; }
        s_form = -1;
        if (s_pg && s_pg->forms && s_pg->nforms < PAGE_FORMS) {
            PageForm *f = &s_pg->forms[s_pg->nforms];
            int k;
            decode_entities(s_action);
            for (k = 0; s_action[k] && k < PAGE_HREF_MAX - 1; k++) f->action[k] = s_action[k];
            f->action[k] = 0;
            f->post = eq_ci(s_method, "post");
            s_form = s_pg->nforms++;
        }
        return;
    }
    if (tag_is("input")) { if (!s_tag_close) input_tag(); return; }
    if (tag_is("textarea")) {
        if (!s_tag_close) {
            int fi = add_field(FK_AREA, "", 0, 4096);
            s_area_w = s_size[0] ? number(s_size) : 48;
            if (fi >= 0) { newline(1); s_area = fi; s_area_fresh = 1; }
        } else if (s_area >= 0) {
            int fi = s_area;
            s_area = -1;
            field_box(fi, s_area_w);
            newline(1);
        }
        return;
    }
    if (tag_is("button")) {
        /* Its own words are its label: they become the link, in brackets. */
        if (!s_tag_close) {
            int n, fi;
            if (eq_ci(s_type, "button") || eq_ci(s_type, "reset")) return;
            decode_entities(s_val);
            for (n = 0; s_val[n]; n++) { }
            fi = add_field(FK_SUBMIT, s_val, n, n);
            if (fi < 0) return;
            flush_word();
            s_linkid = field_link(fi);
            s_link = 1;
            s_button = fi;
            push('[', C_GREY);
        } else if (s_button >= 0) {
            push(']', C_GREY);
            flush_word();
            s_link = 0;
            s_linkid = 0;
            s_button = -1;
        }
        return;
    }
    if (tag_is("title")) {
        if (s_pg) { s_in_title = !s_tag_close; return; }
        newline(2);
        s_bold = !s_tag_close;
        if (s_tag_close) newline(1);
        return;
    }
    if (s_tag[0] == 'h' && s_tag[1] >= '1' && s_tag[1] <= '6' && !s_tag[2]) {
        newline(2);
        s_bold = !s_tag_close;
        if (s_tag_close) newline(1);
        return;
    }
    if (tag_is("a")) {
        if (s_tag_close) { flush_word(); s_link = 0; s_linkid = 0; }
        else if (s_href[0]) { flush_word(); s_link = 1; s_linkid = add_link(); }
        return;
    }
    if (tag_is("img")) { image(); return; }
    if (tag_is("hr")) { rule(); return; }
    if (tag_is("br")) { newline(1); s_newlines = 0; return; }
    if (tag_is("li")) {
        if (!s_tag_close) { newline(1); emit_text("* ", C_DIM); }
        return;
    }
    for (i = 0; block[i]; i++)
        if (tag_is(block[i])) { newline(2); return; }
}

/* An entity's character: "amp", "#233", "#xE9".  0 if it is not one. */
static u32 entity_code(const char *n)
{
    static const struct { const char *n; u32 c; } e[] = {
        { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' },
        { "apos", '\'' }, { "nbsp", 0xA0 }, { "aring", 0xE5 },
        { "auml", 0xE4 }, { "ouml", 0xF6 }, { "Aring", 0xC5 },
        { "Auml", 0xC4 }, { "Ouml", 0xD6 }, { "eacute", 0xE9 },
        { "copy", 0xA9 }, { "mdash", 0x2014 }, { "ndash", 0x2013 },
        { "hellip", 0x2026 }, { "rsquo", 0x2019 }, { "lsquo", 0x2018 },
        { "ldquo", 0x201C }, { "rdquo", 0x201D }, { "bull", 0x2022 },
        { "middot", 0xB7 }, { 0, 0 } };
    int i, j;

    if (n[0] == '#') {
        u32 v = 0;
        int hex = (n[1] == 'x' || n[1] == 'X'), any = 0;
        for (i = hex ? 2 : 1; n[i]; i++) {
            char c = n[i];
            int d = (c >= '0' && c <= '9') ? c - '0' :
                    (hex && c >= 'a' && c <= 'f') ? c - 'a' + 10 :
                    (hex && c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
            if (d < 0) return 0;
            v = v * (hex ? 16 : 10) + (u32)d;
            any = 1;
        }
        return any && v < 0x110000 ? v : 0;
    }
    for (i = 0; e[i].n; i++) {
        for (j = 0; e[i].n[j] && e[i].n[j] == n[j]; j++) { }
        if (!e[i].n[j] && !n[j]) return e[i].c;
    }
    return 0;
}

static void entity_ended(void)
{
    u32 c;
    int i;
    s_ent[s_ent_len] = 0;
    c = entity_code(s_ent);
    if (c) { emit(c); return; }
    emit('&');                           /* not one we know: as written    */
    for (i = 0; s_ent[i]; i++) emit((u8)s_ent[i]);
}

static void attr_begin_value(void)
{
    s_aname[s_aname_len] = 0;
    s_aval = 0;
    if (s_aname_len == 4 && s_aname[0] == 'h' && s_aname[1] == 'r' &&
        s_aname[2] == 'e' && s_aname[3] == 'f') {
        s_aval = s_href; s_aval_max = PAGE_HREF_MAX;
    } else if (s_aname_len == 3 && s_aname[0] == 'a' && s_aname[1] == 'l' &&
               s_aname[2] == 't') {
        s_aval = s_alt; s_aval_max = (int)sizeof s_alt;
    } else if (s_aname_len == 5 && s_aname[0] == 'w' && s_aname[1] == 'i' &&
               s_aname[2] == 'd' && s_aname[3] == 't' && s_aname[4] == 'h') {
        s_aval = s_wattr; s_aval_max = (int)sizeof s_wattr;
    } else if (s_aname_len == 6 && s_aname[0] == 'h' && s_aname[1] == 'e' &&
               s_aname[2] == 'i' && s_aname[3] == 'g' && s_aname[4] == 'h' && s_aname[5] == 't') {
        s_aval = s_hattr; s_aval_max = (int)sizeof s_hattr;
    }
    else {
        static const struct { const char *n; char *buf; int max; } more[] = {
            { "action", s_action, PAGE_HREF_MAX }, { "method", s_method, 8 },
            { "type", s_type, 12 }, { "name", s_fname, 64 },
            { "value", s_val, VAL_MAX }, { "size", s_size, 6 }, { "cols", s_size, 6 },
        };
        int k, j;
        for (k = 0; k < (int)(sizeof more / sizeof more[0]); k++) {
            for (j = 0; more[k].n[j] && j < s_aname_len && more[k].n[j] == s_aname[j]; j++) { }
            if (!more[k].n[j] && j == s_aname_len) {
                s_aval = more[k].buf; s_aval_max = more[k].max;
                if (more[k].buf == s_val) s_has_val = 1;
                break;
            }
        }
    }
    s_aval_len = 0;
    if (s_aval) s_aval[0] = 0;
}

static void attr_byte(u8 b)
{
    if (s_aval && s_aval_len < s_aval_max - 1) {
        s_aval[s_aval_len++] = (char)b;
        s_aval[s_aval_len] = 0;
    }
}

static void tag_start(void)
{
    s_tag_len = 0; s_tag_close = 0; s_tag_done = 0;
    s_astate = A_NONE;
    s_href[0] = 0;
    s_alt[0] = 0;
    s_wattr[0] = s_hattr[0] = 0;
    s_action[0] = s_method[0] = s_type[0] = s_fname[0] = s_size[0] = 0;
    s_val[0] = 0;
    s_has_val = 0;
    s_hstate = H_TAG;
}

/* One byte of the page, as it arrives. */
static void html_byte(u8 b)
{
    int ws = (b == ' ' || b == '\t' || b == '\n' || b == '\r');

    switch (s_hstate) {
    case H_TAG:
        /* Inside a quoted attribute a '>' is just a character. */
        if (s_astate == A_QVAL) {
            if ((char)b == s_quote) s_astate = A_NONE;
            else attr_byte(b);
            return;
        }
        if (b == '>') {
            s_tag[s_tag_len] = 0;
            s_hstate = H_TEXT;
            tag_ended();
            return;
        }
        if (!s_tag_done) {
            if (b == '/' && s_tag_len == 0 && !s_tag_close) s_tag_close = 1;
            else if (ws || b == '/') s_tag_done = s_tag_len > 0;
            else if (s_tag_len < (int)sizeof s_tag - 1)
                s_tag[s_tag_len++] = (char)((b >= 'A' && b <= 'Z') ? b + 32 : b);
            return;
        }
        switch (s_astate) {
        case A_NONE:
            if (!ws && b != '/') {
                s_aname_len = 0;
                s_aname[s_aname_len++] = (char)((b >= 'A' && b <= 'Z') ? b + 32 : b);
                s_astate = A_NAME;
            }
            return;
        case A_NAME:
            if (b == '=') { attr_begin_value(); s_astate = A_EQ; }
            else if (ws) s_astate = A_NONE;
            else if (s_aname_len < (int)sizeof s_aname - 1)
                s_aname[s_aname_len++] = (char)((b >= 'A' && b <= 'Z') ? b + 32 : b);
            else s_aname_len = (int)sizeof s_aname;        /* too long to be one of ours */
            return;
        case A_EQ:
            if (ws) return;
            if (b == '"' || b == '\'') { s_quote = (char)b; s_astate = A_QVAL; return; }
            s_astate = A_VAL;
            attr_byte(b);
            return;
        case A_VAL:
            if (ws) s_astate = A_NONE;
            else attr_byte(b);
            return;
        }
        return;

    case H_SKIP:
        /* Inside <script> and the like: nothing shown until its own end
         * tag.  Watching for "</" and the name is enough. */
        if (b == '<') { s_ent_len = -1; return; }
        if (s_ent_len == -1) { s_ent_len = (b == '/') ? -2 : 0; return; }
        if (s_ent_len <= -2) {
            int k = -s_ent_len - 2;
            char lc = (char)((b >= 'A' && b <= 'Z') ? b + 32 : b);
            if (s_ent_len == -100) {
                if (b == '>') { s_hstate = H_TEXT; s_ent_len = 0; }
            } else if (s_skip_until[k] && lc == s_skip_until[k]) {
                s_ent_len--;
                if (!s_skip_until[k + 1]) s_ent_len = -100;
            } else {
                s_ent_len = 0;
            }
        }
        return;

    case H_ENT:
        if (b == ';') { s_hstate = H_TEXT; entity_ended(); return; }
        if (s_ent_len < (int)sizeof s_ent - 1 &&
            ((b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') ||
             (b >= '0' && b <= '9') || b == '#')) {
            s_ent[s_ent_len++] = (char)b;
            return;
        }
        s_hstate = H_TEXT;
        s_ent[s_ent_len] = 0;
        emit('&');
        { int i; for (i = 0; s_ent[i]; i++) emit((u8)s_ent[i]); }
        break;                           /* and on, to treat b as text     */

    default:
        break;
    }

    if (b == '<') { tag_start(); return; }
    if (b == '&') { s_ent_len = 0; s_hstate = H_ENT; return; }

    /* UTF-8, or Latin-1 if it does not hold together as UTF-8. */
    if (s_cp_left) {
        if ((b & 0xC0) == 0x80) {
            s_cp = (s_cp << 6) | (b & 0x3F);
            if (--s_cp_left == 0) emit(s_cp);
            return;
        }
        s_cp_left = 0;
        emit(0xFFFD);
    }
    if (b >= 0xC2 && b <= 0xDF) { s_cp = b & 0x1F; s_cp_left = 1; return; }
    if (b >= 0xE0 && b <= 0xEF) { s_cp = b & 0x0F; s_cp_left = 2; return; }
    if (b >= 0xF0 && b <= 0xF4) { s_cp = b & 0x07; s_cp_left = 3; return; }
    emit(b);
}

static void filter_reset(int html)
{
    s_html = html;
    s_hstate = H_TEXT;
    s_word_len = s_col = s_space = 0;
    s_newlines = 1;
    s_bold = s_link = s_dim = s_in_title = 0;
    s_linkid = 0;
    s_cp_left = 0;
    s_form = s_area = s_button = -1;
}

static void body_byte(u8 b)
{
    if (s_html) { html_byte(b); return; }
    if (b == '\n') { flush_word(); out_newline(); s_col = 0; s_newlines = 1; return; }
    if (b == '\r') return;
    if (b == ' ') { flush_word(); s_space = 1; return; }
    if (s_cp_left || b >= 0x80) {            /* decode only, no tags in text */
        if (s_cp_left && (b & 0xC0) == 0x80) {
            s_cp = (s_cp << 6) | (b & 0x3F);
            if (--s_cp_left == 0) emit(s_cp);
            return;
        }
        s_cp_left = 0;
        if (b >= 0xC2 && b <= 0xDF) { s_cp = b & 0x1F; s_cp_left = 1; return; }
        if (b >= 0xE0 && b <= 0xEF) { s_cp = b & 0x0F; s_cp_left = 2; return; }
        emit(b);
        return;
    }
    emit(b);
}

/* A page written by FLOPOS itself, through the same reader. */
void http_page_html(const char *html, Page *pg)
{
    Page *keep = s_pg;
    s_pg = pg;
    page_clear(pg);
    filter_reset(1);
    while (*html) html_byte((u8)*html++);
    flush_word();
    s_pg = keep;
}

/* ---- the request and the answer ------------------------------------------- */
static int lower_eq(const char *a, const char *b, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
    }
    return 1;
}

static int stop_asked(void) { return ui_key() == UK_ESC; }

/* The page comes over plain TCP or over TLS; everything here reads and
 * writes the same way either way. */
static int s_secure;
static int s_status;                     /* the last answer's status       */
static int conn_write(const u8 *d, int n) { return A->conn_write(d, n); }
static int conn_read(u8 *b, int n, int w) { return A->conn_read(b, n, w); }
static void conn_close(void) { A->conn_close(); }

/* Returns 0 for a page, 1 with moved filled in for a redirect, -1 for a
 * failure that has already been explained. */
/* post: a form's contents to send, or 0 for an ordinary request. */
static int fetch_once(const Url *u, char *moved, int moved_max, const u8 *post, int postlen)
{
    static u8 buf[1024];
    static char head[2048];
    char req[PAGE_HREF_MAX + 200], line[96];
    int head_len = 0, in_body = 0, status = 0, n, i, html = 1, counting = 0;
    u32 body = 0, said = 0;
    char ctype[48];

    ctype[0] = 0;
    moved[0] = 0;

    s_secure = u->secure;
    if (s_pg) { s_pg->checked = 0; s_pg->untrusted = 0; s_pg->who[0] = 0; }
    if (!s_pg) main_pump(1);
    if (!A->conn_open(u->host, u->port, u->secure, tls_note)) {
        ConnInfo ci;
        A->conn_info(&ci);
        if (s_pg && u->secure) s_pg->untrusted = ci.untrusted;
        note(A->conn_why(), C_ACCENT);
        return -1;
    }
    if (u->secure) {
        ConnInfo ci;
        A->conn_info(&ci);
        if (!s_pg) {
            if (ci.checked)
                fmt_str(line, sizeof line, "encrypted%s, and it is who it says: %s vouches for it",
                        ci.pq ? ", post-quantum" : "", ci.who);
            else
                fmt_str(line, sizeof line, "encrypted%s -- but NOT checked: it could be anybody",
                        ci.pq ? ", post-quantum" : "");
            note(line, ci.checked ? C_DIM : C_ACCENT);
        } else {
            int k;
            s_pg->secure = 1;
            s_pg->pq = ci.pq;
            s_pg->checked = ci.checked;
            for (k = 0; ci.who[k] && k < (int)sizeof s_pg->who - 1; k++) s_pg->who[k] = ci.who[k];
            s_pg->who[k] = 0;
        }
    }

    if (post)
        fmt_str(req, sizeof req,
                "POST %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: FLOPOS/%s (a 1996 Toshiba)\r\n"
                "Accept: text/html, text/plain, */*\r\n"
                "Content-Type: application/x-www-form-urlencoded\r\nContent-Length: %u\r\n"
                "Connection: close\r\n\r\n",
                u->path, u->host, A->os_version(), (u32)postlen);
    else
        fmt_str(req, sizeof req,
                "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: FLOPOS/%s (a 1996 Toshiba)\r\n"
                "Accept: text/html, text/plain, */*\r\nConnection: close\r\n\r\n",
                u->path, u->host, A->os_version());
    for (n = 0; req[n]; n++) { }
    conn_write((const u8 *)req, n);
    for (i = 0; post && i < postlen; i += 4096)        /* the form itself   */
        conn_write(post + i, postlen - i < 4096 ? postlen - i : 4096);
    s_status = 0;

    filter_reset(1);

    for (;;) {
        n = conn_read(buf, sizeof buf, 15000);
        if (n == 0) break;                               /* they finished  */
        if (n < 0) {
            flush_word();
            if (n == -1) note("It went quiet, so I stopped waiting.", C_ACCENT);
            else {
                fmt_str(line, sizeof line, "Cut off: %s.", A->conn_why());
                note(line, C_ACCENT);
            }
            break;
        }

        for (i = 0; i < n; i++) {
            if (in_body) {
                body++;
                if (!counting) body_byte(buf[i]);
                continue;
            }

            if (head_len < (int)sizeof head - 1) head[head_len++] = (char)buf[i];
            if (head_len >= 4 && head[head_len - 1] == '\n' &&
                (head[head_len - 2] == '\n' ||
                 (head[head_len - 2] == '\r' && head[head_len - 3] == '\n'))) {
                char *ln = head, *end;
                head[head_len] = 0;
                in_body = 1;

                for (end = ln; *end && *end != ' '; end++) { }   /* HTTP/1.1 */
                while (*end == ' ') end++;
                while (*end >= '0' && *end <= '9') status = status * 10 + (*end++ - '0');

                for (ln = head; *ln; ) {
                    for (end = ln; *end && *end != '\n'; end++) { }
                    if (lower_eq(ln, "location:", 9)) {
                        char *v = ln + 9; int k = 0;
                        while (*v == ' ') v++;
                        while (v < end && *v != '\r' && k < moved_max - 1) moved[k++] = *v++;
                        moved[k] = 0;
                    } else if (lower_eq(ln, "content-type:", 13)) {
                        char *v = ln + 13; int k = 0;
                        while (*v == ' ') v++;
                        while (v < end && *v != '\r' && *v != ';' && k < 47) ctype[k++] = *v++;
                        ctype[k] = 0;
                    }
                    ln = *end ? end + 1 : end;
                }
                if (s_pg) s_pg->status = status;
                s_status = status;

                if (status >= 300 && status < 400 && moved[0]) {
                    conn_close();
                    return 1;
                }
                fmt_str(line, sizeof line, "%u  %s", (u32)status, ctype[0] ? ctype : "(no type given)");
                note(line, status >= 200 && status < 300 ? C_DIM : C_ACCENT);

                html = lower_eq(ctype, "text/html", 9) ||
                       lower_eq(ctype, "application/xhtml", 17);
                if (!html && !lower_eq(ctype, "text/", 5) && ctype[0]) {
                    note("Not text, so not shown. Counting the bytes instead.", C_DIM);
                    counting = 1;
                } else {
                    filter_reset(html);
                }
            }
        }

        if (s_pg) {
            s_pg->bytes = body;
            if (body - said >= 2048) {
                said = body;
                fmt_str(line, sizeof line, "loading... %u bytes", body);
                note(line, C_DIM);
            }
        }
        if (stop_asked()) {
            flush_word();
            note("Stopped.", C_ACCENT);
            break;
        }
        main_pump(0);
    }

    flush_word();
    conn_close();
    if (!s_pg) {
        if (s_col) term_newline();
        fmt_str(line, sizeof line, "%u bytes.", body);
        note(line, C_DIM);
    } else {
        s_pg->bytes = body;
    }
    return 0;
}

/* Fetch url, following redirects, into the terminal (pg 0) or a page.
 * The address it finally came from goes into pg->url, which is what
 * relative links on it are relative to. */
static int get(const char *url, Page *pg, const u8 *post, int postlen)
{
    Url u;
    char moved[URL_PATH_MAX];
    int hops, r;
    Page *keep = s_pg;

    s_pg = pg;
    if (pg) page_clear(pg);
    if (!url_parse(url, &u)) {
        note("That does not read as a web address.", C_ACCENT);
        s_pg = keep;
        return 0;
    }

    if (!pg) term_plain(1);
    for (hops = 0; hops < 5; hops++) {
        if (pg) url_text(&u, pg->url, (int)sizeof pg->url);
        r = fetch_once(&u, moved, sizeof moved, post, postlen);
        if (r != 1) {
            if (!pg) term_plain(0);
            s_pg = keep;
            return r == 0;
        }
        if (!pg) {
            term_colour(C_DIM);
            term_print("moved to ");
            term_print(moved);
            term_newline();
        }
        /* After a form, "see over there" means go and look, not send it
         * again -- except 307 and 308, which mean send it there too. */
        if (s_status != 307 && s_status != 308) { post = 0; postlen = 0; }
        if (moved[0] == '/' && moved[1] != '/') {        /* same server    */
            int i;
            for (i = 0; moved[i] && i < PAGE_HREF_MAX - 1; i++) u.path[i] = moved[i];
            u.path[i] = 0;
        } else if (moved[0] == '/' && moved[1] == '/') {  /* same scheme    */
            char full[PAGE_HREF_MAX + 8];
            fmt_str(full, sizeof full, "%s:%s", u.secure ? "https" : "http", moved);
            if (!url_parse(full, &u)) break;
        } else if (!url_parse(moved, &u)) {
            break;
        }
        if (pg) page_clear(pg);
    }
    note("It keeps sending me somewhere else. Giving up.", C_ACCENT);
    if (!pg) term_plain(0);
    s_pg = keep;
    return 0;
}

void http_fetch(const char *url) { (void)get(url, 0, 0, 0); }
int  http_page(const char *url, Page *pg) { return get(url, pg, 0, 0); }
int  http_load(const char *url, Page *pg, const u8 *post, int postlen)
{
    return get(url, pg, post, postlen);
}
