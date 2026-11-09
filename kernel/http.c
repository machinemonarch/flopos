/* ===========================================================================
 * http.c -- asking a web server for a page, and making it readable.
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
#include "flopos.h"
#include "io.h"
#include "version.h"

/* ---- where the words go ---------------------------------------------- */
static Page *s_pg;                       /* a page, or 0 for the terminal  */

static void note(const char *t, u8 col)
{
    int i;
    if (!s_pg) { term_colour(col); term_print(t); term_newline(); return; }
    for (i = 0; t[i] && i < (int)sizeof s_pg->note - 1; i++) s_pg->note[i] = t[i];
    s_pg->note[i] = 0;
    s_pg->note_col = col;
    if (s_pg->progress) s_pg->progress();
}

static int out_width(void) { return s_pg ? s_pg->width : term_cols() - 1; }

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
    pg->title[0] = 0;
    pg->note[0] = 0;
    pg->bytes = 0;
    pg->status = 0;
    pg->secure = 0;
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
static char s_alt[60];
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

/* One character of readable text, already decoded. */
static void emit(u32 c)
{
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
    return (u16)++s_pg->nlinks;
}

static void tag_ended(void)
{
    static const char *block[] = { "p", "div", "tr", "ul", "ol", "table",
        "section", "article", "header", "footer", "nav", "pre", "blockquote",
        "dt", "dd", "form", "main", "center", "aside", "figure", 0 };
    int i;

    if (tag_is("script") || tag_is("style") || tag_is("noscript") ||
        tag_is("svg") || tag_is("template")) {
        if (!s_tag_close) {
            for (i = 0; s_tag[i]; i++) s_skip_until[i] = s_tag[i];
            s_skip_until[i] = 0;
            s_hstate = H_SKIP;
            return;
        }
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
    if (tag_is("img")) {
        if (s_alt[0]) {
            s_dim = 1;
            emit_text("[", C_DIM);
            emit_text(s_alt, C_DIM);
            emit_text("]", C_DIM);
            flush_word();
            s_dim = 0;
            s_space = 1;
        }
        return;
    }
    if (tag_is("hr")) { rule(); return; }
    if (tag_is("br")) { newline(1); s_newlines = 0; return; }
    if (tag_is("li")) {
        if (!s_tag_close) { newline(1); emit_text("* ", C_DIM); }
        return;
    }
    for (i = 0; block[i]; i++)
        if (tag_is(block[i])) { newline(2); return; }
}

static void entity_ended(void)
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

    s_ent[s_ent_len] = 0;
    if (s_ent[0] == '#') {
        u32 v = 0;
        int hex = (s_ent[1] == 'x' || s_ent[1] == 'X');
        for (i = hex ? 2 : 1; s_ent[i]; i++) {
            char c = s_ent[i];
            int d = (c >= '0' && c <= '9') ? c - '0' :
                    (hex && c >= 'a' && c <= 'f') ? c - 'a' + 10 :
                    (hex && c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
            if (d < 0) break;
            v = v * (hex ? 16 : 10) + (u32)d;
        }
        emit(v);
        return;
    }
    for (i = 0; e[i].n; i++) {
        for (j = 0; e[i].n[j] && e[i].n[j] == s_ent[j]; j++) { }
        if (!e[i].n[j] && !s_ent[j]) { emit(e[i].c); return; }
    }
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
static int conn_write(const u8 *d, int n) { return s_secure ? tls_write(d, n) : tcp_write(d, n); }
static int conn_read(u8 *b, int n, int w) { return s_secure ? tls_read(b, n, w) : tcp_read(b, n, w); }
static void conn_close(void) { if (s_secure) tls_close(); tcp_close(); }

/* Returns 0 for a page, 1 with moved filled in for a redirect, -1 for a
 * failure that has already been explained. */
static int fetch_once(const Url *u, char *moved, int moved_max)
{
    static u8 buf[1024];
    static char head[2048];
    char req[PAGE_HREF_MAX + 200], line[96];
    int head_len = 0, in_body = 0, status = 0, n, i, html = 1, counting = 0;
    u32 to, body = 0, said = 0;
    char ctype[48];

    ctype[0] = 0;
    moved[0] = 0;

    if (!ip_parse(u->host, &to)) {
        fmt_str(line, sizeof line, "looking up %s...", u->host);
        note(line, C_DIM);
        to = ip_lookup(u->host);
        if (!to) {
            fmt_str(line, sizeof line, "Nobody knows a %s.", u->host);
            note(line, C_ACCENT);
            return -1;
        }
    }

    fmt_str(line, sizeof line, "%s ...", u->host);
    note(line, C_DIM);
    if (!s_pg) main_pump(1);

    if (!tcp_connect(to, u->port, 6000)) {
        fmt_str(line, sizeof line, "Could not reach it: %s.", tcp_why());
        note(line, C_ACCENT);
        return -1;
    }

    s_secure = u->secure;
    if (s_secure) {
        note("agreeing on a key...", C_DIM);
        if (!s_pg) main_pump(1);
        if (!tls_connect(u->host)) {
            fmt_str(line, sizeof line, "The secure handshake failed: %s.", tls_why());
            note(line, C_ACCENT);
            tcp_close();
            return -1;
        }
        if (!s_pg) note("encrypted -- but who is at the other end is not checked yet", C_DIM);
        else s_pg->secure = 1;
    }

    fmt_str(req, sizeof req,
            "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: FLOPOS/%s (a 1996 Toshiba)\r\n"
            "Accept: text/html, text/plain, */*\r\nConnection: close\r\n\r\n",
            u->path, u->host, FLOPOS_VERSION);
    for (n = 0; req[n]; n++) { }
    conn_write((const u8 *)req, n);

    filter_reset(1);

    for (;;) {
        n = conn_read(buf, sizeof buf, 15000);
        if (n == 0) break;                               /* they finished  */
        if (n < 0) {
            flush_word();
            if (n == -1) note("It went quiet, so I stopped waiting.", C_ACCENT);
            else {
                fmt_str(line, sizeof line, "Cut off: %s.", s_secure ? tls_why() : tcp_why());
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
static int get(const char *url, Page *pg)
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
        r = fetch_once(&u, moved, sizeof moved);
        if (r != 1) {
            if (!pg) term_plain(0);
            s_pg = keep;
            return r == 0;
        }
        if (!pg) {
            term_colour(C_DIM);
            term_printf("moved to %s", moved);
            term_newline();
        }
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

void http_fetch(const char *url) { (void)get(url, 0); }
int  http_page(const char *url, Page *pg) { return get(url, pg); }
