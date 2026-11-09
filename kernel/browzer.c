/* ===========================================================================
 * browzer.c -- a small web browser.
 *
 * The whole screen, framed: an address along the top, the page in the
 * middle, a status line along the bottom.  Pages come through the same
 * reader `fetch` uses, but into a Page that remembers which characters
 * belong to which link, so they can be scrolled through and followed.
 *
 *   up / down        scroll a line         pgup / pgdn / space   a screen
 *   tab / shift-tab  next / previous link  enter                 follow it
 *   backspace, left  back                  g                     go somewhere
 *   r                again                 h                     home
 *   esc              leave
 *
 * Typing words rather than an address searches, through FrogFind -- a
 * search engine made for old computers that answers in plain HTML over
 * plain http, which is exactly what this one can best read.
 * ===========================================================================
 */
#include "flopos.h"

#define MAX_LINES  1500
#define MAX_WIDTH  98
#define HISTORY    20

static Page s_pg;
static int  s_ready;
static int  s_w, s_h, s_vh;              /* window, and page rows shown    */
static int  s_top;                       /* first page line shown          */
static int  s_sel;                       /* selected link, 0 for none      */
static char s_hist[HISTORY][PAGE_HREF_MAX + 16];
static int  s_hist_n;
static int  s_editing;
static char s_edit[PAGE_HREF_MAX];
static int  s_edit_len;
static int  s_loading;

static const char s_home[] =
    "<title>home</title>"
    "<h1>browzer</h1>"
    "<p>A very small web browser, on a floppy disk, on a Toshiba from 1996.</p>"
    "<p>Press <b>g</b> and type an address, or just some words to search for them.</p>"
    "<h2>Places that suit a machine like this</h2>"
    "<ul>"
    "<li><a href=\"http://frogfind.com/\">FrogFind</a> -- a search engine for vintage computers</li>"
    "<li><a href=\"http://68k.news/\">68k.news</a> -- the news, for old machines</li>"
    "<li><a href=\"https://text.npr.org/\">text.npr.org</a> -- NPR, text only</li>"
    "<li><a href=\"https://lite.cnn.com/\">lite.cnn.com</a> -- CNN, text only</li>"
    "<li><a href=\"http://info.cern.ch/\">info.cern.ch</a> -- the first website there ever was</li>"
    "<li><a href=\"https://wiby.me/\">wiby.me</a> -- a search engine for the old web</li>"
    "<li><a href=\"http://example.com/\">example.com</a></li>"
    "</ul>"
    "<p>tab picks a link, enter follows it, backspace goes back, esc leaves.</p>"
    "<p>Secure pages are encrypted, and the site has to prove who it is.</p>";

/* ---- drawing ------------------------------------------------------------ */
static int link_line(int id)
{
    int y, x;
    for (y = 0; y < s_pg.lines; y++)
        for (x = 0; x < s_pg.width; x++)
            if (s_pg.cells[y * s_pg.width + x].link == id) return y;
    return -1;
}

static void put_text_clipped(int x, int y, const char *t, u8 fg, u8 bg, int maxx)
{
    while (*t && x < maxx) term_win_put(x++, y, (u8)*t++, fg, bg);
}

static void draw(void)
{
    char line[96];
    int x, y, i;
    u8 pp = WIN_PAPER;

    term_win_open(s_w, s_h);
    term_win_frame(0, C_GREY);

    /* The name, with the z in banana. */
    term_win_put(2, 0, ' ', C_GREY, pp);
    term_win_text(3, 0, "brow", C_BRIGHT, pp);
    term_win_put(7, 0, 'z', PAL_BANANA + 6, pp);
    term_win_text(8, 0, "er", C_BRIGHT, pp);
    term_win_put(10, 0, ' ', C_GREY, pp);
    if (s_pg.title[0]) {
        term_win_put(11, 0, FR_H, C_GREY, pp);
        term_win_put(12, 0, ' ', C_GREY, pp);
        x = 13;
        for (i = 0; s_pg.title[i] && x < s_w - 3; i++) term_win_put(x++, 0, (u8)s_pg.title[i], C_TEXT, pp);
        term_win_put(x, 0, ' ', C_GREY, pp);
    }

    /* The address. */
    if (s_editing) {
        term_win_band(1, 1, s_w - 2, WIN_HILITE);
        term_win_text(2, 1, "go:", C_ACCENT, WIN_HILITE);
        x = 6;
        i = s_edit_len > s_w - 10 ? s_edit_len - (s_w - 10) : 0;
        for (; i < s_edit_len; i++) term_win_put(x++, 1, (u8)s_edit[i], C_BRIGHT, WIN_HILITE);
        term_win_put(x, 1, '_', C_ACCENT, WIN_HILITE);
    } else {
        term_win_text(2, 1, s_pg.secure ? "*" : ">", s_pg.secure ? C_ACCENT : C_DIM, pp);
        put_text_clipped(4, 1, s_pg.url, C_TEXT, pp, s_w - 2);
    }

    /* Rules above and below the page, joined to the frame. */
    for (x = 1; x < s_w - 1; x++) {
        term_win_put(x, 2, FR_H, C_GREY, pp);
        term_win_put(x, s_h - 3, FR_H, C_GREY, pp);
    }
    term_win_put(0, 2, FR_TEE_L, C_GREY, pp);       term_win_put(s_w - 1, 2, FR_TEE_R, C_GREY, pp);
    term_win_put(0, s_h - 3, FR_TEE_L, C_GREY, pp); term_win_put(s_w - 1, s_h - 3, FR_TEE_R, C_GREY, pp);

    /* The page. */
    for (y = 0; y < s_vh; y++) {
        int ln = s_top + y;
        if (ln >= s_pg.lines) break;
        for (x = 0; x < s_pg.width; x++) {
            PageCell *c = &s_pg.cells[ln * s_pg.width + x];
            int sel = s_sel && c->link == s_sel;
            if (c->ch == ' ' && !sel) continue;
            term_win_put(2 + x, 3 + y, c->ch, sel ? C_BRIGHT : c->col, sel ? WIN_HILITE : pp);
        }
    }

    /* A thumb down the right edge, for where in the page this is. */
    if (s_pg.lines > s_vh) {
        int pos = (s_vh - 1) * s_top / (s_pg.lines - s_vh);
        term_win_put(s_w - 2, 3 + pos, CH_FILL, C_GREY, pp);
    }

    /* Status: what is happening, or where the picked link goes. */
    if (s_sel && !s_loading) {
        char where[PAGE_HREF_MAX + 16];
        if (!url_resolve(s_pg.url, s_pg.href[s_sel - 1], where, sizeof where))
            fmt_str(where, sizeof where, "(not a page: %s)", s_pg.href[s_sel - 1]);
        term_win_text(2, s_h - 2, "->", C_ACCENT, pp);
        put_text_clipped(5, s_h - 2, where, C_TEXT, pp, s_w - 12);
    } else if (s_pg.note[0]) {
        put_text_clipped(2, s_h - 2, s_pg.note, s_pg.note_col, pp, s_w - 12);
    }
    if (s_pg.lines > 1) {
        fmt_str(line, sizeof line, "%u/%u", (u32)(s_top + 1), (u32)s_pg.lines);
        for (i = 0; line[i]; i++) { }
        term_win_text(s_w - 2 - i, s_h - 2, line, C_DIM, pp);
    }

    /* Keys, let into the bottom of the frame. */
    {
        const char *keys = s_w >= 60 ? " tab link  enter go  bksp back  g go to  esc leave "
                                     : " tab enter bksp g esc ";
        int kl = 0;
        while (keys[kl]) kl++;
        if (kl < s_w - 4) term_win_text((s_w - kl) / 2, s_h - 1, keys, C_DIM, pp);
    }
}

static void progress(void)
{
    draw();
    main_pump(1);
}

/* ---- going places ------------------------------------------------------- */
static void push_history(void)
{
    int i;
    if (!s_pg.url[0]) return;
    if (s_hist_n == HISTORY) {
        for (i = 1; i < HISTORY; i++) {
            int k;
            for (k = 0; k < (int)sizeof s_hist[0]; k++) s_hist[i - 1][k] = s_hist[i][k];
        }
        s_hist_n--;
    }
    for (i = 0; s_pg.url[i] && i < (int)sizeof s_hist[0] - 1; i++) s_hist[s_hist_n][i] = s_pg.url[i];
    s_hist[s_hist_n][i] = 0;
    s_hist_n++;
}

static void home(void)
{
    http_page_html(s_home, &s_pg);
    fmt_str(s_pg.url, sizeof s_pg.url, "flopos:home");
    s_top = 0;
    s_sel = 0;
}

static void go(const char *url)
{
    s_top = 0;
    s_sel = 0;
    if (url[0] == 'f' && url[1] == 'l' && url[2] == 'o' && url[3] == 'p') { home(); return; }
    s_loading = 1;
    s_pg.title[0] = 0;
    s_pg.lines = 1;
    progress();
    if (!net_up() || (!ip_addr() && !ip_apply_saved() && !ip_dhcp())) {
        page_clear(&s_pg);
        fmt_str(s_pg.note, sizeof s_pg.note, "No network. Try the network command.");
        s_pg.note_col = C_ACCENT;
        s_loading = 0;
        return;
    }
    if (!http_page(url, &s_pg) && s_pg.lines <= 1 && s_pg.col == 0) {
        /* Nothing came: say why on the page itself, not only in the
         * status line, and keep the address so r can try again. */
        static char html[600];
        char why[128], where[PAGE_HREF_MAX + 16];
        int i;
        for (i = 0; s_pg.note[i]; i++) why[i] = s_pg.note[i];
        why[i] = 0;
        for (i = 0; s_pg.url[i]; i++) where[i] = s_pg.url[i];
        where[i] = 0;
        if (s_pg.untrusted)
            fmt_str(html, sizeof html,
                    "<title>who is this?</title><h2>This site could not prove who it is.</h2>"
                    "<p>%s</p><p>That can mean somebody is pretending to be it, or that the "
                    "site is set up wrong, or that this laptop's clock is.</p>"
                    "<p>a goes there anyway, unchecked -- type nothing private. "
                    "backspace goes back.</p>", why);
        else
            fmt_str(html, sizeof html,
                    "<title>could not open it</title><h2>That page did not come.</h2>"
                    "<p>%s</p><p>r tries again, backspace goes back, g goes somewhere else.</p>",
                    why[0] ? why : "Nothing answered.");
        {
            int untrusted = s_pg.untrusted;
            http_page_html(html, &s_pg);
            s_pg.untrusted = untrusted;  /* so a can still go anyway        */
        }
        for (i = 0; where[i]; i++) s_pg.url[i] = where[i];
        s_pg.url[i] = 0;
        fmt_str(s_pg.note, sizeof s_pg.note, "%s", why);
        s_pg.note_col = C_ACCENT;
    }
    s_loading = 0;
    if (s_pg.status >= 200 && s_pg.status < 300) {
        if (s_pg.secure && s_pg.checked)
            fmt_str(s_pg.note, sizeof s_pg.note, "%s, vouched for by %s",
                    s_pg.pq ? "encrypted, post-quantum" : "encrypted", s_pg.who);
        else if (s_pg.secure)
            fmt_str(s_pg.note, sizeof s_pg.note, "%s -- NOT checked, could be anybody",
                    s_pg.pq ? "encrypted, post-quantum" : "encrypted");
        else
            fmt_str(s_pg.note, sizeof s_pg.note, "%u bytes, not encrypted", s_pg.bytes);
        s_pg.note_col = s_pg.secure && !s_pg.checked ? C_ACCENT : C_DIM;
    }
}

/* What was typed into the address bar: an address, or words to look for. */
static void go_typed(const char *t)
{
    char url[PAGE_HREF_MAX + 16];
    int i, n = 0, dot = 0, space = 0, scheme = 0;

    for (i = 0; t[i]; i++) {
        if (t[i] == '.') dot = 1;
        if (t[i] == ' ') space = 1;
        if (t[i] == ':' && t[i + 1] == '/' && t[i + 2] == '/') scheme = 1;
    }
    if (scheme) {
        for (i = 0; t[i] && n < (int)sizeof url - 1; i++) url[n++] = t[i];
    } else if (dot && !space) {
        fmt_str(url, sizeof url, "http://%s", t);
        n = -1;
    } else {
        /* Words: ask FrogFind, spelled the way a query is spelled. */
        static const char hexd[] = "0123456789ABCDEF";
        const char *pre = "http://frogfind.com/?q=";
        for (i = 0; pre[i]; i++) url[n++] = pre[i];
        for (i = 0; t[i] && n < (int)sizeof url - 4; i++) {
            u8 c = (u8)t[i];
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                c == '-' || c == '_' || c == '.' || c == '~') url[n++] = (char)c;
            else if (c == ' ') url[n++] = '+';
            else { url[n++] = '%'; url[n++] = hexd[c >> 4]; url[n++] = hexd[c & 15]; }
        }
    }
    if (n >= 0) url[n] = 0;
    push_history();
    go(url);
}

static void follow(void)
{
    char url[PAGE_HREF_MAX + 16];
    if (!s_sel) return;
    if (!url_resolve(s_pg.url, s_pg.href[s_sel - 1], url, sizeof url)) return;
    push_history();
    go(url);
}

static void back(void)
{
    char url[PAGE_HREF_MAX + 16];
    int i;
    if (!s_hist_n) return;
    s_hist_n--;
    for (i = 0; s_hist[s_hist_n][i]; i++) url[i] = s_hist[s_hist_n][i];
    url[i] = 0;
    go(url);
}

/* Next or previous link from the one picked, or from the top of what is
 * on screen, and scroll so it can be seen. */
static void pick(int dir)
{
    int id, ln;
    if (!s_pg.nlinks) return;
    if (!s_sel) {
        /* The first link on screen, or the last one going backwards. */
        int best = 0;
        for (id = 1; id <= s_pg.nlinks; id++) {
            ln = link_line(id);
            if (ln < 0) continue;
            if (dir > 0 && ln >= s_top) { best = id; break; }
            if (dir < 0 && ln < s_top + s_vh) best = id;
        }
        s_sel = best ? best : (dir > 0 ? 1 : s_pg.nlinks);
    } else {
        s_sel += dir;
        if (s_sel < 1) s_sel = s_pg.nlinks;
        if (s_sel > s_pg.nlinks) s_sel = 1;
    }
    ln = link_line(s_sel);
    if (ln >= 0 && (ln < s_top || ln >= s_top + s_vh)) {
        s_top = ln - s_vh / 3;
        if (s_top < 0) s_top = 0;
    }
}

static void scroll(int by)
{
    int max = s_pg.lines - s_vh;
    s_top += by;
    if (s_top > max) s_top = max;
    if (s_top < 0) s_top = 0;
    /* A picked link that has scrolled out of sight is let go. */
    if (s_sel) {
        int ln = link_line(s_sel);
        if (ln < s_top || ln >= s_top + s_vh) s_sel = 0;
    }
}

void browzer_run(const char *start)
{
    int k;

    term_win_open(term_cols(), term_rows());
    s_w = term_win_width();
    s_h = term_win_height();
    s_vh = s_h - 6;

    if (!s_ready) {
        s_pg.cells = (PageCell *)halloc(MAX_LINES * MAX_WIDTH * sizeof(PageCell));
        s_pg.href = (char (*)[PAGE_HREF_MAX])halloc(PAGE_LINKS * PAGE_HREF_MAX);
        s_pg.max_lines = MAX_LINES;
        s_pg.progress = progress;
        s_ready = 1;
        s_pg.width = 0;
    }

    /* Pages are laid out for a width; if the screen changed since the
     * last visit, the old one is laid out wrong, so it is fetched again. */
    if (s_pg.width != s_w - 4) {
        s_pg.width = s_w - 4;
        if (!start || !start[0]) {
            if (s_pg.url[0] && s_pg.url[0] != 'f') start = s_pg.url;
            else start = 0;
        }
        if (!start) home();
    }
    if (start && start[0]) go_typed(start);
    s_editing = 0;

    for (;;) {
        draw();
        k = ui_key();
        if (k < 0) { main_pump(0); continue; }

        if (s_editing) {
            if (k == UK_ENTER) {
                s_editing = 0;
                s_edit[s_edit_len] = 0;
                if (s_edit_len) go_typed(s_edit);
            } else if (k == UK_ESC) {
                s_editing = 0;
            } else if (k == UK_BKSP) {
                if (s_edit_len) s_edit_len--;
            } else if (k >= 32 && k < 127 && s_edit_len < PAGE_HREF_MAX - 1) {
                s_edit[s_edit_len++] = (char)k;
            }
            continue;
        }

        switch (k) {
        case UK_ESC: case 'q':
            term_win_close();
            return;
        case UK_UP:    scroll(-1); break;
        case UK_DOWN:  scroll(+1); break;
        case UK_PGUP:  scroll(-(s_vh - 1)); break;
        case UK_PGDN: case ' ': scroll(s_vh - 1); break;
        case UK_HOME:  s_top = 0; s_sel = 0; break;
        case UK_END:   scroll(s_pg.lines); break;
        case UK_TAB:   pick(+1); break;
        case UK_BTAB:  pick(-1); break;
        case UK_ENTER: follow(); break;
        case UK_BKSP: case UK_LEFT: back(); break;
        case 'g': case '/': s_editing = 1; s_edit_len = 0; break;
        case 'r': if (s_pg.url[0]) { char u[PAGE_HREF_MAX + 16]; int i;
                      for (i = 0; s_pg.url[i]; i++) u[i] = s_pg.url[i];
                      u[i] = 0; go(u); } break;
        case 'h': push_history(); home(); break;
        case 'a':                        /* anyway: past a failed proof     */
            if (s_pg.untrusted && s_pg.url[0]) {
                char u[PAGE_HREF_MAX + 16]; int i;
                for (i = 0; s_pg.url[i]; i++) u[i] = s_pg.url[i];
                u[i] = 0;
                tls_check(0);
                go(u);
                tls_check(1);
            }
            break;
        default: break;
        }
    }
}
