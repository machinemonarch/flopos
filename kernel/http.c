/* ===========================================================================
 * http.c -- asking a web server for a page, and reading it out.
 *
 * HTTP is text over a TCP conversation: a line saying what you want, a
 * few lines about yourself, a blank line.  The answer is the same shape
 * the other way, followed by the page.  This asks in HTTP/1.0 on purpose:
 * a server answering 1.0 sends the page plainly and hangs up when it is
 * finished, rather than cutting it into chunks and keeping the line open,
 * so "the page" is simply everything until the other end goes quiet.
 *
 * A page is mostly HTML, which is not for reading.  So it goes through a
 * small filter on its way to the terminal: tags dropped, scripts and
 * styles skipped whole, the common entities turned back into characters,
 * whitespace squeezed, a line break where a paragraph or heading or list
 * item would be, and words wrapped to the width of the screen.  Headings
 * come out bright and links in the accent colour.  UTF-8 and Latin-1 are
 * both understood, with the Swedish letters drawn properly and the rest
 * of the world approximated.
 * ===========================================================================
 */
#include "flopos.h"
#include "io.h"
#include "version.h"

static void say(const char *t)     { term_colour(C_ACCENT); term_print(t); term_newline(); }
static void say_dim(const char *t) { term_colour(C_DIM);    term_print(t); term_newline(); }

/* ---- turning HTML into lines --------------------------------------------- */
enum { H_TEXT, H_TAG, H_SKIP, H_ENT };

static int  s_html;                      /* filter, or print as it comes   */
static int  s_hstate;
static char s_tag[12];
static int  s_tag_len, s_tag_close, s_tag_done;
static char s_skip_until[10];
static char s_ent[10];
static int  s_ent_len;
static u8   s_word[80], s_word_col[80];
static int  s_word_len, s_col, s_newlines, s_space;
static u8   s_colour;
static int  s_bold, s_link;
static u32  s_cp, s_cp_left;             /* a UTF-8 character in progress  */
static u32  s_shown;

static u8 colour_now(void)
{
    return s_bold ? C_BRIGHT : s_link ? C_ACCENT : C_TEXT;
}

static void flush_word(void)
{
    int i, w = term_cols() - 1;

    if (!s_word_len) return;
    if (s_col && s_col + 1 + s_word_len > w) { term_newline(); s_col = 0; }
    else if (s_col && s_space) { term_putc(' '); s_col++; }
    for (i = 0; i < s_word_len; i++) {
        if (s_col >= w) { term_newline(); s_col = 0; }
        term_colour(s_word_col[i]);
        term_putc(s_word[i]);
        s_col++;
    }
    s_word_len = 0;
    s_space = 0;
    s_newlines = 0;
    s_shown++;
}

static void newline(int want)
{
    flush_word();
    while (s_newlines < want) {
        if (s_col || s_newlines || want > 1) term_newline();
        s_col = 0;
        s_newlines++;
        if (want == 1) break;
    }
    s_space = 0;
}

/* One character of readable text, already decoded. */
static void emit(u32 c)
{
    u8 g;

    if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == 0xA0) {
        flush_word();
        s_space = 1;
        return;
    }
    switch (c) {
    case 0xE5: g = CH_ARING_L; break;    case 0xC5: g = CH_ARING; break;
    case 0xE4: g = CH_ADIA_L;  break;    case 0xC4: g = CH_ADIA;  break;
    case 0xF6: g = CH_ODIA_L;  break;    case 0xD6: g = CH_ODIA;  break;
    case 0xE9: case 0xE8: case 0xEA: case 0xEB: g = 'e'; break;
    case 0xC9: g = 'E'; break;
    case 0xFC: g = 'u'; break;           case 0xDC: g = 'U'; break;
    case 0xE0: case 0xE1: case 0xE2: g = 'a'; break;
    case 0xF8: g = 'o'; break;           case 0xD8: g = 'O'; break;
    case 0xE6: g = 'a'; break;
    case 0x2018: case 0x2019: g = '\''; break;
    case 0x201C: case 0x201D: g = '"'; break;
    case 0x2013: case 0x2014: g = '-'; break;
    case 0x2026: g = '.'; break;
    case 0xA9: g = 'c'; break;
    default:
        g = (c >= 32 && c < 127) ? (u8)c : '?';
    }
    if (s_word_len < (int)sizeof s_word) {
        s_word[s_word_len] = g;
        s_word_col[s_word_len] = colour_now();
        s_word_len++;
    }
    if (s_word_len == (int)sizeof s_word) flush_word();
}

static int tag_is(const char *t)
{
    int i;
    for (i = 0; t[i]; i++) if (s_tag[i] != t[i]) return 0;
    return s_tag[i] == 0;
}

static void tag_ended(void)
{
    static const char *block[] = { "p", "div", "br", "li", "tr", "ul", "ol",
        "table", "section", "article", "header", "footer", "nav", "pre",
        "blockquote", "dt", "dd", "hr", "form", "main", 0 };
    int i;

    if (tag_is("script") || tag_is("style")) {
        if (!s_tag_close) {
            for (i = 0; s_tag[i]; i++) s_skip_until[i] = s_tag[i];
            s_skip_until[i] = 0;
            s_hstate = H_SKIP;
            return;
        }
    }
    if ((s_tag[0] == 'h' && s_tag[1] >= '1' && s_tag[1] <= '6' && !s_tag[2]) ||
        tag_is("title")) {
        newline(2);
        s_bold = !s_tag_close;
        if (s_tag_close) newline(1);
        return;
    }
    if (tag_is("a")) { s_link = !s_tag_close; return; }
    if (tag_is("br")) { newline(1); s_newlines = 0; return; }
    for (i = 0; block[i]; i++)
        if (tag_is(block[i])) { newline(tag_is("li") ? 1 : 2); return; }
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
        { "ldquo", 0x201C }, { "rdquo", 0x201D }, { 0, 0 } };
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

/* One byte of the page, as it arrives. */
static void html_byte(u8 b)
{
    switch (s_hstate) {
    case H_TAG:
        if (b == '>') {
            s_tag[s_tag_len] = 0;
            s_hstate = H_TEXT;
            tag_ended();
        } else if (!s_tag_done) {
            if (b == '/' && s_tag_len == 0 && !s_tag_close) s_tag_close = 1;
            else if (b == ' ' || b == '\t' || b == '\n' || b == '\r' || b == '/')
                s_tag_done = s_tag_len > 0;
            else if (s_tag_len < (int)sizeof s_tag - 1)
                s_tag[s_tag_len++] = (char)((b >= 'A' && b <= 'Z') ? b + 32 : b);
        }
        return;

    case H_SKIP:
        /* Inside <script> or <style>: nothing shown until its own end tag.
         * Watching for "</" followed by the name is enough. */
        if (b == '<') { s_tag_len = 0; s_tag_close = 0; s_tag_done = 0; s_ent_len = -1; return; }
        if (s_ent_len == -1) { s_ent_len = (b == '/') ? -2 : 0; return; }
        if (s_ent_len <= -2) {
            int k = -s_ent_len - 2;
            char lc = (char)((b >= 'A' && b <= 'Z') ? b + 32 : b);
            if (s_skip_until[k] && lc == s_skip_until[k]) {
                s_ent_len--;
                if (!s_skip_until[k + 1]) s_ent_len = -100;
            } else if (s_ent_len == -100 && b == '>') {
                s_hstate = H_TEXT;
                s_ent_len = 0;
            } else if (s_ent_len != -100) {
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
        break;                           /* and fall on to treat b as text */

    default:
        break;
    }

    if (b == '<') {
        s_tag_len = 0; s_tag_close = 0; s_tag_done = 0;
        s_hstate = H_TAG;
        return;
    }
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
    s_bold = s_link = 0;
    s_cp_left = 0;
    s_shown = 0;
    s_colour = C_TEXT;
}

static void body_byte(u8 b)
{
    if (s_html) { html_byte(b); return; }
    if (b == '\n') { flush_word(); term_newline(); s_col = 0; s_newlines = 1; return; }
    if (b == '\r') return;
    if (s_cp_left || b >= 0x80) { html_byte(b); return; }   /* decode only */
    emit(b);
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

typedef struct {
    char host[64];
    char path[160];
    u16  port;
    int  secure;
} Url;

static int parse_url(const char *u, Url *out)
{
    int i = 0, n;

    out->port = 80;
    out->secure = 0;
    if (lower_eq(u, "http://", 7)) u += 7;
    else if (lower_eq(u, "https://", 8)) { u += 8; out->secure = 1; out->port = 443; }

    for (n = 0; u[i] && u[i] != '/' && u[i] != ':' && n < 63; i++)
        out->host[n++] = u[i];
    out->host[n] = 0;
    if (!n) return 0;
    if (u[i] == ':') {
        u32 p = 0;
        for (i++; u[i] >= '0' && u[i] <= '9'; i++) p = p * 10 + (u32)(u[i] - '0');
        if (!p || p > 65535) return 0;
        out->port = (u16)p;
    }
    n = 0;
    if (u[i] != '/') out->path[n++] = '/';
    for (; u[i] && n < 159; i++) out->path[n++] = u[i];
    out->path[n] = 0;
    return 1;
}

static int stop_asked(void)
{
    int k = ui_key();
    return k == UK_ESC;
}

/* The page comes over plain TCP or over TLS; everything above here reads
 * and writes the same way either way. */
static int s_secure;
static int conn_write(const u8 *d, int n) { return s_secure ? tls_write(d, n) : tcp_write(d, n); }
static int conn_read(u8 *b, int n, int w) { return s_secure ? tls_read(b, n, w) : tcp_read(b, n, w); }
static void conn_close(void) { if (s_secure) tls_close(); tcp_close(); }

/* Returns 0 on success; 1 with *moved filled in for a redirect. */
static int fetch_once(const Url *u, char *moved, int moved_max)
{
    static u8 buf[1024];
    static char head[2048];
    char req[320];
    int head_len = 0, in_body = 0, status = 0, n, i, html = 1;
    u32 to, body = 0, total = 0;
    char ctype[48];

    ctype[0] = 0;
    moved[0] = 0;

    if (!ip_parse(u->host, &to)) {
        to = ip_lookup(u->host);
        if (!to) {
            term_colour(C_ACCENT);
            term_printf("Nobody knows a %s.", u->host);
            term_newline();
            return -1;
        }
    }

    term_colour(C_DIM);
    term_printf("%s ...", u->host);
    term_newline();
    main_pump(1);

    if (!tcp_connect(to, u->port, 6000)) {
        term_colour(C_ACCENT);
        term_printf("Could not reach it: %s.", tcp_why());
        term_newline();
        return -1;
    }

    s_secure = u->secure;
    if (s_secure) {
        term_colour(C_DIM);
        say_dim("agreeing on a key...");
        main_pump(1);
        if (!tls_connect(u->host)) {
            term_colour(C_ACCENT);
            term_printf("The secure handshake failed: %s.", tls_why());
            term_newline();
            tcp_close();
            return -1;
        }
        term_colour(C_DIM);
        say_dim("encrypted -- but who is at the other end is not checked yet");
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
            term_colour(C_ACCENT);
            if (n == -1) say("It went quiet, so I stopped waiting.");
            else { term_printf("Cut off: %s.", s_secure ? tls_why() : tcp_why()); term_newline(); }
            break;
        }
        total += (u32)n;

        for (i = 0; i < n; i++) {
            if (in_body) { body++; body_byte(buf[i]); continue; }

            if (head_len < (int)sizeof head - 1) head[head_len++] = (char)buf[i];
            if (head_len >= 4 && head[head_len - 1] == '\n' &&
                (head[head_len - 2] == '\n' ||
                 (head[head_len - 2] == '\r' && head[head_len - 3] == '\n'))) {
                char *line = head, *end;
                head[head_len] = 0;
                in_body = 1;

                /* "HTTP/1.1 200 OK" */
                for (end = line; *end && *end != ' '; end++) { }
                while (*end == ' ') end++;
                while (*end >= '0' && *end <= '9') status = status * 10 + (*end++ - '0');

                /* The headers worth reading. */
                for (line = head; *line; ) {
                    for (end = line; *end && *end != '\n'; end++) { }
                    if (lower_eq(line, "location:", 9)) {
                        char *v = line + 9; int k = 0;
                        while (*v == ' ') v++;
                        while (v < end && *v != '\r' && k < moved_max - 1) moved[k++] = *v++;
                        moved[k] = 0;
                    } else if (lower_eq(line, "content-type:", 13)) {
                        char *v = line + 13; int k = 0;
                        while (*v == ' ') v++;
                        while (v < end && *v != '\r' && *v != ';' && k < 47) ctype[k++] = *v++;
                        ctype[k] = 0;
                    }
                    line = *end ? end + 1 : end;
                }

                term_colour(status >= 200 && status < 300 ? C_DIM : C_ACCENT);
                term_printf("%u  %s", (u32)status, ctype[0] ? ctype : "(no type given)");
                term_newline();

                if (status >= 300 && status < 400 && moved[0]) {
                    conn_close();
                    return 1;
                }
                html = lower_eq(ctype, "text/html", 9);
                if (!html && !lower_eq(ctype, "text/", 5) && ctype[0]) {
                    term_colour(C_DIM);
                    say_dim("Not text, so not shown. Counting the bytes instead.");
                    s_html = -1;
                } else {
                    filter_reset(html);
                }
            }
        }
        if (s_html == -1) body = total;
        if (stop_asked()) {
            flush_word();
            term_colour(C_ACCENT);
            say("Stopped.");
            break;
        }
        main_pump(0);
    }

    flush_word();
    conn_close();
    if (s_col) term_newline();
    term_colour(C_DIM);
    term_printf("%u bytes.", body);
    term_newline();
    return 0;
}

void http_fetch(const char *url)
{
    Url u;
    char moved[192];
    int hops;

    if (!parse_url(url, &u)) { say("That does not read as a web address."); return; }

    term_plain(1);
    for (hops = 0; hops < 4; hops++) {
        if (fetch_once(&u, moved, sizeof moved) != 1) { term_plain(0); return; }

        term_colour(C_DIM);
        term_printf("moved to %s", moved);
        term_newline();
        if (moved[0] == '/') {           /* the same server, another path  */
            int i;
            for (i = 0; moved[i] && i < 159; i++) u.path[i] = moved[i];
            u.path[i] = 0;
        } else if (!parse_url(moved, &u)) {
            say("And I cannot read where to.");
            term_plain(0);
            return;
        }
    }
    say("It keeps sending me somewhere else. Giving up.");
    term_plain(0);
}
