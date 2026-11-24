/* ===========================================================================
 * cookie.c -- cookies, the least of them.
 *
 * Some sites need to remember you between pages -- a login, a chat's
 * name -- and the way they do it is a cookie: a name and value the site
 * hands over in Set-Cookie and gets back in Cookie on every later request.
 * Here they are kept as close to harmless as they can be:
 *
 *   in memory only, forgotten when browzer closes, never on the floppy;
 *   sent back only to the site that set them (or, if it said so, to its
 *   own subdomains), and only along the path it named;
 *   marked Secure, only ever over https;
 *   and browzer never loads anything a page merely embeds, so no other
 *   site can set one by being part of a page.
 *
 * Expiry dates are not kept: a cookie lasts until browzer closes, or until
 * its site sets it again with Max-Age=0 or an Expires already past (a
 * year before 2000), which is how sites log you out.
 * ===========================================================================
 */
#include "web.h"

#define JAR 32
typedef struct {
    char host[64];
    char path[64];
    char name[64];
    char value[1024];
    u8   subdomains, secure;
} Cookie;

static Cookie s_jar[JAR];
static int    s_n;

static char lc(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }

static int same_ci(const char *a, const char *b)
{
    while (*a && lc(*a) == lc(*b)) { a++; b++; }
    return !*a && !*b;
}

static int same(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static int starts_ci(const char *s, int n, const char *w)
{
    int i;
    for (i = 0; w[i]; i++) if (i >= n || lc(s[i]) != w[i]) return 0;
    return 1;
}

/* host is the cookie's host, or one of its subdomains when allowed. */
static int host_ok(const Cookie *c, const char *host)
{
    int hl = 0, cl = 0;
    if (same_ci(c->host, host)) return 1;
    if (!c->subdomains) return 0;
    while (host[hl]) hl++;
    while (c->host[cl]) cl++;
    return hl > cl + 1 && host[hl - cl - 1] == '.' && same_ci(c->host, host + hl - cl);
}

static int path_ok(const Cookie *c, const char *path)
{
    int i;
    for (i = 0; c->path[i]; i++) if (path[i] != c->path[i]) return 0;
    return c->path[i - 1] == '/' || !path[i] || path[i] == '/' || path[i] == '?';
}

/* Copy n bytes of s, trimmed of spaces, into out (cap). */
static void take(char *out, int cap, const char *s, int n)
{
    int k = 0;
    while (n > 0 && (*s == ' ' || *s == '\t')) { s++; n--; }
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) n--;
    if (n > 1 && s[0] == '"' && s[n - 1] == '"') { s++; n -= 2; }
    while (k < n && k < cap - 1) { out[k] = s[k]; k++; }
    out[k] = 0;
}

/* A Set-Cookie header's value, from u; line ends at CR, LF or NUL. */
void cookie_set(const Url *u, const char *line)
{
    static Cookie c;
    const char *p = line, *e;
    int len = 0, i, gone = 0, seg;

    while (p[len] && p[len] != '\r' && p[len] != '\n') len++;
    for (i = 0; i < (int)sizeof c; i++) ((char *)&c)[i] = 0;

    /* name=value, up to the first ; */
    for (seg = 0; seg < len && p[seg] != ';'; seg++) { }
    for (e = p; e < p + seg && *e != '='; e++) { }
    if (e >= p + seg) return;                   /* not name=value: ignored */
    take(c.name, sizeof c.name, p, (int)(e - p));
    take(c.value, sizeof c.value, e + 1, (int)(p + seg - e - 1));
    if (!c.name[0]) return;

    for (i = 0; u->host[i] && i < (int)sizeof c.host - 1; i++) c.host[i] = u->host[i];
    /* Path: by default, where the page that set it lives. */
    {
        int last = 0;
        for (i = 0; u->path[i] && u->path[i] != '?' && i < (int)sizeof c.path - 1; i++)
            if (u->path[i] == '/') last = i;
        for (i = 0; i <= last && i < (int)sizeof c.path - 1; i++) c.path[i] = u->path[i];
        if (!c.path[0]) c.path[0] = '/';
    }

    /* The attributes after it. */
    while (seg < len) {
        const char *a = p + seg + 1;
        int n = 0;
        while (a + n < p + len && a[n] != ';') n++;
        while (n > 0 && *a == ' ') { a++; n--; }
        if (starts_ci(a, n, "secure")) c.secure = 1;
        else if (starts_ci(a, n, "max-age=")) {
            const char *v = a + 8;
            if (*v == '-' || *v == '0') gone = 1;
        } else if (starts_ci(a, n, "expires=")) {
            int k, y = 0, digits = 0;
            for (k = 8; k < n; k++) {
                if (a[k] >= '0' && a[k] <= '9') { y = y * 10 + (a[k] - '0'); digits++; }
                else { if (digits == 4) break; y = 0; digits = 0; }
            }
            if (digits == 4 && y < 2000) gone = 1;
        } else if (starts_ci(a, n, "path=")) {
            take(c.path, sizeof c.path, a + 5, n - 5);
            if (c.path[0] != '/') { c.path[0] = '/'; c.path[1] = 0; }
        } else if (starts_ci(a, n, "domain=")) {
            /* Only the site itself or a parent of it, never a bare top
             * level like .com: a dot must be left in what it names. */
            char d[64];
            int dl = 0, hl = 0, dot = 0, k;
            take(d, sizeof d, a + 7, n - 7);
            k = d[0] == '.' ? 1 : 0;
            while (d[k + dl]) { if (d[k + dl] == '.') dot = 1; dl++; }
            while (u->host[hl]) hl++;
            if (!dot || dl > hl) return;
            if (!same_ci(d + k, u->host + hl - dl) || (hl > dl && u->host[hl - dl - 1] != '.')) return;
            for (i = 0; i <= dl; i++) c.host[i] = d[k + i];
            c.subdomains = 1;
        }
        seg += 1 + (int)(a - (p + seg + 1)) + n;
    }

    /* Secure is for https: a plain page can neither set such a cookie nor
     * put its own in place of one (RFC 6265bis, 4.1.3 and 5.7). */
    if (!u->secure) {
        if (c.secure) return;
        for (i = 0; i < s_n; i++)
            if (s_jar[i].secure && same(s_jar[i].name, c.name) &&
                (host_ok(&s_jar[i], u->host) || host_ok(&c, s_jar[i].host))) return;
    }

    /* Replacing one already held, or forgetting it. */
    for (i = 0; i < s_n; i++)
        if (same_ci(s_jar[i].host, c.host) && s_jar[i].subdomains == c.subdomains &&
            same(s_jar[i].name, c.name) && same(s_jar[i].path, c.path)) {
            if (gone) { s_jar[i] = s_jar[--s_n]; return; }
            s_jar[i] = c;
            return;
        }
    if (gone) return;
    if (s_n == JAR) {                           /* full: the oldest goes  */
        for (i = 0; i < JAR - 1; i++) s_jar[i] = s_jar[i + 1];
        s_n--;
    }
    s_jar[s_n++] = c;
}

/* "Cookie: a=b; c=d\r\n" for a request to u, into out; its length, 0 if
 * there are none to send. */
int cookie_header(const Url *u, char *out, int cap)
{
    int n = 0, i, any = 0;
    const char *lead = "Cookie: ";
    for (i = 0; i < s_n; i++) {
        const Cookie *c = &s_jar[i];
        int k;
        int nl = 0, vl = 0;
        if (!host_ok(c, u->host) || !path_ok(c, u->path) || (c->secure && !u->secure)) continue;
        while (c->name[nl]) nl++;
        while (c->value[vl]) vl++;
        if (n + (any ? 2 : 8) + nl + 1 + vl + 3 > cap) continue;      /* whole or not at all, and room for the end */
        if (!any) { for (k = 0; lead[k]; k++) out[n++] = lead[k]; any = 1; }
        else { out[n++] = ';'; out[n++] = ' '; }
        for (k = 0; k < nl; k++) out[n++] = c->name[k];
        out[n++] = '=';
        for (k = 0; k < vl; k++) out[n++] = c->value[k];
    }
    if (!any) return 0;
    out[n++] = '\r'; out[n++] = '\n';
    out[n] = 0;
    return n;
}
