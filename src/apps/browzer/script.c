/* ===========================================================================
 * script.c -- a page's styles and scripts.
 *
 * When a page has loaded and its tree is built (dom.c), its style sheets
 * are read (css.c).  Then its scripts -- but only on a site that has been
 * allowed, with one key; on any other, they are counted and left alone:
 * "3 scripts, none of them run."
 *
 * On an allowed site, each script goes through the gate, in browzer,
 * before anything is fetched:
 *   - a file from another site only over https, with no cookies
 *     (script_from_ok below);
 *   - a script over 100 KB, or past 250 KB for the page, or past the
 *     sixteenth, is not fetched either.
 * What passes runs in the script room (the kernel's second wall, src/room/),
 * one after another in the order of the page, each with its time.
 *
 * The room's requests come to door() below.  Each is checked: the element
 * must exist, a text must fit, and some things stop the script outright:
 * fetching from or going to another site where that is not allowed, asking for anything
 * not on the list.  Text made into code by a script (an onclick attribute
 * it set, a <script> it wrote out) is never run.
 *
 * Then the page is drawn again from the tree, with its styles: what is
 * hidden left out, what is pinned over the page (pop-ups) never drawn.
 * ===========================================================================
 */
#include "dom.h"
#include "../../room/room.h"
#include "../../room/door.h"

#define SCRIPT_MAX   (100 * 1024)       /* one written in the page, or a style sheet: held whole */
#define SCRIPT_BIG   (256 * 1024)       /* one fetched: streamed to the room, which holds this much */
#define PAGE_SCRIPTS (640 * 1024)
#define MAX_SCRIPTS  16
#define CSS_MAX      (64 * 1024)
#define CSS_PAGE     (200 * 1024)
#define LOAD_MS      2000                /* each script, as the page loads  */
#define LOAD_PER_KB  60                  /* and this much more for each KB of it: a */
#define LOAD_MAX     15000               /* library takes time just to read (jQuery, 87 KB: 3 s) */
#define EVENT_MS     500                 /* each click, each timer          */
#define FETCHES      30                  /* a page's fetches                */

int g_dom_dirty;
static char  s_host[64];                 /* the page's site                 */
static int   s_on;                       /* scripts allowed here            */
static int   s_room;                     /* the room is open and set up     */
static int   s_count, s_ran, s_other, s_big, s_failed, s_textmade;
static char  s_note[160];
static char  s_fail[120];
static u32   s_meter, s_page_nodes, s_cur_script, s_next_timer, s_last_draw;
static int   s_loading, s_fetches;
static char  s_go[PAGE_HREF_MAX];        /* where a script asked to go      */
static u8    s_task[ROOM_MAIL_SIZE];
static u8    s_ans[8200];              /* a module's list of imports is the longest answer */
static u8    s_doorbuf[65536];
static u8    s_req[32768];             /* the room asks in at most 32 KB (dom.c) */
static char  s_fetched[SCRIPT_MAX + 1];
static char  s_scratch[65536];

const char *script_note(void) { return s_note; }

/* 0 if a and b are the same letters, whatever their case (strcmp's way) */
static int lower_cmp_n(const char *a, const char *b, int n)
{
    while (n--) {
        char x = *a++, y = *b++;
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return 1;
        if (!x) return 0;
    }
    return 0;
}

static int lower_cmp(const char *a, const char *b)
{
    while (*a && *b && ((*a | 32) == (*b | 32) || (*a == *b))) { a++; b++; }
    return *a || *b;
}

static int lower_has(const char *s, const char *w)
{
    int i, k;
    for (i = 0; s[i]; i++) {
        for (k = 0; w[k] && s[i + k] && (s[i + k] | 32) == (w[k] | 32); k++) { }
        if (!w[k]) return 1;
    }
    return 0;
}

/* What is being fetched, on the bottom line while it comes. */
static Page *s_pg;
static void note_loading(const char *what)
{
    if (!s_pg) return;
    fmt_str(s_pg->note, sizeof s_pg->note, "fetching %s...", what);
    s_pg->note_col = C_DIM;
    if (s_pg->progress) s_pg->progress();
}

/* ---- sites ------------------------------------------------------------------ */
/* The part of a host name that is the site: example.se from www.example.se,
 * and bbc.co.uk from www.bbc.co.uk. */
static const char *site_of(const char *h)
{
    int n = 0, dots[8], nd = 0, i, ip = 1;
    while (h[n]) { if (!((h[n] >= '0' && h[n] <= '9') || h[n] == '.')) ip = 0; n++; }
    if (ip) return h;                        /* an address: all of it is the site */
    for (i = n - 1; i >= 0 && nd < 8; i--) if (h[i] == '.') dots[nd++] = i;
    if (nd < 2) return h;
    /* a short second-level name under a country (co.uk, com.au): one more */
    if (n - dots[0] - 1 == 2 && dots[0] - dots[1] - 1 <= 3 && nd >= 2) return nd >= 3 ? h + dots[2] + 1 : h;
    return h + dots[1] + 1;
}

static int same_site(const char *a, const char *b)
{
    const char *x = site_of(a), *y = site_of(b);
    while (*x && (*x | 32) == (*y | 32)) { x++; y++; }
    return !*x && !*y;
}

/* A script from another site, on a site that is allowed: most sites keep
 * theirs under a second name of their own (bcbits.com for bandcamp.com),
 * and nothing in the page says whose a name is -- so any site will do,
 * but only over https, and with no cookies sent or kept (s_anon), so a
 * tracker among them cannot follow anyone from site to site.  What comes
 * runs in the same room, with the same limits and tripwires as the site's
 * own scripts. */
static int s_anon;
static void wire(const char *a, const char *b);

static int script_from_ok(const Url *u, const char *url)
{
    if (same_site(u->host, s_host)) return 1;
    if (!u->secure) { s_other++; wire("not fetched, another site without https: ", url); return 0; }
    return 1;
}

/* SITES.TXT: the sites scripts are allowed on, a line each. */
static char s_sites[2048];
static AppFile s_sites_file;
static int s_sites_loaded, s_sites_ok;

static void sites_load(void)
{
    if (s_sites_loaded) return;
    s_sites_loaded = 1;
    if (A->file_find("SITES.TXT", &s_sites_file) && s_sites_file.size == sizeof s_sites &&
        A->file_read(&s_sites_file, (u8 *)s_sites, sizeof s_sites) == (int)sizeof s_sites) s_sites_ok = 1;
    else memset(s_sites, ' ', sizeof s_sites);
}

static int sites_has(const char *site, int *at)
{
    u32 i = 0;
    int n = 0;
    while (site[n]) n++;
    while (i < sizeof s_sites) {
        u32 st = i, k;
        while (i < sizeof s_sites && s_sites[i] != '\n') i++;
        if ((int)(i - st) == n) {
            for (k = 0; k < (u32)n && (s_sites[st + k] | 32) == (site[k] | 32); k++) { }
            if (k == (u32)n) { if (at) *at = (int)st; return 1; }
        }
        i++;
    }
    return 0;
}

int script_site_on(const char *host)
{
    if (!host) return s_on;
    sites_load();
    return sites_has(site_of(host), 0);
}

/* ---- fetching ------------------------------------------------------------------ */
/* An address, fetched into out: its status and type.  Returns its length,
 * or -1.  Used for style sheets, scripts, and scripts' own fetches; it
 * sends the site's cookies, as a page's own request would. */
static int (*s_sink)(const u8 *p, int n);   /* set: web_get hands the body here, not to out */
/* A request to another site, as a script's fetch() makes it (CORS): it
 * says where it comes from, sends no cookies and keeps none, and its
 * answer is only for the script if the other site says so. */
static int  s_cross;
static char s_origin[80], s_extra[160];
static char s_acao[96], s_acam[96];      /* Access-Control-Allow-Origin, -Methods */

int web_get(const char *url, const char *method, const u8 *body, int blen, const char *btype,
            char *out, int cap, int *status, char *ctype, int ccap)
{
    static u8 buf[2048];
    char req[PAGE_HREF_MAX + 600], ck[512];
    Url u;
    int n, i, state = 0, got = 0, hops;
    char loc[PAGE_HREF_MAX], where[PAGE_HREF_MAX + 16];
    *status = 0;
    if (ctype && ccap) ctype[0] = 0;
    for (i = 0; url[i] && i < PAGE_HREF_MAX + 15; i++) where[i] = url[i];
    where[i] = 0;
    for (hops = 0; hops < 3; hops++) {
        char head[2048];
        Head hd = { 0, 0, 0, 0 };
        if (!url_parse(where, &u)) return -1;
        if (!A->conn_open(u.host, u.port, u.secure, 0)) return -1;
        ck[0] = 0;
        s_acao[0] = s_acam[0] = 0;
        if (s_cross) fmt_str(ck, sizeof ck, "Origin: %s\r\n%s", s_origin, s_extra);
        else if (!s_anon) cookie_header(&u, ck, sizeof ck);
        if (body && blen >= 0 && method && method[0] == 'P')
            fmt_str(req, sizeof req, "%s %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: " BROWZER_UA "\r\n"
                    "Content-Type: %s\r\nContent-Length: %u\r\n%sConnection: close\r\n\r\n",
                    method, u.path, u.host, A->os_version(), btype && btype[0] ? btype : "text/plain", (u32)blen, ck);
        else
            fmt_str(req, sizeof req, "%s %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: " BROWZER_UA "\r\n"
                    "Accept: */*\r\n%sConnection: close\r\n\r\n", method && method[0] ? method : "GET", u.path, u.host, A->os_version(), ck);
        for (n = 0; req[n]; n++) { }
        A->conn_write((const u8 *)req, n);
        if (body && blen > 0 && method && method[0] == 'P') A->conn_write(body, blen);
        state = 0; got = 0; loc[0] = 0;
        for (;;) {
            n = A->conn_read(buf, sizeof buf, 15000);
            if (n <= 0) break;
            for (i = 0; i < n; i++) {
                if (!state) {
                    if (head_byte(&hd, head, (int)sizeof head, buf[i])) {
                        char *ln = head, *end;
                        state = 1;
                        for (end = ln; *end && *end != ' '; end++) { }
                        while (*end == ' ') end++;
                        while (*end >= '0' && *end <= '9') *status = *status * 10 + (*end++ - '0');
                        for (ln = head; *ln; ) {
                            for (end = ln; *end && *end != '\n'; end++) { }
                            if (((ln[0] | 32) == 'l') && ((ln[1] | 32) == 'o') && ((ln[8] | 32) == ':' || ln[8] == ':')) {
                                char *v = ln + 9; int k = 0;
                                while (*v == ' ') v++;
                                while (v < end && *v != '\r' && k < PAGE_HREF_MAX - 1) loc[k++] = *v++;
                                loc[k] = 0;
                            } else if (((ln[0] | 32) == 's') && ((ln[4] | 32) == '-') && ((ln[5] | 32) == 'c') && ln[10] == ':') {
                                char save = *end;
                                *end = 0;
                                if (!s_cross && !s_anon) cookie_set(&u, ln + 11);   /* another site's: not kept */
                                *end = save;
                            } else if (!lower_cmp_n(ln, "access-control-allow-", 21) && (end - ln) > 28) {
                                char *v = ln + 21, *dst = 0;
                                int k = 0;
                                if (!lower_cmp_n(v, "origin:", 7)) { dst = s_acao; v += 7; }
                                else if (!lower_cmp_n(v, "methods:", 8)) { dst = s_acam; v += 8; }
                                if (dst) {
                                    while (*v == ' ') v++;
                                    while (v < end && *v != '\r' && k < 95) dst[k++] = *v++;
                                    dst[k] = 0;
                                }
                            } else if (ctype && ((ln[0] | 32) == 'c') && ((ln[8] | 32) == 't') && ln[12] == ':') {
                                char *v = ln + 13; int k = 0;
                                while (*v == ' ') v++;
                                while (v < end && *v != '\r' && *v != ';' && k < ccap - 1) ctype[k++] = *v++;
                                ctype[k] = 0;
                            }
                            ln = *end ? end + 1 : end;
                        }
                    }
                    continue;
                }
                if (s_sink && *status == 200) {      /* the rest of this read, at once */
                    if (!s_sink(buf + i, n - i)) { got = -1; break; }
                    got += n - i;
                    break;
                }
                if (!s_sink && got < cap) out[got++] = (char)buf[i];
            }
            if (got < 0 || (!s_sink && got >= cap)) break;
            main_pump(0);
        }
        A->conn_close();
        if (got < 0) return -1;
        if (*status >= 300 && *status < 400 && loc[0]) {
            /* a redirect: to the same site only */
            char next[PAGE_HREF_MAX + 16];
            Url to;
            if (!url_resolve(where, loc, next, sizeof next) || !url_parse(next, &to) || !same_site(to.host, u.host)) return -1;
            for (i = 0; next[i] && i < PAGE_HREF_MAX + 15; i++) where[i] = next[i];
            where[i] = 0;
            *status = 0;
            if (method && method[0] == 'P') { method = "GET"; body = 0; blen = 0; }
            continue;
        }
        return got;
    }
    return -1;
}

/* ---- the room ------------------------------------------------------------------- */
static void wire(const char *a, const char *b)
{
    char line[300];
    fmt_str(line, sizeof line, "[script] %s%s\n", a, b);
    A->wire(line);
}

static int door(u8 *buf, u32 len, u32 cap);

/* One task for the room: its answer code, or -1 if it had to be stopped. */
static int task(u32 n, u32 ms, char *why, int whycap)
{
    int r = A->room_task(s_task, n, s_ans, sizeof s_ans - 1, ms, door, s_doorbuf, sizeof s_doorbuf);
    if (why && whycap) why[0] = 0;
    if (r < 0) {
        s_room = 0;
        if (why) fmt_str(why, (u32)whycap, "%s", A->room_why());
        return -1;
    }
    s_ans[r] = 0;
    if (r >= 5) s_next_timer = (u32)s_ans[1] | ((u32)s_ans[2] << 8) | ((u32)s_ans[3] << 16) | ((u32)s_ans[4] << 24);
    if (why && r > 6) fmt_str(why, (u32)whycap, "%s", (const char *)s_ans + 6);
    return s_ans[0];
}

static int prevented(void) { return s_ans[5] & 1; }

static void put32(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }

static int room_start(void)
{
    u8 c[6];
    union { u64 u; double d; } now;
    u32 i;
    if (!A->room_open()) {
        fmt_str(s_fail, sizeof s_fail, "the script room: %s", A->room_why());
        wire("no room: ", A->room_why());
        return 0;
    }
    /* the date, in milliseconds since 1970, from the laptop's clock */
    now.d = 0;
    if (A->clock(c)) {
        s32 y = 2000 + c[0], m = c[1], d = c[2], era, yoe, doy, doe;
        s32 days;
        y -= m <= 2;
        era = y / 400;
        yoe = y - era * 400;
        doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
        doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        days = era * 146097 + doe - 719468;
        now.d = ((double)days * 86400.0 + c[3] * 3600.0 + c[4] * 60.0 + c[5]) * 1000.0;
    }
    s_task[0] = T_INIT;
    for (i = 0; i < 8; i++) s_task[1 + i] = (u8)(now.u >> (8 * i));
    if (task(9, 3000, s_fail, sizeof s_fail) != A_OK) {
        if (!s_fail[0]) fmt_str(s_fail, sizeof s_fail, "the script room did not start");
        return 0;
    }
    s_room = 1;
    return 1;
}

static const char *base_name(const char *u)
{
    const char *b = u, *p;
    for (p = u; *p && *p != '?'; p++) if (*p == '/' && p[1] && p[1] != '?') b = p + 1;
    return b;
}

/* Run one script's text in the room: in pieces if it is long. */
/* A script's text goes to the room in pieces of 60000 (T_SRC); the last
 * stays in s_task, after where the task that runs it will need its
 * header, until that task is sent.  It comes from a buffer, or straight
 * from the network as it arrives (feed_bytes, through web_get's sink),
 * so a fetched script never needs room for all of it here. */
#define PIECE 60000
static u32 s_feed_n, s_feed_total;
static int s_feed_bad;                   /* 1 too big, 2 the room said no */

static int feed_bytes(const u8 *p, int n)
{
    while (n > 0) {
        u32 take = PIECE - s_feed_n;
        if (take > (u32)n) take = (u32)n;
        if (s_feed_total + take > SCRIPT_BIG) { s_feed_bad = 1; return 0; }
        memcpy(s_task + 1 + s_feed_n, p, take);
        s_feed_n += take; s_feed_total += take; p += take; n -= (int)take;
        if (s_feed_n == PIECE) {
            s_task[0] = T_SRC;
            if (task(PIECE + 1, 1000, 0, 0) != A_OK) { s_feed_bad = 2; return 0; }
            s_feed_n = 0;
        }
    }
    return 1;
}

static void feed_start(void) { s_feed_n = s_feed_total = 0; s_feed_bad = 0; }

/* Pieces sent, then given up: the room forgets them. */
static void feed_drop(void)
{
    if (s_feed_total > s_feed_n && s_room) { s_task[0] = T_SRCDROP; (void)task(1, 1000, 0, 0); }
    feed_start();
}

/* The fetched script at url, fed to the room.  Its length, or -1 (and
 * s_fail says why). */
static int feed_url(const char *url)
{
    int st, got;
    Url u;
    feed_start();
    s_sink = feed_bytes;
    s_anon = url_parse(url, &u) && !same_site(u.host, s_host);   /* another site's: no cookies */
    got = web_get(url, "GET", 0, 0, 0, 0, 0, &st, 0, 0);
    s_sink = 0; s_anon = 0;
    if (got < 0 || st != 200 || s_feed_bad) {
        if (s_feed_bad == 1) { s_big++; fmt_str(s_fail, sizeof s_fail, "%s is too big (over %u KB)", base_name(url), (u32)SCRIPT_BIG >> 10); wire("too big: ", url); }
        else { s_failed++; fmt_str(s_fail, sizeof s_fail, "%s did not come", base_name(url)); }
        feed_drop();
        return -1;
    }
    return got;
}

static int feed_text(const char *src, u32 len)
{
    feed_start();
    if (!feed_bytes((const u8 *)src, (int)len)) { feed_drop(); return -1; }
    return (int)len;
}

/* The last piece, moved up behind a header of hdr bytes. */
static u32 feed_last(u32 hdr)
{
    memmove(s_task + hdr, s_task + 1, s_feed_n);
    return hdr + s_feed_n;
}

/* Run what was fed, as the script name (at node). */
static void run_fed(u32 len, const char *name, u32 node)
{
    u32 n, k, hdr, ms = LOAD_MS + (len >> 10) * LOAD_PER_KB, t0 = A->ms();
    char why[200], head[80];
    int r;
    if (ms > LOAD_MAX) ms = LOAD_MAX;
    hdr = 9;
    for (k = 0; name[k] && k < 60; k++) head[k] = name[k];
    head[k] = 0;
    hdr += k + 1;
    n = feed_last(hdr);
    s_task[0] = T_RUN;
    put32(s_task + 1, ms);
    put32(s_task + 5, node);
    for (k = 0; k <= hdr - 10; k++) s_task[9 + k] = (u8)head[k];
    s_cur_script = node;
    r = task(n, ms, why, sizeof why);
    s_cur_script = 0;
    feed_start();
    {
        char t[200];
        if (r == A_OK) fmt_str(t, sizeof t, "%s, %u KB, %u ms", name, len >> 10, A->ms() - t0);
        else fmt_str(t, sizeof t, "%s, %u KB, %u ms: %s", name, len >> 10, A->ms() - t0, why);
        wire(r == A_OK ? "ran " : "stopped: ", t);
    }
    if (r == A_OK) { s_ran++; return; }
    s_failed++;
    if (r < 0) fmt_str(s_fail, sizeof s_fail, "%s: the room was stopped: %s", name, why);
    else fmt_str(s_fail, sizeof s_fail, "%s was stopped: %s", name, why);
    if (s_fail[0] >= 'a' && s_fail[0] <= 'z' && name[0] == 'a' && name[1] == ' ') s_fail[0] = (char)(s_fail[0] - 32);
    wire(s_fail, "");
}

static void run_text(const char *src, u32 len, const char *name, u32 node)
{
    if (feed_text(src, len) < 0) { s_failed++; fmt_str(s_fail, sizeof s_fail, "%s: the room would not take it", name); return; }
    run_fed(len, name, node);
}

static int is_js(u32 n)
{
    const char *t = dom_attr(n, "type");
    int k;
    char low[40];
    if (!t || !t[0]) return 1;
    for (k = 0; t[k] && k < 39; k++) low[k] = (char)(t[k] >= 'A' && t[k] <= 'Z' ? t[k] + 32 : t[k]);
    low[k] = 0;
    return !lower_cmp(low, "text/javascript") || !lower_cmp(low, "application/javascript") ||
           !lower_cmp(low, "module") || !lower_cmp(low, "text/ecmascript") || !lower_cmp(low, "application/x-javascript");
}

/* The next <script> in the page not yet seen, in order. */
static u32 next_script(void)
{
    u32 n;
    for (n = 2; n < g_nodes; n++) {
        Node *x = &g_node[n];
        if (x->type != N_ELEM || (x->flags & NF_SCRIPT_DONE) || !x->parent) continue;
        if (dom_tag(n)[0] == 's' && !lower_cmp(dom_tag(n), "script")) return n;
    }
    return 0;
}

/* ---- modules ------------------------------------------------------------------------
 * <script type="module">: browzer fetches the module (by the same rules as
 * any script: this site, or a library host), has the room read it
 * (T_MODSCAN), which answers the addresses it imports from; those are
 * fetched and read the same way, then each module is run after the ones
 * it imports from (T_MODRUN).  A module is read and run once a page,
 * however many import it.  Addresses are relative to the module, or
 * names the page's import map gives (<script type="importmap">).  The
 * room's import() asks through the door (D_IMPORT): it is loaded the
 * same way once the task that asked is over, then settled (T_DYNDONE). */
#define MOD_MAX  48
#define MOD_URL  (PAGE_HREF_MAX + 16)
static char s_mod_url[MOD_MAX][MOD_URL];
static signed char s_mod_state[MOD_MAX];         /* 0 being read, 1 ran, -1 failed */
static int  s_nmod;
static char s_imap[4096];                /* the page's import map, as JSON  */
static struct { u32 ticket; char spec[MOD_URL]; char base[MOD_URL]; } s_dyn[8];
static int  s_ndyn;
static u32  s_dyn_ticket;

/* "key": "value" pairs inside the import map's "imports": the value for
 * spec, the longest key ending in / that spec starts with, or none. */
static int imap_lookup(const char *spec, char *out, int cap)
{
    const char *p = s_imap, *best_v = 0;
    int best_k = -1, sl = 0;
    while (spec[sl]) sl++;
    while (*p && !(p[0] == '"' && !lower_cmp_n(p + 1, "imports\"", 8))) p++;
    if (!*p) return 0;
    while (*p && *p != '{') p++;
    if (!*p) return 0;
    p++;
    for (;;) {
        const char *k, *v;
        int kl = 0, i;
        while (*p && *p != '"' && *p != '}') p++;
        if (*p != '"') break;
        k = ++p;
        while (*p && *p != '"') { if (*p == '\\' && p[1]) p++; p++; kl++; }
        if (!*p) break;
        kl = (int)(p - k);
        p++;
        while (*p && *p != '"' && *p != '}' && *p != '{') p++;
        if (*p != '"') break;                    /* a scope or something else: stop */
        v = ++p;
        while (*p && *p != '"') { if (*p == '\\' && p[1]) p++; p++; }
        if (!*p) break;
        p++;
        if (kl == sl) { for (i = 0; i < kl && k[i] == spec[i]; i++) { } if (i == kl) { best_v = v; best_k = 100000; break; } }
        if (kl > best_k && kl < sl && k[kl - 1] == '/') {
            for (i = 0; i < kl && k[i] == spec[i]; i++) { }
            if (i == kl) { best_v = v; best_k = kl; }
        }
    }
    if (!best_v) return 0;
    {
        int n = 0;
        const char *q = best_v;
        while (*q && *q != '"' && n < cap - 1) out[n++] = *q++;
        if (best_k != 100000) { const char *r = spec + best_k; while (*r && n < cap - 1) out[n++] = *r++; }
        out[n] = 0;
    }
    return 1;
}

/* Where an import's address leads, from the module at base. */
static int resolve_spec(const char *base, const char *spec, char *out, int cap)
{
    char mapped[MOD_URL];
    if (spec[0] == '.' || spec[0] == '/' || !lower_cmp_n(spec, "http://", 7) || !lower_cmp_n(spec, "https://", 8))
        return url_resolve(base, spec, out, cap);
    if (!imap_lookup(spec, mapped, sizeof mapped)) return 0;     /* a bare name, not in the map */
    return url_resolve(s_pg->url, mapped, out, cap);
}

/* The place of the module at url: found, or a new one, not yet read. */
static int module_slot(const char *url)
{
    int i;
    for (i = 0; i < s_nmod; i++) if (!lower_cmp(s_mod_url[i], url)) return i;
    if (s_nmod >= MOD_MAX) return -1;
    fmt_str(s_mod_url[s_nmod], MOD_URL, "%s", url);
    s_mod_state[s_nmod] = 0;
    return s_nmod++;
}

/* The module in place me, fetched (or given, for one written in the
 * page), read, its own imports loaded, and run.  0 if it failed. */
static int load_module(int me, const char *given, u32 given_len, int depth)
{
    int i, ndep = 0, dep[64];
    u32 len, ms, k, n, t0 = A->ms();
    const char *url = s_mod_url[me];
    char why[200];
    if (s_mod_state[me]) return s_mod_state[me] > 0;      /* read already (or a circle) */
    s_mod_state[me] = -1;
    if (depth > 24) { s_big++; return 0; }
    if (given) {
        int got = feed_text(given, given_len);
        if (got < 0) { s_failed++; return 0; }
        len = (u32)got;
    } else {
        Url u;
        int got;
        if (!url_parse(url, &u)) return 0;
        if (!script_from_ok(&u, url)) return 0;
        if (s_meter >= PAGE_SCRIPTS) { s_big++; return 0; }
        note_loading(base_name(url));
        if ((got = feed_url(url)) < 0) return 0;
        if (s_meter + (u32)got > PAGE_SCRIPTS) { s_big++; feed_drop(); return 0; }
        len = (u32)got;
    }
    s_meter += len;
    /* read it: the last piece, behind the address */
    ms = LOAD_MS + (len >> 10) * LOAD_PER_KB;
    if (ms > LOAD_MAX) ms = LOAD_MAX;
    for (k = 0; url[k]; k++) { }
    n = feed_last(5 + k + 1);
    s_task[0] = T_MODSCAN;
    put32(s_task + 1, ms);
    for (k = 0; url[k]; k++) s_task[5 + k] = (u8)url[k];
    s_task[5 + k] = 0;
    feed_start();
    if (task(n, ms, why, sizeof why) != A_OK) {
        char t[260];
        s_failed++;
        fmt_str(s_fail, sizeof s_fail, "%s was stopped: %s", base_name(url), why);
        fmt_str(t, sizeof t, "%s: %s", base_name(url), why);
        wire("stopped: ", t);
        return 0;
    }
    /* its imports, a line each in the answer: each given its place first,
     * since loading them uses the answer again */
    {
        const char *line = (const char *)s_ans + 6;
        char spec[MOD_URL], to[MOD_URL];
        while (*line && ndep < 64) {
            int sl = 0;
            while (*line && *line != '\n') { if (sl < MOD_URL - 1) spec[sl++] = *line; line++; }
            spec[sl] = 0;
            if (*line) line++;
            if (!resolve_spec(url, spec, to, sizeof to) || (dep[ndep] = module_slot(to)) < 0) {
                s_failed++;
                fmt_str(s_fail, sizeof s_fail, "%s imports %s, which browzer cannot find", base_name(url), spec);
                wire("module: ", s_fail);
                return 0;
            }
            ndep++;
        }
    }
    for (i = 0; i < ndep; i++)
        if (!load_module(dep[i], 0, 0, depth + 1) && s_mod_state[dep[i]] < 0) { s_failed++; return 0; }
    /* run it, its imports having run (or, in a circle, being run) */
    s_task[0] = T_MODRUN;
    put32(s_task + 1, ms);
    n = 5;
    for (k = 0; url[k]; k++) s_task[n++] = (u8)url[k];
    s_task[n++] = 0;
    put32(s_task + n, (u32)ndep); n += 4;
    for (i = 0; i < ndep; i++) {
        for (k = 0; s_mod_url[dep[i]][k]; k++) s_task[n++] = (u8)s_mod_url[dep[i]][k];
        s_task[n++] = 0;
    }
    if (task(n, ms, why, sizeof why) != A_OK) {
        char t[260];
        s_failed++;
        fmt_str(s_fail, sizeof s_fail, "%s was stopped: %s", base_name(url), why);
        fmt_str(t, sizeof t, "%s, %u KB, %u ms: %s", base_name(url), len >> 10, A->ms() - t0, why);
        wire("stopped: ", t);
        return 0;
    }
    {
        char t[200];
        fmt_str(t, sizeof t, "%s, a module, %u KB, %u ms", base_name(url), len >> 10, A->ms() - t0);
        wire("ran ", t);
    }
    s_ran++;
    s_mod_state[me] = 1;
    return 1;
}

/* The import()s the last task asked for: each loaded, then settled. */
static void drain_imports(void)
{
    int guard = 0;
    while (s_ndyn && s_room && guard++ < 32) {
        char to[MOD_URL], why[120];
        int got = -1, i;
        u32 n, k, ticket = s_dyn[0].ticket;
        why[0] = 0;
        if (!resolve_spec(s_dyn[0].base[0] ? s_dyn[0].base : s_pg->url, s_dyn[0].spec, to, sizeof to))
            fmt_str(why, sizeof why, "browzer cannot find %s", s_dyn[0].spec);
        else if ((got = module_slot(to)) < 0 || !load_module(got, 0, 0, 0)) {
            got = -1;
            fmt_str(why, sizeof why, "%s", s_fail[0] ? s_fail : "the module did not load");
        }
        for (i = 1; i < s_ndyn; i++) s_dyn[i - 1] = s_dyn[i];
        s_ndyn--;
        s_task[0] = T_DYNDONE;
        put32(s_task + 1, ticket);
        n = 5;
        if (got >= 0) for (k = 0; s_mod_url[got][k]; k++) s_task[n++] = (u8)s_mod_url[got][k];
        s_task[n++] = 0;
        for (k = 0; why[k]; k++) s_task[n++] = (u8)why[k];
        s_task[n++] = 0;
        (void)task(n, EVENT_MS, 0, 0);
    }
}

/* Run the scripts not yet run, in page order, each through the gate. */
static void run_scripts(void)
{
    u32 n;
    int guard = 0;
    while ((n = next_script()) && guard++ < 200) {
        const char *src = dom_attr(n, "src"), *type = dom_attr(n, "type");
        g_node[n].flags |= NF_SCRIPT_DONE;
        if (type && !lower_cmp(type, "importmap") && !s_imap[0] && (!src || !src[0])) {   /* the page's import map */
            u32 l = dom_text(n, s_imap, sizeof s_imap);
            if (l >= sizeof s_imap) s_imap[0] = 0;
            continue;
        }
        if (!is_js(n)) continue;
        if (dom_attr(n, "nomodule")) continue;   /* for browsers without modules: not this one */
        s_count++;
        if (!s_on || !s_room) continue;
        if (type && !lower_cmp(type, "module")) {
            char url[MOD_URL];
            if (src && src[0]) {
                int at;
                if (url_resolve(s_pg->url, src, url, sizeof url) && (at = module_slot(url)) >= 0) (void)load_module(at, 0, 0, 0);
            } else if (n < s_page_nodes) {
                u32 len = dom_text(n, s_scratch, sizeof s_scratch);
                if (len < sizeof s_scratch && len <= SCRIPT_MAX) {
                    int at;
                    fmt_str(url, sizeof url, "%s#module-%u", s_pg->url, n);   /* its own address, for its imports */
                    if ((at = module_slot(url)) >= 0) (void)load_module(at, s_scratch, len, 0);
                } else s_big++;
            } else s_textmade++;
            drain_imports();
            if (!s_room) break;
            continue;
        }
        if (s_ran + s_failed >= MAX_SCRIPTS) { s_big++; continue; }
        if (src && src[0]) {
            char url[PAGE_HREF_MAX + 16];
            Url u;
            int st, len;
            if (!url_resolve(s_pg->url, src, url, sizeof url) || !url_parse(url, &u)) continue;
            if (!script_from_ok(&u, url)) continue;
            if (s_meter >= PAGE_SCRIPTS) { s_big++; continue; }
            note_loading(base_name(url));
            (void)st;
            if ((len = feed_url(url)) < 0) continue;
            if (s_meter + (u32)len > PAGE_SCRIPTS) {
                s_big++;
                fmt_str(s_fail, sizeof s_fail, "%s would pass %u KB of scripts for the page", base_name(url), (u32)PAGE_SCRIPTS >> 10);
                feed_drop();
                continue;
            }
            s_meter += (u32)len;
            run_fed((u32)len, base_name(url), n);
            drain_imports();
        } else {
            u32 len;
            if (n >= s_page_nodes) { s_textmade++; continue; }   /* text a script made into a script: no */
            len = dom_text(n, s_fetched, SCRIPT_MAX + 1);
            if (len >= SCRIPT_MAX || s_meter + len > PAGE_SCRIPTS) { s_big++; continue; }
            s_meter += len;
            run_text(s_fetched, len, "a script in the page", n);
            drain_imports();
        }
        if (!s_room && s_on) {
            /* the room was stopped: nothing more on this page */
            break;
        }
    }
}

/* An event for the room: type, at element n with its ancestors. */
static int event_at(u32 n, const char *type, const char *key)
{
    u32 at = 1, path = 0, k, len, cnt = 0;
    char why[200];
    int r;
    if (!s_room) return 0;
    s_task[0] = T_CALL;
    put32(s_task + at, EVENT_MS); at += 4;
    for (len = 0; type[len]; len++) { }
    put32(s_task + at, len); at += 4;
    for (k = 0; k < len; k++) s_task[at++] = (u8)type[k];
    put32(s_task + at, n); at += 4;
    path = at; at += 4;
    for (k = n; k && cnt < 200; k = g_node[k].parent) { put32(s_task + at, k); at += 4; cnt++; }
    put32(s_task + path, cnt);
    for (len = 0; key && key[len]; len++) { }
    put32(s_task + at, len); at += 4;
    for (k = 0; k < len; k++) s_task[at++] = (u8)key[k];
    r = task(at, EVENT_MS, why, sizeof why);
    if (r != A_OK) {
        fmt_str(s_fail, sizeof s_fail, "A %s handler was stopped: %s", type, why);
        wire("event: ", s_fail);
        return 0;
    }
    return prevented();
}

/* ---- the page's styles ------------------------------------------------------------- */
static void styles(void)
{
    u32 n, total = 0;
    int sheets = 0;
    css_reset();
    for (n = 2; n < g_nodes; n++) {
        const char *t;
        if (g_node[n].type != N_ELEM || !g_node[n].parent) continue;
        t = dom_tag(n);
        if (!lower_cmp(t, "style")) {
            u32 len = dom_text(n, s_scratch, sizeof s_scratch);
            if (total + len <= CSS_PAGE) { css_add(s_scratch, len); total += len; }
        } else if (!lower_cmp(t, "link")) {
            const char *rel = dom_attr(n, "rel"), *href = dom_attr(n, "href"), *media = dom_attr(n, "media");
            char url[PAGE_HREF_MAX + 16];
            Url u;
            int st, len;
            if (!rel || !href || lower_cmp(rel, "stylesheet") || sheets >= 4 || total >= CSS_PAGE) continue;
            if (media && (lower_has(media, "print") || lower_has(media, "max-width"))) continue;
            if (!url_resolve(s_pg->url, href, url, sizeof url) || !url_parse(url, &u) || !same_site(u.host, s_host)) continue;
            note_loading("the page's styles");
            len = web_get(url, "GET", 0, 0, 0, s_fetched, CSS_MAX, &st, 0, 0);
            if (len <= 0 || st != 200) continue;
            if (total + (u32)len > CSS_PAGE) len = (int)(CSS_PAGE - total);
            css_add(s_fetched, (u32)len);
            total += (u32)len;
            sheets++;
        }
    }
}

/* The fields as the reader typed them, into the tree, before a script
 * reads them or the page is drawn again. */
static void fields_to_tree(void)
{
    int i;
    if (!s_pg) return;
    for (i = 0; i < s_pg->nfields; i++) {
        PageField *f = &s_pg->fields[i];
        u32 n = f->node;
        if (!n || n >= g_nodes) continue;
        if (f->kind == FK_CHECK || f->kind == FK_RADIO) {
            if (f->on) dom_set_attr(n, "checked", "", 0); else dom_del_attr(n, "checked");
        } else if (f->kind == FK_TEXT || f->kind == FK_PASSWORD || f->kind == FK_AREA || f->kind == FK_HIDDEN) {
            u32 at = g_pool_used, k;
            if (g_pool_used + f->vlen + 1 > DOM_POOL) continue;
            for (k = 0; k < f->vlen; k++) g_pool[g_pool_used++] = s_pg->pool[f->val + k];
            g_pool[g_pool_used++] = 0;
            g_node[n].value = at;
            g_node[n].vlen = f->vlen;
            g_node[n].flags |= NF_VALUE;
        } else if (f->kind == FK_SELECT && f->nopt) {
            /* the picked option: the n-th option child */
            u32 c, k = 0;
            for (c = g_node[n].first; c; c = g_node[c].next) {
                if (lower_cmp(dom_tag(c), "option")) continue;
                if (k == f->sel) dom_set_attr(c, "selected", "", 0); else dom_del_attr(c, "selected");
                k++;
            }
        }
    }
}

/* For the test bench only (the emulator's own address for the PC that
 * runs it): the lines the page shows, down the serial wire, so a test can
 * read what styles and scripts made of it. */
static void wire_shown(void)
{
    int l, x;
    char line[200];
    if (lower_cmp(s_host, "10.0.2.2") || !s_pg) return;
    for (l = 0; l < s_pg->lines && l < 40; l++) {
        int n = 0, any = 0;
        for (x = 0; x < s_pg->width && n < 190; x++) {
            int c = s_pg->cells[l * s_pg->width + x].ch;
            line[n++] = (char)(c >= 32 && c < 127 ? c : '?');
            if (c != ' ') any = 1;
        }
        while (n && line[n - 1] == ' ') n--;
        line[n] = 0;
        if (any) { char t[220]; fmt_str(t, sizeof t, "[shown] %s\n", line); A->wire(t); }
    }
    A->wire("[shown] --\n");
}

static void redraw(void)
{
    css_apply();
    web_rerender(s_pg);
    s_last_draw = ms_now();
    g_dom_dirty = 0;
    wire_shown();
}

/* ---- what browzer tells us ---------------------------------------------------------- */
static void set_note(void)
{
    s_note[0] = 0;
    if (!s_count) return;
    if (!s_on) {
        fmt_str(s_note, sizeof s_note, "%u script%s, none of them run. \1j\1 runs this site's.", (u32)s_count, s_count == 1 ? "" : "s");
        return;
    }
    {
        char extra[120] = "";
        if (s_other) fmt_str(extra, sizeof extra, ", %u from another site without https not fetched", (u32)s_other);
        if (s_big) { int k = 0; while (extra[k]) k++; fmt_str(extra + k, sizeof extra - (u32)k, ", %u too big or too many", (u32)s_big); }
        fmt_str(s_note, sizeof s_note, "Scripts: %u ran%s.%s%s", (u32)s_ran, extra, s_fail[0] ? " " : "", s_fail);
    }
}

void script_page(Page *pg, const Url *u)
{
    int k;
    s_pg = pg;
    for (k = 0; u->host[k] && k < 63; k++) s_host[k] = u->host[k];
    s_host[k] = 0;
    s_on = script_site_on(s_host);
    s_count = s_ran = s_other = s_big = s_failed = s_textmade = 0;
    s_meter = 0; s_fetches = 0;
    s_nmod = 0; s_ndyn = 0; s_imap[0] = 0;
    s_fail[0] = 0;
    s_go[0] = 0;
    s_next_timer = 0;
    g_dom_dirty = 0;
    if (g_dom_full) { wire("the page was too big to build: drawn as it came", ""); return; }
    s_page_nodes = g_nodes;
    styles();
    s_loading = 1;
    if (s_on) {
        if (s_room || room_start()) {
            s_task[0] = T_CLEAR;               /* a new page: a fresh engine */
            for (k = 1; k < 9; k++) s_task[k] = 0;
            if (task(9, 3000, s_fail, sizeof s_fail) != A_OK) s_room = 0;
        }
        if (!s_room) wire("not run: ", s_fail);
    }
    run_scripts();
    if (s_on && s_room) {
        event_at(1, "DOMContentLoaded", 0);
        event_at(0, "load", 0);
        run_scripts();                       /* what the handlers added */
    }
    s_loading = 0;
    if (!s_on) {
        /* nothing will run: count every script in the page */
    }
    if (s_textmade) wire("refused: script text made by a script", "");
    redraw();
    set_note();
    if (s_note[0]) {
        int i;
        for (i = 0; s_note[i] && i < (int)sizeof pg->note - 1; i++) pg->note[i] = s_note[i];
        pg->note[i] = 0;
        pg->note_col = C_DIM;
        wire(s_note, "");
    }
}

/* From the main loop: timers that are due, a redraw if scripts changed
 * the page, and a place a script asked to go. */
void script_tick(Page *pg)
{
    char why[200];
    if (pg != s_pg || !s_on || !s_room) return;
    if (s_next_timer && (s32)(ms_now() - s_next_timer) >= 0) {
        s_task[0] = T_TIMERS;
        put32(s_task + 1, EVENT_MS);
        fields_to_tree();
        if (task(5, EVENT_MS + 500, why, sizeof why) < 0) {
            fmt_str(s_fail, sizeof s_fail, "a timer was stopped: %s", why);
            wire(s_fail, "");
            set_note();
        }
        run_scripts();
    }
    if (g_dom_dirty && ms_now() - s_last_draw > 500) redraw();
}

int script_click(Page *pg, int link)
{
    u32 n;
    if (pg != s_pg || !s_on || !s_room || !pg->link_node || link <= 0 || link > PAGE_LINKS) return 0;
    n = pg->link_node[link];
    if (!n || n >= g_nodes) return 0;
    fields_to_tree();
    if (event_at(n, "click", 0) || (pg->href[link - 1][0] == 'f' && pg->href[link - 1][1] == 'l')) {
        run_scripts();
        if (g_dom_dirty) redraw();
        return 1;
    }
    run_scripts();
    if (g_dom_dirty) redraw();
    return 0;
}

int script_submit(Page *pg, int form)
{
    u32 n;
    int stop;
    if (pg != s_pg || !s_on || !s_room || form < 0 || form >= pg->nforms) return 0;
    n = pg->forms[form].node;
    if (!n || n >= g_nodes) return 0;
    fields_to_tree();
    stop = event_at(n, "submit", 0);
    run_scripts();
    if (g_dom_dirty) redraw();
    return stop;
}

void script_changed(Page *pg, int field)
{
    u32 n;
    if (pg != s_pg || !s_on || !s_room || field < 0 || field >= pg->nfields) return;
    n = pg->fields[field].node;
    if (!n || n >= g_nodes) return;
    fields_to_tree();
    event_at(n, "input", 0);
    event_at(n, "change", 0);
    run_scripts();
    if (g_dom_dirty) redraw();
}

/* Where a script asked to go, once: browzer goes there. */
const char *script_go(void)
{
    static char go[PAGE_HREF_MAX];
    int i;
    if (!s_go[0]) return 0;
    for (i = 0; s_go[i] && i < PAGE_HREF_MAX - 1; i++) go[i] = s_go[i];
    go[i] = 0;
    s_go[0] = 0;
    return go;
}

int script_state(void) { return !s_count ? 0 : s_on ? 2 : 1; }

void script_site_toggle(Page *pg)
{
    const char *site;
    int at;
    if (!pg->url[0]) return;
    {
        Url u;
        if (!url_parse(pg->url, &u)) return;
        site = site_of(u.host);
        sites_load();
        if (sites_has(site, &at)) {
            int k = at;
            while (k < (int)sizeof s_sites && s_sites[k] != '\n') s_sites[k++] = ' ';
            if (k < (int)sizeof s_sites) s_sites[k] = ' ';
            wire("scripts off for ", site);
        } else {
            /* the first stretch of spaces long enough */
            int need = 0, i, run = 0, start = -1;
            while (site[need]) need++;
            for (i = 0; i < (int)sizeof s_sites; i++) {
                if (s_sites[i] == ' ' || s_sites[i] == 0) { if (!run++) start = i; if (run > need + 1) break; }
                else run = 0;
            }
            if (run > need + 1 && start >= 0) {
                if (start > 0 && s_sites[start - 1] != '\n') start++;
                for (i = 0; i < need; i++) s_sites[start + i] = site[i];
                s_sites[start + need] = '\n';
            }
            wire("scripts on for ", site);
        }
        if (s_sites_ok) A->file_write(&s_sites_file, (const u8 *)s_sites);
    }
}

/* ---- the door: what the room asks ------------------------------------------------------ */
static u32 s_ri, s_rl;
static u8 *s_out;
static u32 s_on_, s_cap;

static u32 g32(void)
{
    u32 v;
    if (s_ri + 4 > s_rl) return 0;
    v = (u32)s_req[s_ri] | ((u32)s_req[s_ri + 1] << 8) | ((u32)s_req[s_ri + 2] << 16) | ((u32)s_req[s_ri + 3] << 24);
    s_ri += 4;
    return v;
}

/* A text from the request, as a zero-ended string in place (the request
 * is a copy, so it can be written into: the byte after a text is its
 * next field's, already read or not needed). */
static char *gstr(u32 *len)
{
    static char texts[4][4096];
    static int which;
    u32 n = g32(), k;
    char *t = texts[which++ & 3];
    if (s_ri + n > s_rl) n = s_rl - s_ri;
    if (len) *len = n;
    if (n >= 4096) {
        /* long text (innerHTML, a fetch's body): straight from the request */
        static char big[65536];
        for (k = 0; k < n && k < sizeof big - 1; k++) big[k] = (char)s_req[s_ri + k];
        big[k] = 0;
        s_ri += n;
        return big;
    }
    for (k = 0; k < n; k++) t[k] = (char)s_req[s_ri + k];
    t[n] = 0;
    s_ri += n;
    return t;
}

static int put(u8 v) { if (s_on_ < s_cap) { s_out[s_on_++] = v; return 1; } return 0; }
static void put4(u32 v) { put((u8)v); put((u8)(v >> 8)); put((u8)(v >> 16)); put((u8)(v >> 24)); }
static int ans(int kind) { s_on_ = 0; put((u8)kind); return 1; }
static int ans_str(const char *s, u32 n) { u32 k; ans(DK_STR); put4(n); for (k = 0; k < n; k++) put((u8)s[k]); return (int)s_on_; }
static int ans_cstr(const char *s) { u32 n = 0; while (s && s[n]) n++; return ans_str(s ? s : "", n); }
static int ans_node(u32 n) { if (!n) { ans(DK_NULL); return (int)s_on_; } ans(DK_NODE); put4(n); return (int)s_on_; }
static int ans_bool(int b) { ans(DK_BOOL); put((u8)(b ? 1 : 0)); return (int)s_on_; }
static int ans_num(s32 v) { ans(DK_NUM); put4((u32)v); return (int)s_on_; }
static int ans_undef(void) { ans(DK_UNDEF); return (int)s_on_; }
static int refuse(const char *why)
{
    u32 n = 0, k;
    wire("refused: ", why);
    while (why[n]) n++;
    s_on_ = 0;
    put(DK_ERR);
    put4(n);
    for (k = 0; k < n; k++) put((u8)why[k]);
    return (int)s_on_;
}

static int ok_node(u32 n) { return n >= 1 && n < g_nodes; }
static int is_el(u32 n) { return ok_node(n) && g_node[n].type == N_ELEM; }

static int ans_list_start(void) { ans(DK_LIST); put4(0); return 0; }
static void list_add(u32 *count, u32 n) { put4(n); (*count)++; }
static int list_end(u32 count) { s_out[1] = (u8)count; s_out[2] = (u8)(count >> 8); s_out[3] = (u8)(count >> 16); s_out[4] = (u8)(count >> 24); return (int)s_on_; }

static void upper_into(const char *t, char *o) { int k; for (k = 0; t[k] && k < 31; k++) o[k] = (char)(t[k] >= 'a' && t[k] <= 'z' ? t[k] - 32 : t[k]); o[k] = 0; }

static u32 first_el(u32 n) { u32 c; for (c = g_node[n].first; c && g_node[c].type != N_ELEM; c = g_node[c].next) { } return c; }
static u32 last_el(u32 n) { u32 c; for (c = g_node[n].last; c && g_node[c].type != N_ELEM; c = g_node[c].prev) { } return c; }
static u32 next_el(u32 n) { u32 c; for (c = g_node[n].next; c && g_node[c].type != N_ELEM; c = g_node[c].next) { } return c; }
static u32 prev_el(u32 n) { u32 c; for (c = g_node[n].prev; c && g_node[c].type != N_ELEM; c = g_node[c].prev) { } return c; }

static int attr_bool(u32 n, const char *name) { return dom_attr(n, name) != 0; }

static int resolved(u32 n, const char *name)
{
    const char *v = dom_attr(n, name);
    char url[PAGE_HREF_MAX + 16];
    if (!v) return ans_cstr("");
    if (!url_resolve(s_pg->url, v, url, sizeof url)) return ans_cstr(v);
    return ans_cstr(url);
}

/* Another site?  For a form's action, a fetch, a script's navigation. */
/* A host on the local network, or this machine: no public page's script
 * may reach one (a router's pages, say). */
static int private_host(const char *h)
{
    u32 a[4] = { 0, 0, 0, 0 }, n = 0, i, dots = 0, len = 0;
    const char *p;
    for (p = h; *p; p++) { if (*p == '.') dots++; len++; }
    if (!dots || !lower_cmp(h, "localhost")) return 1;
    {
        static const char *const tails[] = { ".local", ".localhost", ".lan", ".home", ".internal", ".intranet", ".arpa", 0 };
        for (i = 0; tails[i]; i++) {
            u32 tl = 0;
            while (tails[i][tl]) tl++;
            if (len > tl && !lower_cmp(h + len - tl, tails[i])) return 1;
        }
    }
    for (p = h; *p; p++) {                   /* an address: four numbers, all of it */
        if (*p >= '0' && *p <= '9') { if (n < 4) a[n] = a[n] * 10 + (u32)(*p - '0'); }
        else if (*p == '.') n++;
        else return 0;                       /* a name, not an address */
    }
    if (n != 3) return 0;
    return a[0] == 10 || a[0] == 127 || a[0] == 0 || (a[0] == 169 && a[1] == 254) ||
           (a[0] == 172 && a[1] >= 16 && a[1] <= 31) || (a[0] == 192 && a[1] == 168) || a[0] >= 224;
}

/* Does the other site's answer let this page's script read it? */
static int cors_allows(void)
{
    const char *a = s_acao;
    while (*a == ' ') a++;
    return (a[0] == '*' && !a[1]) || !lower_cmp(a, s_origin);
}

static int elsewhere(const char *where)
{
    char url[PAGE_HREF_MAX + 16];
    Url u;
    if (!where[0]) return 0;
    if (!url_resolve(s_pg->url, where, url, sizeof url) || !url_parse(url, &u)) return 0;
    return !same_site(u.host, s_host);
}

static int get_prop(u32 n, u32 p)
{
    char buf[32];
    u32 c, cnt = 0;
    if (!ok_node(n)) return ans_undef();
    if (n == 1) {                            /* the document itself         */
        switch (p) {
        case DP_TITLE: return ans_cstr(s_pg->title);
        case DP_BODY: return ans_node(dom_find("body"));
        case DP_HEAD: return ans_node(dom_find("head"));
        case DP_ROOT: case DP_FIRSTEL: case DP_FIRST: return ans_node(dom_root());
        case DP_NODETYPE: return ans_num(9);
        case DP_CHILDREN: case DP_NODES: ans_list_start(); list_add(&cnt, dom_root()); return list_end(cnt);
        case DP_PARENT: return ans_node(0);
        case DP_TEXT: return ans(DK_NULL), (int)s_on_;
        }
    }
    if (g_node[n].type == N_TEXT) {
        switch (p) {
        case DP_DATA: case DP_TEXT: return ans_str(g_pool + g_node[n].name, g_node[n].len);
        case DP_NODETYPE: return ans_num(3);
        case DP_TAG: return ans_cstr("#text");
        case DP_PARENT: return ans_node(g_node[n].parent);
        case DP_NEXT: return ans_node(g_node[n].next);
        case DP_PREV: return ans_node(g_node[n].prev);
        case DP_NEXTEL: return ans_node(next_el(n));
        case DP_PREVEL: return ans_node(prev_el(n));
        }
        return ans(DK_NULL), (int)s_on_;
    }
    switch (p) {
    case DP_TAG: upper_into(dom_tag(n), buf); return ans_cstr(buf);
    case DP_ID: return ans_cstr(dom_attr(n, "id"));
    case DP_CLASS: return ans_cstr(dom_attr(n, "class"));
    case DP_NAME: return ans_cstr(dom_attr(n, "name"));
    case DP_TYPE: { const char *t = dom_attr(n, "type"); if (!t && !lower_cmp(dom_tag(n), "button")) t = "submit"; if (!t && !lower_cmp(dom_tag(n), "input")) t = "text"; return ans_cstr(t); }
    case DP_PLACEHOLDER: return ans_cstr(dom_attr(n, "placeholder"));
    case DP_ALT: return ans_cstr(dom_attr(n, "alt"));
    case DP_LABEL: return ans_cstr(dom_attr(n, "label"));
    case DP_TITLE: return ans_cstr(dom_attr(n, "title"));
    case DP_METHOD: { const char *m = dom_attr(n, "method"); return ans_cstr(m ? m : "get"); }
    case DP_TEXT: { u32 len = dom_text(n, s_scratch, sizeof s_scratch); return ans_str(s_scratch, len); }
    case DP_HTML: { u32 len = dom_html(n, s_scratch, sizeof s_scratch, 0); return ans_str(s_scratch, len); }
    case DP_OUTER: { u32 len = dom_html(n, s_scratch, sizeof s_scratch, 1); return ans_str(s_scratch, len); }
    case DP_VALUE: { int len; fields_to_tree(); len = dom_value(n, s_scratch, sizeof s_scratch); return ans_str(s_scratch, (u32)len); }
    case DP_CHECKED: fields_to_tree(); return ans_bool(attr_bool(n, "checked"));
    case DP_DISABLED: return ans_bool(attr_bool(n, "disabled"));
    case DP_SELECTED: return ans_bool(attr_bool(n, "selected"));
    case DP_HIDDEN: return ans_bool(attr_bool(n, "hidden"));
    case DP_READONLY: return ans_bool(attr_bool(n, "readonly"));
    case DP_SELINDEX: {
        s32 i = 0, at = -1;
        fields_to_tree();
        for (c = g_node[n].first; c; c = g_node[c].next) {
            if (lower_cmp(dom_tag(c), "option")) continue;
            if (at < 0) at = 0;
            if (attr_bool(c, "selected")) { at = i; break; }
            i++;
        }
        return ans_num(at);
    }
    case DP_HREF: return resolved(n, "href");
    case DP_SRC: return resolved(n, "src");
    case DP_ACTION: { const char *a = dom_attr(n, "action"); if (!a || !a[0]) return ans_cstr(s_pg->url); return resolved(n, "action"); }
    case DP_PARENT: return ans_node(g_node[n].parent);
    case DP_FIRST: return ans_node(g_node[n].first);
    case DP_LAST: return ans_node(g_node[n].last);
    case DP_NEXT: return ans_node(g_node[n].next);
    case DP_PREV: return ans_node(g_node[n].prev);
    case DP_FIRSTEL: return ans_node(first_el(n));
    case DP_LASTEL: return ans_node(last_el(n));
    case DP_NEXTEL: return ans_node(next_el(n));
    case DP_PREVEL: return ans_node(prev_el(n));
    case DP_NODETYPE: return ans_num(1);
    case DP_COUNTEL: { s32 k = 0; for (c = first_el(n); c; c = next_el(c)) k++; return ans_num(k); }
    case DP_CHILDREN: case DP_OPTIONS:
        ans_list_start();
        for (c = g_node[n].first; c; c = g_node[c].next)
            if (g_node[c].type == N_ELEM && (p == DP_CHILDREN || !lower_cmp(dom_tag(c), "option"))) list_add(&cnt, c);
        return list_end(cnt);
    case DP_NODES:
        ans_list_start();
        for (c = g_node[n].first; c; c = g_node[c].next) list_add(&cnt, c);
        return list_end(cnt);
    case DP_FORM: { for (c = g_node[n].parent; c > 1; c = g_node[c].parent) if (!lower_cmp(dom_tag(c), "form")) return ans_node(c); return ans_node(0); }
    case DP_DATA: return ans(DK_NULL), (int)s_on_;
    }
    return ans_undef();
}

static void set_value(u32 n, const char *v, u32 len)
{
    u32 at = g_pool_used, k;
    int i;
    if (g_pool_used + len + 1 > DOM_POOL) return;
    for (k = 0; k < len; k++) g_pool[g_pool_used++] = v[k];
    g_pool[g_pool_used++] = 0;
    g_node[n].value = at; g_node[n].vlen = len; g_node[n].flags |= NF_VALUE;
    /* and in the box on the page, so the reader sees it now */
    for (i = 0; s_pg && i < s_pg->nfields; i++) {
        PageField *f = &s_pg->fields[i];
        if (f->node != n) continue;
        if (len > f->vcap) len = f->vcap;
        for (k = 0; k < len; k++) s_pg->pool[f->val + k] = v[k];
        f->vlen = len;
        page_field_show(s_pg, i);
    }
}

static void set_flag_attr(u32 n, const char *name, const char *v)
{
    int i;
    if (v[0]) dom_set_attr(n, name, "", 0); else dom_del_attr(n, name);
    if (!lower_cmp(name, "checked"))
        for (i = 0; s_pg && i < s_pg->nfields; i++)
            if (s_pg->fields[i].node == n) { s_pg->fields[i].on = (u8)(v[0] != 0); page_field_show(s_pg, i); }
}

static int replace_outer(u32 n, const char *html, u32 len)
{
    u32 tmp = dom_create("fl-frag"), c, parent = g_node[n].parent;
    if (!tmp || !parent) return 0;
    dom_set_html(tmp, html, len);
    while ((c = g_node[tmp].first)) dom_append(parent, c, n);
    dom_remove(n);
    return 1;
}

static int set_prop(u32 n, u32 p, const char *v, u32 len)
{
    if (!ok_node(n)) return ans_undef();
    g_dom_dirty = 1;
    if (n == 1) {
        if (p == DP_TITLE) {
            int i;
            for (i = 0; v[i] && i < 59; i++) s_pg->title[i] = v[i];
            s_pg->title[i] = 0;
        }
        return ans_undef();
    }
    if (g_node[n].type == N_TEXT) { if (p == DP_DATA || p == DP_TEXT) dom_set_text(n, v, len); return ans_undef(); }
    switch (p) {
    case DP_ID: dom_set_attr(n, "id", v, len); break;
    case DP_CLASS: dom_set_attr(n, "class", v, len); break;
    case DP_NAME: dom_set_attr(n, "name", v, len); break;
    case DP_TYPE: dom_set_attr(n, "type", v, len); break;
    case DP_TITLE: dom_set_attr(n, "title", v, len); break;
    case DP_PLACEHOLDER: dom_set_attr(n, "placeholder", v, len); break;
    case DP_ALT: dom_set_attr(n, "alt", v, len); break;
    case DP_LABEL: dom_set_attr(n, "label", v, len); break;
    case DP_TEXT: dom_set_text(n, v, len); break;
    case DP_HTML: dom_set_html(n, v, len); break;
    case DP_OUTER: replace_outer(n, v, len); break;
    case DP_VALUE: set_value(n, v, len); break;
    case DP_CHECKED: set_flag_attr(n, "checked", v); break;
    case DP_DISABLED: set_flag_attr(n, "disabled", v); break;
    case DP_SELECTED: set_flag_attr(n, "selected", v); break;
    case DP_HIDDEN: set_flag_attr(n, "hidden", v); break;
    case DP_READONLY: set_flag_attr(n, "readonly", v); break;
    case DP_SELINDEX: {
        u32 c, i = 0, want = 0;
        while (*v >= '0' && *v <= '9') want = want * 10 + (u32)(*v++ - '0');
        for (c = g_node[n].first; c; c = g_node[c].next) {
            if (lower_cmp(dom_tag(c), "option")) continue;
            if (i == want) dom_set_attr(c, "selected", "", 0); else dom_del_attr(c, "selected");
            i++;
        }
        break;
    }
    case DP_HREF: dom_set_attr(n, "href", v, len); break;
    case DP_SRC: dom_set_attr(n, "src", v, len); break;
    case DP_METHOD: dom_set_attr(n, "method", v, len); break;
    case DP_ACTION:
        dom_set_attr(n, "action", v, len);
        break;
    }
    return ans_undef();
}

/* el.style.x: in the element's style="..." */
static int style_get(u32 n, const char *name)
{
    const char *s = dom_attr(n, "style");
    u32 nl = 0, i = 0;
    while (name[nl]) nl++;
    if (!s) return ans_cstr("");
    while (s[i]) {
        u32 st, vs, ve;
        while (s[i] == ' ' || s[i] == ';' || s[i] == '\n') i++;
        st = i;
        while (s[i] && s[i] != ':' && s[i] != ';') i++;
        if (s[i] != ':') continue;
        {
            u32 ne = i;
            while (ne > st && s[ne - 1] == ' ') ne--;
            i++;
            while (s[i] == ' ') i++;
            vs = i;
            while (s[i] && s[i] != ';') i++;
            ve = i;
            while (ve > vs && s[ve - 1] == ' ') ve--;
            if (ne - st == nl) {
                u32 k;
                for (k = 0; k < nl && (s[st + k] | 32) == (name[k] | 32); k++) { }
                if (k == nl) return ans_str(s + vs, ve - vs);
            }
        }
    }
    return ans_cstr("");
}

static int style_set(u32 n, const char *name, const char *val)
{
    const char *s = dom_attr(n, "style");
    char out[2048];
    u32 o = 0, nl = 0, i = 0, k;
    g_dom_dirty = 1;
    if (!lower_cmp(name, "css-text")) { for (k = 0; val[k]; k++) { } dom_set_attr(n, "style", val, k); return ans_undef(); }
    while (name[nl]) nl++;
    if (s) while (s[i] && o < sizeof out - 4) {   /* the others, kept */
        u32 st = i, e;
        while (s[i] && s[i] != ';') i++;
        e = i;
        if (s[i]) i++;
        {
            u32 a = st, ne;
            while (a < e && s[a] == ' ') a++;
            ne = a;
            while (ne < e && s[ne] != ':') ne++;
            {
                u32 nn = ne;
                while (nn > a && s[nn - 1] == ' ') nn--;
                if (nn - a == nl) {
                    for (k = 0; k < nl && (s[a + k] | 32) == (name[k] | 32); k++) { }
                    if (k == nl) continue;      /* the one being set: dropped */
                }
            }
            if (e > a) { for (k = a; k < e && o < sizeof out - 4; k++) out[o++] = s[k]; out[o++] = ';'; }
        }
    }
    if (val[0]) {
        for (k = 0; name[k] && o < sizeof out - 4; k++) out[o++] = name[k];
        out[o++] = ':';
        for (k = 0; val[k] && o < sizeof out - 2; k++) out[o++] = val[k];
        out[o++] = ';';
    }
    dom_set_attr(n, "style", out, o);
    return ans_undef();
}

static int class_op(u32 n, u32 op, const char *name)
{
    const char *c = dom_attr(n, "class");
    char out[1024];
    u32 o = 0, nl = 0, i = 0, k;
    int had = 0;
    while (name[nl]) nl++;
    if (!nl) return ans_bool(0);
    had = dom_has_class(n, name, nl);
    if (op == 3) return ans_bool(had);
    if ((op == 0 && had) || (op == 1 && !had)) return ans_bool(had);
    g_dom_dirty = 1;
    if (c) while (c[i] && o < sizeof out - nl - 4) {
        u32 st;
        while (c[i] == ' ' || c[i] == '\t' || c[i] == '\n') i++;
        st = i;
        while (c[i] && c[i] != ' ' && c[i] != '\t' && c[i] != '\n') i++;
        if (i == st) continue;
        if (i - st == nl) { for (k = 0; k < nl && c[st + k] == name[k]; k++) { } if (k == nl) continue; }
        if (o) out[o++] = ' ';
        for (k = st; k < i; k++) out[o++] = c[k];
    }
    if (op == 0 || (op == 2 && !had)) {
        if (o) out[o++] = ' ';
        for (k = 0; k < nl; k++) out[o++] = name[k];
    }
    dom_set_attr(n, "class", out, o);
    return ans_bool(op == 2 ? !had : op == 0);
}

static void subtree(u32 from, int (*want)(u32, const char *), const char *arg, u32 *cnt)
{
    u32 n = g_node[from].first;
    int guard = 0;
    while (n && guard++ < DOM_NODES * 2) {
        if (g_node[n].type == N_ELEM && want(n, arg)) { if (*cnt < 4000) list_add(cnt, n); }
        if (g_node[n].first) n = g_node[n].first;
        else {
            while (n && n != from && !g_node[n].next) n = g_node[n].parent;
            if (!n || n == from) break;
            n = g_node[n].next;
        }
    }
}
static int want_tag(u32 n, const char *t) { return t[0] == '*' || !lower_cmp(dom_tag(n), t); }
static int want_classes(u32 n, const char *t)
{
    u32 i = 0;
    while (t[i]) {
        u32 st;
        while (t[i] == ' ') i++;
        st = i;
        while (t[i] && t[i] != ' ') i++;
        if (i > st && !dom_has_class(n, t + st, i - st)) return 0;
    }
    return 1;
}
static int want_name(u32 n, const char *t) { const char *v = dom_attr(n, "name"); return v && !lower_cmp(v, t); }

static int insert_node(u32 parent, u32 child, u32 before)
{
    g_dom_dirty = 1;
    if (!is_el(parent) && parent != 1) return 0;
    if (!ok_node(child)) return 0;
    if (before && (!ok_node(before) || g_node[before].parent != parent)) before = 0;
    if (g_node[child].type == N_ELEM && !lower_cmp(dom_tag(child), "fl-frag")) {
        u32 c;                               /* a fragment: its children go in */
        while ((c = g_node[child].first)) dom_append(parent, c, before);
        return 1;
    }
    return dom_append(parent, child, before);
}

static int door(u8 *buf, u32 len, u32 cap)
{
    u32 op, n, k;
    char *a, *b;
    u32 al, bl;
    if (len > sizeof s_req) return -1;
    for (k = 0; k < len; k++) s_req[k] = buf[k];
    s_rl = len; s_ri = 1;
    s_out = buf; s_cap = cap; s_on_ = 0;
    op = s_req[0];
    switch (op) {
    case D_LOG: {
        u32 level = g32();
        a = gstr(&al);
        { char line[300]; fmt_str(line, sizeof line, "[console%s] %s\n", level == 2 ? " error" : level == 1 ? " warn" : "", a); A->wire(line); }
        return ans_undef();
    }
    case D_ALERT:
        a = gstr(&al);
        wire("alert: ", a);
        if (s_pg) { int i; for (i = 0; a[i] && i < (int)sizeof s_pg->note - 1; i++) s_pg->note[i] = a[i]; s_pg->note[i] = 0; s_pg->note_col = C_ACCENT; }
        return ans_undef();
    case D_GETID: a = gstr(&al); return ans_node(dom_by_id(a));
    case D_QUERY: {
        static u16 hits[4000];
        u32 all, from;
        int found, i;
        from = g32(); a = gstr(&al); all = g32();
        if (!ok_node(from)) return ans_undef();
        found = dom_query(from, a, al, hits, all ? 4000 : 1);
        if (found < 0) return ans_undef();
        if (!all) return ans_node(found ? hits[0] : 0);
        ans_list_start();
        for (i = 0; i < found; i++) put4(hits[i]);
        return list_end((u32)found);
    }
    case D_TAGS: case D_CLASSES: {
        u32 cnt = 0;
        n = g32(); a = gstr(&al);
        if (!ok_node(n)) return ans_undef();
        ans_list_start();
        subtree(n, op == D_TAGS ? want_tag : want_classes, a, &cnt);
        return list_end(cnt);
    }
    case D_NAMES: { u32 cnt = 0; a = gstr(&al); ans_list_start(); subtree(1, want_name, a, &cnt); return list_end(cnt); }
    case D_GET: n = g32(); k = g32(); return get_prop(n, k);
    case D_SET: n = g32(); k = g32(); a = gstr(&al); return set_prop(n, k, a, al);
    case D_ATTR: {
        const char *v;
        n = g32(); a = gstr(&al);
        if (!is_el(n)) return ans(DK_NULL), (int)s_on_;
        /* an onclick written by a script is not run: text made into code */
        if ((a[0] | 32) == 'o' && (a[1] | 32) == 'n' && n >= s_page_nodes) return ans(DK_NULL), (int)s_on_;
        { char low[48]; int i; for (i = 0; a[i] && i < 47; i++) low[i] = (char)(a[i] >= 'A' && a[i] <= 'Z' ? a[i] + 32 : a[i]); low[i] = 0; v = dom_attr(n, low); }
        if (!v) return ans(DK_NULL), (int)s_on_;
        return ans_cstr(v);
    }
    case D_SETATTR: {
        n = g32(); a = gstr(&al); b = gstr(&bl);
        if (!is_el(n)) return ans_undef();
        if ((a[0] | 32) == 'o' && (a[1] | 32) == 'n') return ans_undef();   /* code from text: ignored */
        dom_set_attr(n, a, b, bl);
        g_dom_dirty = 1;
        return ans_undef();
    }
    case D_DELATTR: n = g32(); a = gstr(&al); if (is_el(n)) { dom_del_attr(n, a); g_dom_dirty = 1; } return ans_undef();
    case D_CREATE: a = gstr(&al); return ans_node(dom_create(a[0] ? a : "div"));
    case D_TEXT: a = gstr(&al); return ans_node(dom_text_node(a, al));
    case D_CLONE: n = g32(); k = g32(); return ans_node(ok_node(n) ? dom_clone(n, k != 0) : 0);
    case D_INSERT: { u32 p = g32(), c = g32(), before = g32(); return ans_bool(insert_node(p, c, before)); }
    case D_REMOVE: n = g32(); if (ok_node(n)) { dom_remove(n); g_dom_dirty = 1; } return ans_undef();
    case D_STYLE: n = g32(); a = gstr(&al); return is_el(n) ? style_get(n, a) : ans_cstr("");
    case D_SETSTYLE: n = g32(); a = gstr(&al); b = gstr(&bl); return is_el(n) ? style_set(n, a, b) : ans_undef();
    case D_CLASS: n = g32(); k = g32(); a = gstr(&al); return is_el(n) ? class_op(n, k, a) : ans_bool(0);
    case D_LISTEN:
        n = g32(); a = gstr(&al);
        if (is_el(n)) {
            if (!lower_cmp(a, "click") || !lower_cmp(a, "mousedown") || !lower_cmp(a, "mouseup")) g_node[n].flags |= NF_CLICK;
            else if (!lower_cmp(a, "submit")) g_node[n].flags |= NF_SUBMIT;
            else g_node[n].flags |= NF_CHANGE;
            g_dom_dirty = 1;
        }
        return ans_undef();
    case D_IMPORT: {                         /* import(): queued, loaded after this task */
        a = gstr(&al); b = gstr(&bl);
        if (s_ndyn >= 8 || al >= MOD_URL || bl >= MOD_URL) return ans_num(-1);
        s_dyn[s_ndyn].ticket = ++s_dyn_ticket;
        fmt_str(s_dyn[s_ndyn].spec, MOD_URL, "%s", a);
        fmt_str(s_dyn[s_ndyn].base, MOD_URL, "%s", b);
        s_ndyn++;
        return ans_num((s32)s_dyn_ticket);
    }
    case D_FETCH: {
        char *m, *u, *body, *t, url[PAGE_HREF_MAX + 16], type[64];
        u32 ml, ul, bodyl, tl;
        int st, got;
        m = gstr(&ml); u = gstr(&ul); body = gstr(&bodyl); t = gstr(&tl);
        if (++s_fetches > FETCHES) return refuse("it fetched too often");
        if (!url_resolve(s_pg->url, u, url, sizeof url)) return refuse("it fetched an address browzer cannot read");
        type[0] = 0;
        if (elsewhere(u)) {
            /* another site: only as CORS allows (preflight first when it is
             * not a plain GET, HEAD or POST of a form or text), never into
             * the local network from a page that is not on it */
            Url pu, tu;
            int simple, i;
            if (!url_parse(s_pg->url, &pu) || !url_parse(url, &tu)) return refuse("it fetched an address browzer cannot read");
            if (private_host(tu.host) && !private_host(pu.host)) { got = -1; st = 0; wire("refused: a fetch into the local network: ", url); goto answer; }
            fmt_str(s_origin, sizeof s_origin, "%s://%s", pu.secure ? "https" : "http", pu.host);
            if (!((pu.secure && pu.port == 443) || (!pu.secure && pu.port == 80))) {
                for (i = 0; s_origin[i]; i++) { }
                fmt_str(s_origin + i, sizeof s_origin - (u32)i, ":%u", (u32)pu.port);
            }
            simple = (!m[0] || !lower_cmp(m, "GET") || !lower_cmp(m, "HEAD") || !lower_cmp(m, "POST")) &&
                     (!t[0] || !lower_cmp_n(t, "text/plain", 10) || !lower_cmp_n(t, "application/x-www-form-urlencoded", 33) ||
                      !lower_cmp_n(t, "multipart/form-data", 19));
            s_cross = 1;
            note_loading("what a script asked another site for");
            if (!simple) {
                fmt_str(s_extra, sizeof s_extra, "Access-Control-Request-Method: %s\r\n%s", m[0] ? m : "GET",
                        t[0] ? "Access-Control-Request-Headers: content-type\r\n" : "");
                got = web_get(url, "OPTIONS", 0, 0, 0, s_scratch, sizeof s_scratch - 1, &st, 0, 0);
                if (got < 0 || st < 200 || st >= 300 || !cors_allows() ||
                    (s_acam[0] && s_acam[0] != '*' && !lower_has(s_acam, m[0] ? m : "GET"))) {
                    s_cross = 0; s_extra[0] = 0; got = -1; st = 0;
                    wire("refused by the other site: ", url);
                    goto answer;
                }
            }
            s_extra[0] = 0;
            got = web_get(url, m[0] ? m : "GET", (const u8 *)body, (int)bodyl, t, s_scratch, sizeof s_scratch - 1, &st, type, sizeof type);
            s_cross = 0;
            if (got >= 0 && !cors_allows()) { got = -1; st = 0; type[0] = 0; wire("not allowed by the other site: ", url); }
            goto answer;
        }
        note_loading("what a script asked for");
        got = web_get(url, m[0] ? m : "GET", (const u8 *)body, (int)bodyl, t, s_scratch, sizeof s_scratch - 1, &st, type, sizeof type);
    answer:
        s_on_ = 0;
        put(DK_STR);                          /* status, type, body          */
        put4((u32)(got < 0 ? 0 : st));
        for (k = 0; type[k]; k++) { }
        put4(k);
        { u32 i; for (i = 0; i < k; i++) put((u8)type[i]); }
        if (got < 0) got = 0;
        if ((u32)got + s_on_ + 4 > s_cap) got = (int)(s_cap - s_on_ - 4);
        put4((u32)got);
        { int i; for (i = 0; i < got; i++) put((u8)s_scratch[i]); }
        return (int)s_on_;
    }
    case D_GO: {
        char url[PAGE_HREF_MAX + 16];
        int i;
        a = gstr(&al);
        if (elsewhere(a)) return refuse("it tried to send this page to another site");
        if (!url_resolve(s_pg->url, a, url, sizeof url)) return ans_undef();
        for (i = 0; url[i] && i < PAGE_HREF_MAX - 1; i++) s_go[i] = url[i];
        s_go[i] = 0;
        return ans_undef();
    }
    case D_WRITE: {
        u32 tmp, c, at = s_cur_script, parent;
        a = gstr(&al);
        if (!s_loading || !at || !ok_node(at)) return ans_undef();
        parent = g_node[at].parent;
        tmp = dom_create("fl-frag");
        if (!tmp || !parent) return ans_undef();
        dom_set_html(tmp, a, al);
        while ((c = g_node[tmp].last)) dom_append(parent, c, g_node[at].next);
        g_dom_dirty = 1;
        return ans_undef();
    }
    case D_HREF: return ans_cstr(s_pg->url);
    case D_PATH: {
        u32 cnt = 0, x;
        n = g32();
        if (!ok_node(n)) return ans_undef();
        ans_list_start();
        for (x = n; x && cnt < 256; x = g_node[x].parent) list_add(&cnt, x);
        return list_end(cnt);
    }
    case D_SHOWN:
        n = g32();
        if (!ok_node(n)) return ans_bool(0);
        if (g_dom_dirty) css_apply();
        return ans_bool(!(g_node[n].flags & NF_HIDDEN) && g_node[n].parent);
    }
    return refuse("it asked for something FLOPOS does not offer");
}
