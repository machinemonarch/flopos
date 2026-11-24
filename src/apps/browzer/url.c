/* ===========================================================================
 * url.c -- web addresses: taking them apart, and joining a link to a page.
 *
 * A link on a page is usually not a whole address.  "/about" means this
 * server, "about" means beside this page, "../about" one level up,
 * "//other.host/x" another server by the same scheme, "?page=2" this page
 * with a different question.  Working those out is fiddly in exactly the
 * way that goes wrong quietly, so it lives here on its own, with nothing
 * it depends on, and was checked against Python's own resolver on a long
 * list of awkward cases.
 * ===========================================================================
 */
#include "web.h"

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

/* A byte that cannot go into a request as it is: a space, a control
 * character, a quote or angle bracket, anything past ASCII.  Such bytes are
 * sent as %XX, as browsers send them; tabs and line ends are dropped, as
 * browsers drop them.  A line end left in would end the request's first
 * line early, and whatever followed it would be read as a header. */
static int needs_escape(u8 c) { return c <= ' ' || c >= 0x7F || c == '"' || c == '<' || c == '>'; }

int url_parse(const char *u, Url *out)
{
    static const char hexd[] = "0123456789ABCDEF";
    int i = 0, n;

    out->port = 80;
    out->secure = 0;
    if (lower_eq(u, "http://", 7)) u += 7;
    else if (lower_eq(u, "https://", 8)) { u += 8; out->secure = 1; out->port = 443; }

    for (n = 0; u[i] && u[i] != '/' && u[i] != ':' && u[i] != '?' && u[i] != '#'; i++) {
        if (n >= 63 || needs_escape((u8)u[i])) return 0;    /* too long, or not a name */
        out->host[n++] = (u[i] >= 'A' && u[i] <= 'Z') ? (char)(u[i] + 32) : u[i];
    }
    out->host[n] = 0;
    if (!n) return 0;
    if (u[i] == ':') {
        u32 p = 0;
        for (i++; u[i] >= '0' && u[i] <= '9'; i++) if ((p = p * 10 + (u32)(u[i] - '0')) > 65535) return 0;
        if (!p) return 0;
        out->port = (u16)p;
    }
    n = 0;
    if (u[i] != '/') out->path[n++] = '/';
    for (; u[i] && u[i] != '#' && n < URL_PATH_MAX - 1; i++) {
        u8 c = (u8)u[i];
        if (c == '\t' || c == '\r' || c == '\n') continue;
        if (!needs_escape(c)) { out->path[n++] = (char)c; continue; }
        if (n + 3 > URL_PATH_MAX - 1) break;
        out->path[n++] = '%'; out->path[n++] = hexd[c >> 4]; out->path[n++] = hexd[c & 15];
    }
    out->path[n] = 0;
    return 1;
}

void url_text(const Url *u, char *out, int max)
{
    int def = (u->secure && u->port == 443) || (!u->secure && u->port == 80);
    if (def) fmt_str(out, max, "%s://%s%s", u->secure ? "https" : "http", u->host, u->path);
    else     fmt_str(out, max, "%s://%s:%u%s", u->secure ? "https" : "http",
                     u->host, (u32)u->port, u->path);
}

/* Fold "." and ".." out of a path that starts with '/', as a stack of
 * segments: a name goes on, ".." takes the last one off, "." does
 * nothing.  If the path ends in either of them it ends in a slash, since
 * it now names a directory.  The query, if any, is left exactly as it was. */
static void normalise(char *path)
{
    char in[URL_PATH_MAX], out[URL_PATH_MAX];
    int seg_at[64], seg_len[64], nseg = 0, trailing = 0;
    int i = 1, q, o = 0, k;

    for (q = 0; path[q] && path[q] != '?'; q++) in[q] = path[q];
    in[q] = 0;

    while (i <= q) {
        int s = i, len = 0, last;
        while (s + len < q && in[s + len] != '/') len++;
        last = (s + len >= q);
        if (len == 1 && in[s] == '.') {
            trailing = last;
        } else if (len == 2 && in[s] == '.' && in[s + 1] == '.') {
            if (nseg) nseg--;
            trailing = last;
        } else if (nseg < 64) {
            seg_at[nseg] = s;
            seg_len[nseg] = len;
            nseg++;
            trailing = 0;
        }
        i = s + len + 1;
    }

    out[o++] = '/';
    for (k = 0; k < nseg; k++) {
        int j;
        for (j = 0; j < seg_len[k] && o < URL_PATH_MAX - 2; j++) out[o++] = in[seg_at[k] + j];
        if (k < nseg - 1 && o < URL_PATH_MAX - 2) out[o++] = '/';
    }
    if (trailing && out[o - 1] != '/') out[o++] = '/';
    for (; path[q] && o < URL_PATH_MAX - 1; q++) out[o++] = path[q];
    out[o] = 0;
    for (i = 0; i <= o; i++) path[i] = out[i];
}

int url_resolve(const char *base, const char *href, char *out, int max)
{
    Url b;
    char path[URL_PATH_MAX];
    int n = 0, i, last = 0;

    while (*href == ' ' || *href == '\t' || *href == '\n') href++;
    if (!*href || *href == '#') return 0;
    if (lower_eq(href, "mailto:", 7) || lower_eq(href, "javascript:", 11) ||
        lower_eq(href, "tel:", 4) || lower_eq(href, "data:", 5) ||
        lower_eq(href, "ftp:", 4)) return 0;
    if (lower_eq(href, "http://", 7) || lower_eq(href, "https://", 8)) {
        Url a;
        if (!url_parse(href, &a)) return 0;
        normalise(a.path);
        url_text(&a, out, max);
        return 1;
    }
    if (!url_parse(base, &b)) return 0;
    if (href[0] == '/' && href[1] == '/') {
        char full[URL_PATH_MAX + 80];
        Url a;
        fmt_str(full, sizeof full, "%s:%s", b.secure ? "https" : "http", href);
        if (!url_parse(full, &a)) return 0;
        normalise(a.path);
        url_text(&a, out, max);
        return 1;
    }

    if (href[0] == '/') {
        for (i = 0; href[i] && href[i] != '#' && n < URL_PATH_MAX - 1; i++) path[n++] = href[i];
    } else if (href[0] == '?') {
        for (i = 0; b.path[i] && b.path[i] != '?' && n < URL_PATH_MAX - 1; i++) path[n++] = b.path[i];
        for (i = 0; href[i] && href[i] != '#' && n < URL_PATH_MAX - 1; i++) path[n++] = href[i];
    } else {
        for (i = 0; b.path[i] && b.path[i] != '?'; i++) if (b.path[i] == '/') last = i;
        for (i = 0; i <= last && n < URL_PATH_MAX - 1; i++) path[n++] = b.path[i];
        for (i = 0; href[i] && href[i] != '#' && n < URL_PATH_MAX - 1; i++) path[n++] = href[i];
    }
    path[n] = 0;
    normalise(path);
    for (i = 0; path[i]; i++) b.path[i] = path[i];
    b.path[i] = 0;
    url_text(&b, out, max);
    return 1;
}
