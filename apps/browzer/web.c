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
#include "../app_glyphs.h"

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

static void out_char(int ch, u8 col, u16 link)
{
    PageCell *c;
    if (!s_pg) { term_colour(col); term_putc(ch); return; }
    if (s_pg->full || s_pg->col >= s_pg->width) return;
    c = &s_pg->cells[(s_pg->lines - 1) * s_pg->width + s_pg->col++];
    c->ch = ch; c->col = col; c->link = link;
}

static int s_select, s_opt;              /* a list being read (below)       */

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
    pg->nopts = 0;
    s_select = s_opt = -1;
    pg->pool_used = 0;
    pg->title[0] = 0;
    pg->note[0] = 0;
    pg->bytes = 0;
    pg->status = 0;
    pg->refresh = 0;
    pg->refresh_url[0] = 0;
    pg->gen++;
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
static u16  s_word[80];
static u8   s_word_col[80];
static u16  s_word_link[80];
static int  s_word_len, s_col, s_newlines, s_space;
static int  s_bold, s_link, s_dim, s_in_title, s_kbd;
static u16  s_linkid;
static u32  s_cp, s_cp_left;

/* Attributes: only href and alt are kept. */
static int  s_astate;
static char s_aname[12];
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
static int  s_select = -1;               /* the <select> being read; -2 one with no room */
static int  s_opt = -1, s_opt_val, s_opt_sel;   /* the <option> being read     */
static int  s_checked;                   /* the tag says checked or selected */
#define OPT_LAB 60                       /* bytes of a choice's words kept  */
static char s_alt[60];
static char s_equiv[16], s_content[256], s_src[PAGE_HREF_MAX];   /* meta, frames */
static int  s_refresh_go;                 /* a refresh of 0 s elsewhere: stop reading */
static char s_wattr[8], s_hattr[8];      /* an image's width and height    */
static char *s_aval;
static int  s_aval_len, s_aval_max;

static u8 colour_now(void)
{
    return s_dim ? C_DIM : s_kbd ? C_KEY : s_bold ? C_BRIGHT : s_link ? C_ACCENT : C_TEXT;
}

static int  s_word_cont;                 /* the word goes on past what is flushed */
static void bare_link(void);

static void flush_word(void)
{
    int i, w = out_width();

    if (!s_word_len) return;
    bare_link();
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

static void push(int g, u8 col)
{
    if (s_word_len < (int)(sizeof s_word / sizeof *s_word)) {
        s_word[s_word_len] = g;
        s_word_col[s_word_len] = col;
        s_word_link[s_word_len] = s_linkid;
        s_word_len++;
    }
    if (s_word_len == (int)(sizeof s_word / sizeof *s_word)) { s_word_cont = 1; flush_word(); s_word_cont = 0; }
}

static int glyph_for(u32 c)
{
    int f = (c >= 0x80) ? a_from_unicode(c) : 0;
    if (f && (f < A_CH_WIDE || a_drawable((unsigned)f))) return f;   /* the font has it */
    switch (c) {
    case 0xE3: case 0xE6: return 'a';    case 0xC3: case 0xC6: return 'A';
    case 0xF5: case 0xF8: return 'o';    case 0xD5: case 0xD8: return 'O';
    case 0xFD: return 'y';               case 0xDD: return 'Y';
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
    /* Inside a drop-down list, the words are its choices'; anything
     * between them is not shown. */
    if (s_select != -1 && s_pg) {
        if (s_opt >= 0) {
            PageOption *o = &s_pg->opts[s_opt];
            char b[4];
            int n, k;
            if (c == ' ' || c == '\n' || c == '\t' || c == '\r' || c == 0xA0) {
                if (!o->llen || s_pg->pool[o->lab + o->llen - 1] == ' ') return;
                c = ' ';
            }
            n = put_utf8(b, c);
            if (o->llen + (u32)n <= OPT_LAB)
                for (k = 0; k < n; k++) s_pg->pool[o->lab + o->llen++] = b[k];
        }
        return;
    }
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
            } else if (n < 59) {             /* room for a wide one: 4 bytes  */
                n += a_ch_put(s_pg->title + n, glyph_for(c));
                s_pg->title[n] = 0;
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
            int ch;
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

static int eq_ci(const char *a, const char *b);
static int lower_eq(const char *a, const char *b, int n);
static int conn_write(const u8 *d, int n);
static int conn_read(u8 *b, int n, int w);
static void conn_close(void);

/* One line down the serial cable, for whoever is testing: what a page
 * turned out to be, and the links browzer made itself. */
static void wire_line(const char *what, const char *t)
{
    char line[PAGE_HREF_MAX + 80];
    fmt_str(line, sizeof line, "[page] %s%s\n", what, t);
    A->wire(line);
}

/* A link made from an address, not from s_href (which a tag may be using). */
static u16 add_link_to(const char *u)
{
    int i;
    if (!s_pg || !u[0] || s_pg->nlinks >= PAGE_LINKS) return 0;
    for (i = 0; u[i] && i < PAGE_HREF_MAX - 1; i++) s_pg->href[s_pg->nlinks][i] = u[i];
    s_pg->href[s_pg->nlinks][i] = 0;
    ++s_pg->nlinks;
    if (s_pg->link_field) s_pg->link_field[s_pg->nlinks] = 0;
    return (u16)s_pg->nlinks;
}

static int is_http(const u16 *w, int n)
{
    return n > 10 && (w[0] | 32) == 'h' && w[1] == 't' && w[2] == 't' && w[3] == 'p' &&
           ((w[4] == ':' && w[5] == '/' && w[6] == '/') || (w[4] == 's' && w[5] == ':' && w[6] == '/' && w[7] == '/'));
}

/* An address written out as text, not as a link -- 72fm.com shows its
 * stations' stream addresses so -- is made a link to itself.  A long one
 * arrives in pieces of a word's length; s_bare is its link meanwhile. */
static u16 s_bare;

static void bare_link(void)
{
    int i, k;
    if (!s_pg) { s_bare = 0; return; }
    if (s_bare) {                                  /* the rest of a long one */
        char *h = s_pg->href[s_bare - 1];
        for (k = 0; h[k]; k++) { }
        for (i = 0; i < s_word_len && k < PAGE_HREF_MAX - 1; i++) h[k++] = s_word[i] < 0x80 ? (char)s_word[i] : '?';
        h[k] = 0;
        for (i = 0; i < s_word_len; i++) { s_word_link[i] = s_bare; s_word_col[i] = C_ACCENT; }
    } else if (!s_word_link[0] && is_http(s_word, s_word_len)) {
        char u[PAGE_HREF_MAX];
        u16 id;
        for (i = 0; i < s_word_len && i < PAGE_HREF_MAX - 1; i++) u[i] = s_word[i] < 0x80 ? (char)s_word[i] : '?';
        u[i] = 0;
        id = add_link_to(u);
        if (id) {
            s_bare = id;
            for (i = 0; i < s_word_len; i++) { s_word_link[i] = id; s_word_col[i] = C_ACCENT; }
        }
    }
    if (s_bare && !s_word_cont) {
        /* It has ended.  A full stop or bracket after it is the sentence's. */
        char *h = s_pg->href[s_bare - 1];
        for (k = 0; h[k]; k++) { }
        while (k > 0 && (h[k - 1] == '.' || h[k - 1] == ',' || h[k - 1] == ')' || h[k - 1] == ';' ||
                         h[k - 1] == ':' || h[k - 1] == '!' || h[k - 1] == '?' || h[k - 1] == '"' || h[k - 1] == '\''))
            h[--k] = 0;
        wire_line("an address in the text: ", h);
        s_bare = 0;
    }
}

/* ---- sound a page names without a link to it ----------------------------
 * A player written in JavaScript keeps its stream's address in a script,
 * or in a data attribute, as JSON: "stream":"/sc/stream/1/", or
 * "url":"https:\/\/host\/show.mp3".  browzer runs no scripts.  It only
 * reads the page as it arrives, as plain text, for such addresses, and
 * lists them at the end as links.  Nothing in them is run. */
#define SNIFF_MAX 12
static char s_sniffed[SNIFF_MAX][PAGE_HREF_MAX];
static int  s_nsniffed;
static char s_sn[PAGE_HREF_MAX];         /* the address being read          */
static int  s_sn_len, s_sn_on;           /* 1 an absolute address, 2 a "stream" key's value */
static int  s_azura;                     /* the page is an AzuraCast station's */
static char s_rb[PAGE_HREF_MAX];         /* the Radio Browser directory it names */
static char s_ring[32];                  /* the page's last 32 bytes        */
static u32  s_ring_at;

static int ring_ends(const char *p)
{
    int n = 0, i;
    while (p[n]) n++;
    for (i = 0; i < n; i++) {
        char c = s_ring[(s_ring_at - 1 - (u32)i) & 31], want = p[n - 1 - i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if (c != want) return 0;
    }
    return 1;
}

static int has_at(const char *u, int n, const char *w)
{
    int i, k;
    for (i = 0; i < n; i++) {
        for (k = 0; w[k] && i + k < n; k++) {
            char c = u[i + k];
            if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
            if (c != w[k]) break;
        }
        if (!w[k]) return 1;
    }
    return 0;
}

/* Does it look like something to listen to? */
static int sounds(const char *u, int mode)
{
    static const char *const ext[] = { ".mp3", ".aac", ".aacp", ".ogg", ".oga", ".flac", ".wav",
                                       ".m3u", ".pls", ".mod", ".xm", ".s3m", ".it", 0 };
    int n = 0, k, e;
    while (u[n] && u[n] != '?' && u[n] != '#') n++;      /* the path's end   */
    for (k = 0; ext[k]; k++) {
        for (e = 0; ext[k][e]; e++) { }
        if (n > e && has_at(u + n - e, e, ext[k])) return 1;
    }
    if (has_at(u, n, "/stream") || has_at(u, n, "/;") || has_at(u, n, "icecast") || has_at(u, n, "shoutcast"))
        return 1;
    return mode == 2 && has_at(u, n, "/");                /* a stream key's address */
}

static void sniff_end(void)
{
    char u[PAGE_HREF_MAX];
    int i, k = 0, mode = s_sn_on;
    s_sn_on = 0;
    s_sn[s_sn_len] = 0;
    /* JSON's \/ and \u0026, and HTML's &amp; */
    for (i = 0; s_sn[i] && k < PAGE_HREF_MAX - 1; i++) {
        const char *q = s_sn + i;
        if (q[0] == '\\' && q[1] == '/') { u[k++] = '/'; i++; }
        else if (q[0] == '\\' && q[1] == 'u' && q[2] == '0' && q[3] == '0' && q[4] == '2' && ((q[5] | 32) == '6' || (q[5] | 32) == 'f')) {
            u[k++] = (q[5] | 32) == '6' ? '&' : '/'; i += 5;
        } else if (q[0] == '&' && q[1] == 'a' && q[2] == 'm' && q[3] == 'p' && q[4] == ';') { u[k++] = '&'; i += 4; }
        else u[k++] = q[0];
    }
    while (k && (u[k - 1] == '&' || u[k - 1] == '\\' || u[k - 1] == '.')) k--;   /* what began &quot; or a \", or a sentence's end */
    u[k] = 0;
    if (mode == 1 && !s_rb[0] && has_at(u, k, "radio-browser") && k < PAGE_HREF_MAX)
        for (i = 0; i <= k; i++) s_rb[i] = u[i];
    if (!k || !sounds(u, mode) || s_nsniffed >= SNIFF_MAX) return;
    for (i = 0; i < s_nsniffed; i++) if (eq_ci(s_sniffed[i], u)) return;
    for (i = 0; i <= k; i++) s_sniffed[s_nsniffed][i] = u[i];
    s_nsniffed++;
}

static void sniff(u8 b)
{
    s_ring[s_ring_at++ & 31] = (char)b;
    if (b == '/' && ring_ends("/api/nowplaying/")) s_azura = 1;
    if (s_sn_on) {
        if (b == '"' || b == '\'' || b == '<' || b == '>' || b <= ' ' || b == ',' || b == '}' || b == ']' ||
            (b == 'q' && s_sn_len && s_sn[s_sn_len - 1] == '&')) { sniff_end(); return; }
        if (s_sn_len < PAGE_HREF_MAX - 1) s_sn[s_sn_len++] = (char)b;
        else s_sn_on = 0;                          /* too long to be one      */
        return;
    }
    if (b == '/') {
        int sec = ring_ends("https://") || ring_ends("https:\\/\\/");
        if (sec || ring_ends("http://") || ring_ends("http:\\/\\/")) {
            const char *pre = sec ? "https://" : "http://";
            for (s_sn_len = 0; pre[s_sn_len]; s_sn_len++) s_sn[s_sn_len] = pre[s_sn_len];
            s_sn_on = 1;
        }
    } else if (b == '"' || b == ';') {
        static const char *const keys[] = { "stream", "streamurl", "stream_url", "listen_url", "listenurl",
                                            "audiourl", "audio_url", "mp3", 0 };
        static const char *const seps[] = { "\":\"", "\": \"", "&quot;:&quot;", 0 };
        char pat[40];
        int k, j, a, c;
        for (k = 0; keys[k]; k++)
            for (j = 0; seps[j]; j++) {
                for (a = 0; keys[k][a]; a++) pat[a] = keys[k][a];
                for (c = 0; seps[j][c]; c++) pat[a + c] = seps[j][c];
                pat[a + c] = 0;
                if (ring_ends(pat)) { s_sn_len = 0; s_sn_on = 2; return; }
            }
    }
}

/* Asks a page's server, or the directory it names, for something the
 * page's script would have asked for, and hands each byte of the answer
 * to eat().  Nothing in it is run. */
static void side_fetch(const Url *u, const char *path, const char *what, void (*eat)(u8))
{
    static u8 buf[2048];
    char req[PAGE_HREF_MAX + 300];
    int n, i, status = 0, state = 0, sp = 0;
    u32 got = 0, last = 0;
    if (!A->conn_open(u->host, u->port, u->secure, tls_note)) return;
    note(what, C_DIM);
    fmt_str(req, sizeof req, "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: FLOPOS/%s (a 1996 Toshiba)\r\n"
            "Accept: application/json\r\nConnection: close\r\n\r\n", path, u->host, A->os_version());
    for (n = 0; req[n]; n++) { }
    conn_write((const u8 *)req, n);
    while (got < 65536) {
        n = conn_read(buf, sizeof buf, 15000);
        if (n <= 0) break;
        for (i = 0; i < n; i++) {
            u8 b = buf[i];
            if (!state) {                          /* the headers: "HTTP/1.1 200", up to a blank line */
                if (sp == 1 && b >= '0' && b <= '9') status = status * 10 + (b - '0');
                if (b == ' ' && sp < 2) sp++;
                last = (last << 8) | b;
                if (last == 0x0D0A0D0Au || (last & 0xFFFF) == 0x0A0A) state = 1;
                continue;
            }
            if (status != 200) break;
            eat(b);
            got++;
        }
        if (state && status != 200) break;
        main_pump(0);
    }
    conn_close();
}

/* AzuraCast's station pages (radio.zelerk.com/public/chiptune, behind
 * watch.chiptune.fm) are drawn by JavaScript from the station's own
 * description at /api/nowplaying/<station>: JSON that names its streams.
 * browzer asks for it the same way, and reads it for addresses as it does
 * a page. */
static void azuracast(const Url *u)
{
    char path[80], st[48];
    int k;
    if (!lower_eq(u->path, "/public/", 8)) return;
    for (k = 0; u->path[8 + k] && u->path[8 + k] != '/' && u->path[8 + k] != '?' && u->path[8 + k] != '#'; k++) {
        char c = u->path[8 + k];
        if (k >= (int)sizeof st - 1 || !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return;
        st[k] = c;
    }
    st[k] = 0;
    if (!k) return;
    fmt_str(path, sizeof path, "/api/nowplaying/%s", st);
    side_fetch(u, path, "Asking the station (AzuraCast) for its streams...", sniff);
}

/* 72fm.com's country pages (/radio/il) name their stations without links:
 * its script asks the open Radio Browser directory, which the page names
 * in a <link rel="preconnect">.  browzer asks it the same way, and lists
 * the stations, most voted first, as links to their streams. */
static struct {
    char str[PAGE_HREF_MAX], key[16], name[96], url[PAGE_HREF_MAX], codec[12];
    int  in_str, esc, hex, nhex, len, want, depth, n;
    u32  kbps, u;
} s_rj;

static void rb_station(void)
{
    const u8 *p = (const u8 *)s_rj.name;
    char t[40];
    int shown = 0, heb = 0, arab = 0;
    s_linkid = add_link_to(s_rj.url);
    if (!s_linkid) return;
    wire_line("station: ", s_rj.url);
    s_link = 1;
    while (*p) {                                  /* UTF-8, to what the font can show */
        u32 c = *p++;
        int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        if (more) c &= 0x3F >> more;
        while (more-- && (*p & 0xC0) == 0x80) c = (c << 6) | (*p++ & 0x3F);
        if (c >= 0x590 && c < 0x600) heb = 1;
        if (c >= 0x600 && c < 0x700) arab = 1;
        if (glyph_for(c) == '?' && c != '?') continue;
        if (!shown && (c == ' ' || c == '-' || c == '|')) continue;
        emit(c);
        shown++;
    }
    if (!shown) emit_text(heb ? "(a name in Hebrew)" : arab ? "(a name in Arabic)" : "(a name in another script)", C_ACCENT);
    flush_word();
    s_link = 0; s_linkid = 0;
    if (s_rj.codec[0]) {
        if (s_rj.kbps) fmt_str(t, sizeof t, " - %s, %u kbps", s_rj.codec, s_rj.kbps);
        else fmt_str(t, sizeof t, " - %s", s_rj.codec);
        emit_text(t, C_DIM);
        flush_word();
    }
    newline(1);
    s_rj.n++;
}

static void rb_eat(u8 b)
{
    if (s_rj.in_str) {
        char c[4];
        int n = 1, k;
        if (s_rj.hex) {                           /* \uXXXX */
            int v = b >= '0' && b <= '9' ? b - '0' : (b | 32) >= 'a' && (b | 32) <= 'f' ? (b | 32) - 'a' + 10 : 0;
            s_rj.u = (s_rj.u << 4) | (u32)v;
            if (++s_rj.nhex < 4) return;
            s_rj.hex = 0;
            n = put_utf8(c, s_rj.u);
        } else if (s_rj.esc) {
            s_rj.esc = 0;
            if (b == 'u') { s_rj.hex = 1; s_rj.nhex = 0; s_rj.u = 0; return; }
            c[0] = b == 'n' || b == 't' || b == 'r' ? ' ' : (char)b;
        } else if (b == '\\') { s_rj.esc = 1; return; }
        else if (b == '"') {
            s_rj.in_str = 0;
            s_rj.str[s_rj.len] = 0;
            if (!s_rj.want) {
                for (k = 0; k < 15 && s_rj.str[k]; k++) s_rj.key[k] = s_rj.str[k];
                s_rj.key[k] = 0;
            } else if (s_rj.depth == 2) {
                char *to = eq_ci(s_rj.key, "name") ? s_rj.name : eq_ci(s_rj.key, "url_resolved") ? s_rj.url :
                           eq_ci(s_rj.key, "codec") ? s_rj.codec : 0;
                int max = to == s_rj.name ? (int)sizeof s_rj.name : to == s_rj.codec ? (int)sizeof s_rj.codec : PAGE_HREF_MAX;
                if (to) {
                    for (k = 0; k < max - 1 && s_rj.str[k]; k++) to[k] = s_rj.str[k];
                    to[k] = 0;
                }
            }
            s_rj.want = 0;
            return;
        } else c[0] = (char)b;
        for (k = 0; k < n; k++) if (s_rj.len < PAGE_HREF_MAX - 1) s_rj.str[s_rj.len++] = c[k];
        return;
    }
    if (b == '"') { s_rj.in_str = 1; s_rj.len = 0; return; }
    if (b == ':') { s_rj.want = 1; if (eq_ci(s_rj.key, "bitrate")) s_rj.kbps = 0; return; }
    if (b >= '0' && b <= '9' && s_rj.want && s_rj.depth == 2 && eq_ci(s_rj.key, "bitrate")) {
        s_rj.kbps = s_rj.kbps * 10 + (b - '0');
        return;
    }
    if (b == '{' || b == '[') {
        if (++s_rj.depth == 2) { s_rj.name[0] = s_rj.url[0] = s_rj.codec[0] = 0; s_rj.kbps = 0; }
    } else if (b == '}' || b == ']') {
        if (s_rj.depth == 2 && b == '}' && s_rj.url[0]) rb_station();
        s_rj.depth--;
    }
    if (b == ',' || b == '{' || b == '}') s_rj.want = 0;
}

static void radiobrowser(const Url *page)
{
    const char *p = page->path;
    char path[PAGE_HREF_MAX + 120], cc[3];
    Url d;
    int k;
    if (!s_rb[0] || !lower_eq(p, "/radio/", 7)) return;
    for (k = 0; k < 2; k++) {
        char c = p[7 + k];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) return;
        cc[k] = (char)(c & ~32);
    }
    cc[2] = 0;
    if (p[9] && p[9] != '/' && p[9] != '?' && p[9] != '#') return;
    if (!url_parse(s_rb, &d)) return;
    for (k = 0; d.path[k]; k++) { }
    while (k && d.path[k - 1] == '/') d.path[--k] = 0;
    fmt_str(path, sizeof path, "%s/json/stations/bycountrycodeexact/%s?order=votes&reverse=true&hidebroken=true&limit=40",
            d.path, cc);
    for (k = 0; k < (int)sizeof s_rj; k++) ((u8 *)&s_rj)[k] = 0;
    newline(2);
    emit_text("Its stations, from the Radio Browser directory:", C_BRIGHT);
    newline(1);
    side_fetch(&d, path, "Asking the Radio Browser directory for the stations...", rb_eat);
    if (!s_rj.n) { emit_text("(It named none.)", C_DIM); flush_word(); newline(1); }
}

/* At the end of a page: what it named, as links, unless it links to them. */
static void sniff_list(void)
{
    int i, j, shown = 0;
    if (s_sn_on) sniff_end();
    for (i = 0; i < s_nsniffed; i++) {
        for (j = 0; j < s_pg->nlinks && !eq_ci(s_pg->href[j], s_sniffed[i]); j++) { }
        if (j < s_pg->nlinks) continue;
        if (!shown++) {
            newline(2);
            emit_text("Sound this page names, without a link to it:", C_BRIGHT);
            newline(1);
        }
        s_linkid = add_link_to(s_sniffed[i]);
        if (!s_linkid) break;
        wire_line("sound it names: ", s_sniffed[i]);
        s_link = 1;
        for (j = 0; s_sniffed[i][j]; j++) emit((u8)s_sniffed[i][j]);
        flush_word();
        s_link = 0; s_linkid = 0;
        newline(1);
    }
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
    f->form = (u8)s_form; f->kind = (u8)kind; f->link = 0; f->on = 0;
    f->opt0 = f->nopt = f->sel = 0;
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
    c = &pg->cells[f->line * pg->width + f->col];
    if (f->kind == FK_CHECK || f->kind == FK_RADIO) {       /* [x]  (*)      */
        c[0].ch = f->on ? (f->kind == FK_RADIO ? '*' : 'x') : ' ';
        c[0].col = C_BRIGHT;
        return;
    }
    if (f->kind == FK_SELECT) {                              /* [words v]     */
        const PageOption *o = &pg->opts[f->opt0 + f->sel];
        v = (const u8 *)pg->pool + o->lab;
        for (i = 0; i < f->width; i++) { c[i].ch = ' '; c[i].col = C_BRIGHT; }
        for (k = 0, i = 0; k < o->llen && i < f->width - 2; i++) {
            u32 cp = v[k++];
            if (cp >= 0xC0) {
                int more = cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : 1;
                cp &= more == 3 ? 0x07 : more == 2 ? 0x0F : 0x1F;
                while (more-- && k < o->llen) cp = (cp << 6) | (v[k++] & 0x3F);
            }
            c[i].ch = glyph_for(cp);
        }
        c[f->width - 1].ch = 'v';
        c[f->width - 1].col = C_GREY;
        return;
    }
    for (k = 0; k < f->vlen; k++) if ((v[k] & 0xC0) != 0x80) n++;     /* characters */
    skip = n > f->width - 1 ? n - (f->width - 1) : 0;                 /* room for the caret */
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

/* [x] or (*): a tick box, or one of a set of choices. */
static void tick_box(int fi)
{
    Page *p = s_pg;
    PageField *f = &p->fields[fi];
    u16 id = field_link(fi);
    int radio = f->kind == FK_RADIO;
    make_room(3);
    out_char(radio ? '(' : '[', C_GREY, id); s_col++;
    f->line = (u16)(p->lines - 1);
    f->col = (u16)p->col;
    f->width = 1;
    out_char(' ', C_BRIGHT, id); s_col++;
    out_char(radio ? ')' : ']', C_GREY, id); s_col++;
    s_space = 1;
    s_newlines = 0;
    page_field_show(p, fi);
}

static int same_name(const PageField *a, const PageField *b)
{
    u32 k;
    if (a->form != b->form || a->nlen != b->nlen) return 0;
    for (k = 0; k < a->nlen; k++) if (s_pg->pool[a->name + k] != s_pg->pool[b->name + k]) return 0;
    return 1;
}

/* The end of a choice in a list: words tidied, and its value is its words
 * if it was given none. */
static void option_end(void)
{
    PageOption *o;
    PageField *f;
    if (s_opt < 0) return;
    o = &s_pg->opts[s_opt];
    f = &s_pg->fields[s_select];
    while (o->llen && s_pg->pool[o->lab + o->llen - 1] == ' ') o->llen--;
    if (!s_opt_val) { o->val = o->lab; o->vlen = o->llen; }
    f->nopt = (u16)(s_opt - f->opt0 + 1);
    if (s_opt_sel) f->sel = (u16)(s_opt - f->opt0);
    s_opt = -1;
}

static void option_begin(void)
{
    Page *p = s_pg;
    PageOption *o;
    int n, i;
    if (s_select < 0 || p->nopts >= PAGE_OPTIONS) return;
    decode_entities(s_val);
    for (n = 0; s_val[n]; n++) { }
    if (p->pool_used + (u32)n + OPT_LAB > PAGE_POOL) return;
    o = &p->opts[p->nopts];
    o->val = p->pool_used; o->vlen = s_has_val ? (u32)n : 0;
    for (i = 0; i < (int)o->vlen; i++) p->pool[p->pool_used++] = s_val[i];
    o->lab = p->pool_used; o->llen = 0;
    p->pool_used += OPT_LAB;
    s_opt_val = s_has_val;
    s_opt_sel = s_checked;
    s_opt = p->nopts++;
}

/* </select>: the list drawn as a box showing the choice, [words v]. */
static void select_end(void)
{
    Page *p = s_pg;
    PageField *f;
    int w = 4, i, fi = s_select;
    u16 id;
    option_end();
    s_select = -1;
    if (fi < 0) return;
    f = &p->fields[fi];
    if (!f->nopt) return;
    for (i = 0; i < f->nopt; i++) {
        const PageOption *o = &p->opts[f->opt0 + i];
        u32 k;
        int n = 0;
        for (k = 0; k < o->llen; k++) if ((p->pool[o->lab + k] & 0xC0) != 0x80) n++;
        if (n > w) w = n;
    }
    if (w > 30) w = 30;
    if (w > out_width() - 6) w = out_width() - 6;
    f->val = p->opts[f->opt0 + f->sel].val;
    f->vlen = p->opts[f->opt0 + f->sel].vlen;
    id = field_link(fi);
    make_room(w + 4);
    out_char('[', C_GREY, id); s_col++;
    f->line = (u16)(p->lines - 1);
    f->col = (u16)p->col;
    f->width = (u16)(w + 2);
    for (i = 0; i < w + 2; i++) { out_char(' ', C_BRIGHT, id); s_col++; }
    out_char(']', C_GREY, id); s_col++;
    s_space = 1;
    s_newlines = 0;
    page_field_show(p, fi);
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
    } else if (eq_ci(s_type, "checkbox") || eq_ci(s_type, "radio")) {
        int radio = eq_ci(s_type, "radio");
        int fi = add_field(radio ? FK_RADIO : FK_CHECK, s_has_val ? s_val : "on", s_has_val ? n : 2, 0);
        if (fi >= 0) {
            PageField *f = &s_pg->fields[fi];
            int i;
            f->on = (u8)s_checked;
            if (radio && f->on)                  /* only one of a set is picked */
                for (i = 0; i < fi; i++)
                    if (s_pg->fields[i].kind == FK_RADIO && same_name(&s_pg->fields[i], f)) {
                        s_pg->fields[i].on = 0;
                        page_field_show(s_pg, i);
                    }
            tick_box(fi);
        }
    } else if (!s_type[0] || eq_ci(s_type, "text") || eq_ci(s_type, "search") ||
               eq_ci(s_type, "email") || eq_ci(s_type, "url") || eq_ci(s_type, "tel") ||
               eq_ci(s_type, "number") || eq_ci(s_type, "password")) {
        int fi = add_field(eq_ci(s_type, "password") ? FK_PASSWORD : FK_TEXT, s_val, n, n + 512);
        int w = s_size[0] ? number(s_size) : 24;
        if (fi >= 0) field_box(fi, w);
    }
    /* Files, and buttons that only do something with JavaScript: not yet. */
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
    if (tag_is("select")) {
        if (!s_tag_close) {
            int fi;
            if (s_select != -1) select_end();
            fi = add_field(FK_SELECT, "", 0, 0);
            s_select = fi >= 0 ? fi : -2;
            if (fi >= 0) s_pg->fields[fi].opt0 = (u16)s_pg->nopts;
        } else if (s_select != -1) select_end();
        return;
    }
    if (s_select != -1) {                /* inside a list: only its choices */
        if (tag_is("option")) { option_end(); if (!s_tag_close) option_begin(); }
        return;
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
    if (tag_is("kbd")) { s_kbd = !s_tag_close; return; }   /* a key to press: pink */
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
    /* <meta http-equiv="refresh" content="5; url=...">: chat pages without
     * JavaScript reload themselves this way to show what is new.  Kept for
     * browzer to act on (browzer.c). */
    if (tag_is("meta")) {
        if (s_pg && eq_ci(s_equiv, "refresh") && !s_tag_close) {
            const char *c = s_content;
            int secs = 0, k = 0, digits = 0;
            decode_entities(s_content);          /* url=&quot;...&quot; is common */
            while (*c == ' ') c++;
            while (*c >= '0' && *c <= '9') { secs = secs * 10 + (*c++ - '0'); digits++; }
            s_pg->refresh = secs > 0 ? secs : 1;
            for (; *c; c++)
                if ((c[0] | 32) == 'u' && (c[1] | 32) == 'r' && (c[2] | 32) == 'l' && c[3] == '=') {
                    c += 4;
                    while (*c == ' ' || *c == '\'' || *c == '"') c++;
                    while (c[k] && c[k] != '\'' && c[k] != '"' && k < PAGE_HREF_MAX - 1) { s_pg->refresh_url[k] = c[k]; k++; }
                    break;
                }
            s_pg->refresh_url[k] = 0;
            /* At once, to somewhere else: the rest of this page is not
             * worth the wait.  (DuckDuckGo sends 200 KB of scripts after
             * pointing a page without them to its plain version.) */
            if (digits && secs == 0 && k) s_refresh_go = 1;
        }
        return;
    }
    /* A frame: not drawn inside the page, but a link to open it by. */
    if ((tag_is("iframe") || tag_is("frame")) && !s_tag_close && s_src[0] && s_pg) {
        int i, last = 0;
        const char *lab = "[a frame: ";
        for (i = 0; s_src[i] && i < PAGE_HREF_MAX - 1; i++) { s_href[i] = s_src[i]; if (s_src[i] == '/' && s_src[i + 1]) last = i + 1; }
        s_href[i] = 0;
        flush_word();
        newline(1);
        s_linkid = add_link();
        if (s_linkid) {
            s_link = 1;
            for (i = 0; lab[i]; i++) emit((u8)lab[i]);
            for (i = last; s_src[i] && s_src[i] != '?' && i < last + 40; i++) emit((u8)s_src[i]);
            emit(']');
            flush_word();
            s_link = 0; s_linkid = 0;
        }
        newline(1);
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
        { "middot", 0xB7 }, { "uuml", 0xFC }, { "Uuml", 0xDC },
        { "szlig", 0xDF }, { "egrave", 0xE8 }, { "agrave", 0xE0 },
        { "ccedil", 0xE7 }, { "ntilde", 0xF1 }, { "Eacute", 0xC9 },
        { "euro", 0x20AC }, { "pound", 0xA3 }, { "deg", 0xB0 },
        { "sect", 0xA7 }, { 0, 0 } };
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

/* An attribute's name is complete: the ones that matter with no value. */
static void attr_named(void)
{
    if (s_aname_len >= (int)sizeof s_aname) return;
    s_aname[s_aname_len] = 0;
    if (eq_ci(s_aname, "checked") || eq_ci(s_aname, "selected")) s_checked = 1;
}

static void attr_begin_value(void)
{
    /* A name too long to be one of ours is marked by a length of the
     * whole array: its end is the last place, not one past it. */
    s_aname[s_aname_len < (int)sizeof s_aname ? s_aname_len : (int)sizeof s_aname - 1] = 0;
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
            { "http-equiv", s_equiv, 16 }, { "content", s_content, 256 }, { "src", s_src, PAGE_HREF_MAX },
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
    s_checked = 0;
    s_equiv[0] = s_content[0] = s_src[0] = 0;
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
            if (s_tag_done && s_astate == A_NAME) attr_named();
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
            if (b == '=') { attr_named(); attr_begin_value(); s_astate = A_EQ; }
            else if (ws || b == '/') { attr_named(); s_astate = A_NONE; }
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
    s_bold = s_link = s_dim = s_in_title = s_kbd = 0;
    s_linkid = 0;
    s_cp_left = 0;
    s_form = s_area = s_button = -1;
}

static void body_byte(u8 b)
{
    if (s_pg) sniff(b);
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

int  (*web_media_start)(const char *ctype, const char *url, u32 length, u32 metaint, const char *station);
int  (*web_media_bytes)(const u8 *p, int n);
void (*web_media_end)(int whole);

static u32 number_at(const char *v)
{
    u32 x = 0;
    while (*v == ' ') v++;
    while (*v >= '0' && *v <= '9') x = x * 10 + (u32)(*v++ - '0');
    return x;
}
static int conn_write(const u8 *d, int n) { return A->conn_write(d, n); }
static int conn_read(u8 *b, int n, int w) { return A->conn_read(b, n, w); }
static void conn_close(void) { A->conn_close(); }

/* Returns 0 for a page, 1 with moved filled in for a redirect, -1 for a
 * failure that has already been explained. */
/* post: a form's contents to send, or 0 for an ordinary request. */
static int fetch_once(const Url *u, char *moved, int moved_max, const u8 *post, int postlen)
{
    static u8 buf[4096];
    static char head[8192];
    char req[PAGE_HREF_MAX + 200], line[96];
    int head_len = 0, in_body = 0, status = 0, n, i, html = 1, counting = 0, media = 0, stopped = 0, whole = 0, asked = 0;
    u32 clen = 0, metaint = 0;
    char station[48];
    u32 body = 0, said = 0;
    char ctype[48];

    ctype[0] = 0;
    moved[0] = 0;
    station[0] = 0;

    s_secure = u->secure;
    s_refresh_go = 0;
    s_nsniffed = 0; s_sn_on = 0; s_bare = 0; s_azura = 0; s_rb[0] = 0;
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
                "Connection: close\r\n",
                u->path, u->host, A->os_version(), (u32)postlen);
    else
        fmt_str(req, sizeof req,
                "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: FLOPOS/%s (a 1996 Toshiba)\r\n"
                "Accept: text/html, text/plain, */*\r\nIcy-MetaData: 1\r\nConnection: close\r\n",
                u->path, u->host, A->os_version());
    for (n = 0; req[n]; n++) { }
    conn_write((const u8 *)req, n);
    {
        /* Cookies the site gave us before (cookie.c), then the blank line
         * that ends the headers. */
        static char ck[4096];
        int cn = cookie_header(u, ck, (int)sizeof ck);
        if (cn) conn_write((const u8 *)ck, cn);
        conn_write((const u8 *)"\r\n", 2);
    }
    for (i = 0; post && i < postlen; i += 4096)        /* the form itself   */
        conn_write(post + i, postlen - i < 4096 ? postlen - i : 4096);
    s_status = 0;

    filter_reset(1);

    for (;;) {
        /* An answer worked out before it is sent -- an AI's, say -- can
         * take a while to begin: a minute for the first byte, then fifteen
         * seconds of silence at most. */
        n = conn_read(buf, sizeof buf, head_len || body ? 15000 : 60000);
        if (n == 0) { whole = 1; break; }                /* they finished  */
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
            if (in_body && media) {                      /* not a page: handed on whole */
                body += (u32)(n - i);
                if (!web_media_bytes(buf + i, n - i)) stopped = 1;
                break;
            }
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
                    } else if (lower_eq(ln, "set-cookie:", 11)) {
                        cookie_set(u, ln + 11);
                    } else if (lower_eq(ln, "content-length:", 15)) {
                        clen = number_at(ln + 15);
                    } else if (lower_eq(ln, "icy-metaint:", 12)) {
                        metaint = number_at(ln + 12);
                    } else if (lower_eq(ln, "icy-name:", 9)) {
                        char *v = ln + 9; int k = 0;
                        while (*v == ' ') v++;
                        while (v < end && *v != '\r' && k < 47) station[k++] = *v++;
                        station[k] = 0;
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
                if (lower_eq(head, "ICY", 3) && !ctype[0])        /* old radio */
                    fmt_str(ctype, sizeof ctype, "audio/mpeg");
                {
                    /* A playlist is often sent as plain text; it is still
                     * for playing, not for reading. */
                    int k = 0, pl;
                    while (u->path[k] && u->path[k] != '?') k++;
                    pl = (k > 4 && lower_eq(u->path + k - 4, ".m3u", 4)) || (k > 4 && lower_eq(u->path + k - 4, ".pls", 4));
                    if (pl && s_pg) { html = 0; fmt_str(ctype, sizeof ctype, "audio/x-mpegurl"); }
                }
                if (!html && !lower_eq(ctype, "text/", 5) && ctype[0] && s_pg && web_media_start &&
                    status >= 200 && status < 300) {
                    char where[PAGE_HREF_MAX + 16];
                    url_text(u, where, (int)sizeof where);
                    media = web_media_start(ctype, where, clen, metaint, station);
                }
                if (media) {
                    /* handed on from the next byte */
                } else if (!html && !lower_eq(ctype, "text/", 5) && ctype[0]) {
                    note("Not text, so not shown. Counting the bytes instead.", C_DIM);
                    counting = 1;
                } else {
                    filter_reset(html);
                }
            }
        }

        if (s_pg) {
            s_pg->bytes = body;
            if (body - said >= (media ? 16384u : 2048u) && media != 2) {
                said = body;
                if (clen && media) fmt_str(line, sizeof line, "loading... %u KB of %u KB", body >> 10, clen >> 10);
                else fmt_str(line, sizeof line, "loading... %u bytes", body);
                note(line, C_DIM);
            }
        }
        if (stopped || s_refresh_go) break;
        if (stop_asked()) {
            flush_word();
            note("Stopped.", C_ACCENT);
            asked = 1;
            break;
        }
        main_pump(0);
    }

    flush_word();
    conn_close();
    if (s_pg && !media && !counting && in_body) {
        char t[80];
        if (s_azura && status == 200) azuracast(u);
        if (status == 200) radiobrowser(u);
        sniff_list();
        fmt_str(t, sizeof t, "%u, %u links: ", (u32)status, (u32)s_pg->nlinks);
        wire_line(t, s_pg->title[0] ? s_pg->title : "(no title)");
    }
    if (media) web_media_end(asked ? -1 : whole && !stopped);
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
