/* ===========================================================================
 * browzer.c -- a small web browser, and the app it is.
 *
 * BROWZER.APP on the floppy.  Typed as `browzer`, it is the browser; typed
 * as `fetch`, it reads one page out into the terminal as text and stops.
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
 * Forms: tab reaches the boxes and buttons as it does links.  Enter on a
 * box starts typing into it; enter again sends the form (in a big box,
 * enter is a new line and tab finishes).  Enter on a button sends the
 * form with that button.  Esc stops typing.
 *
 * Typing words rather than an address searches, through FrogFind -- a
 * search engine made for old computers that answers in plain HTML over
 * plain http, which is exactly what this one can best read.
 * ===========================================================================
 */
#include "web.h"

#define MAX_LINES  1500
#define MAX_WIDTH  98
#define HISTORY    20

const Api *A;

/* Everything a page needs, kept here in the app's own memory. */
static Page      s_pg;
static PageCell  s_cells[MAX_LINES * MAX_WIDTH];
static char      s_hrefs[PAGE_LINKS][PAGE_HREF_MAX];
static PageForm  s_forms[PAGE_FORMS];
static PageField s_fields[PAGE_FIELDS];
static char      s_pool[PAGE_POOL];
static u16       s_link_field[PAGE_LINKS + 1];
static u8        s_body[PAGE_POOL * 3];  /* a form, ready to send          */
static int  s_ready;
static int  s_typing = -1;               /* the field being typed into     */

int ui_key(void)
{
    int mods, k = A->key(&mods);
    if (k >= 0 && (mods & (M_CTRL | M_ALT))) return -1;
    return k;
}

static PageField *field_of(int link)
{
    int f = link > 0 && link <= PAGE_LINKS ? s_pg.link_field[link] : 0;
    return f ? &s_pg.fields[f - 1] : 0;
}
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
    "<p>tab picks a link, enter follows it, backspace goes back, esc leaves. "
    "Boxes on a page are typed into the same way: tab to one, enter, type, enter sends.</p>"
    "<p>b adds the page you are on to your bookmarks, and m lists them. On that list, p puts a bookmark on this home page.</p>"
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

    /* The caret, in the box being typed into. */
    if (s_typing >= 0) {
        PageField *f = &s_pg.fields[s_typing];
        u32 k;
        int n = 0, cy = f->line - s_top;
        for (k = 0; k < f->vlen; k++) if ((s_pg.pool[f->val + k] & 0xC0) != 0x80) n++;
        if (n > f->width - 1) n = f->width - 1;
        if (cy >= 0 && cy < s_vh) term_win_put(2 + f->col + n, 3 + cy, ' ', C_TEXT, C_BRIGHT);
    }

    /* Status: what is happening, or where the picked link goes. */
    if (s_typing >= 0) {
        put_text_clipped(2, s_h - 2, s_pg.fields[s_typing].kind == FK_AREA
                         ? "typing: enter is a new line, tab finishes"
                         : "typing: enter sends it, esc stops", C_ACCENT, pp, s_w - 12);
    } else if (s_sel && !s_loading && field_of(s_sel)) {
        PageField *f = field_of(s_sel);
        char where[PAGE_HREF_MAX + 16];
        if (f->kind == FK_SUBMIT) {
            const char *act = s_pg.forms[f->form].action;
            if (!url_resolve(s_pg.url, act[0] ? act : s_pg.url, where, sizeof where)) where[0] = 0;
            term_win_text(2, s_h - 2, "sends to", C_ACCENT, pp);
            put_text_clipped(11, s_h - 2, where, C_TEXT, pp, s_w - 12);
        } else
            put_text_clipped(2, s_h - 2, "a box: enter to type into it", C_ACCENT, pp, s_w - 12);
    } else if (s_sel && !s_loading) {
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
        const char *keys = s_typing >= 0 ? " type  enter send  tab done  esc stop "
                         : s_w >= 66 ? " tab link  enter go  bksp back  g go to  b mark  m marks  esc "
                         : s_w >= 60 ? " tab link  enter go  bksp back  g go to  esc leave "
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

/* ---- bookmarks -------------------------------------------------------------
 * BOOKMARK.TXT on the floppy, plain text, one a line: the address, a
 * space, the page's title.  A line that starts with "* " is also on the
 * home page.  Written whole, in place, every time one changes. */
#define MARKS 48
typedef struct { char url[PAGE_HREF_MAX]; char title[64]; u8 home; } Mark;
static Mark s_mark[MARKS];
static int  s_nmarks, s_marks_read, s_del_armed;
static u8   s_mfile[8192];

static void marks_read(void)
{
    AppFile f;
    int n, i = 0;
    if (s_marks_read) return;
    s_marks_read = 1;
    s_nmarks = 0;
    if (!A->file_find("BOOKMARK.TXT", &f)) return;
    n = A->file_read(&f, s_mfile, sizeof s_mfile);
    while (i < n && s_mfile[i] != 0x1A && s_mfile[i] && s_nmarks < MARKS) {
        Mark *m = &s_mark[s_nmarks];
        int k = 0;
        m->home = 0;
        if (s_mfile[i] == '*' && i + 1 < n && s_mfile[i + 1] == ' ') { m->home = 1; i += 2; }
        while (i < n && s_mfile[i] > ' ' && k < PAGE_HREF_MAX - 1) m->url[k++] = (char)s_mfile[i++];
        m->url[k] = 0;
        while (i < n && s_mfile[i] == ' ') i++;
        k = 0;
        while (i < n && s_mfile[i] != '\n' && s_mfile[i] != '\r' && s_mfile[i] != 0x1A && s_mfile[i]) {
            if (k < 63) m->title[k++] = (char)s_mfile[i];
            i++;
        }
        m->title[k] = 0;
        while (i < n && (s_mfile[i] == '\n' || s_mfile[i] == '\r')) i++;
        if (m->url[0]) s_nmarks++;
    }
}

static int marks_write(void)
{
    AppFile f;
    int n = 0, i, k;
    if (!A->file_find("BOOKMARK.TXT", &f) || f.size > sizeof s_mfile) return 0;
    for (i = 0; i < s_nmarks; i++) {
        Mark *m = &s_mark[i];
        if (n + PAGE_HREF_MAX + 70 > (int)f.size - 1) break;
        if (m->home) { s_mfile[n++] = '*'; s_mfile[n++] = ' '; }
        for (k = 0; m->url[k]; k++) s_mfile[n++] = (u8)m->url[k];
        s_mfile[n++] = ' ';
        for (k = 0; m->title[k]; k++) s_mfile[n++] = (u8)m->title[k];
        s_mfile[n++] = '\r'; s_mfile[n++] = '\n';
    }
    s_mfile[n++] = 0x1A;                         /* where DOS stops reading */
    while (n < (int)f.size) s_mfile[n++] = 0;
    return A->file_write(&f, s_mfile);
}

static int mark_find(const char *url)
{
    int i, k;
    for (i = 0; i < s_nmarks; i++) {
        for (k = 0; url[k] && url[k] == s_mark[i].url[k]; k++) { }
        if (!url[k] && !s_mark[i].url[k]) return i;
    }
    return -1;
}

static void say(const char *t, u8 col)
{
    fmt_str(s_pg.note, sizeof s_pg.note, "%s", t);
    s_pg.note_col = col;
}

/* Text safe to put into a page we write ourselves. */
static int html_text(char *out, int n, int cap, const char *t)
{
    for (; *t && n < cap - 8; t++) {
        if (*t == '<') { out[n++] = '&'; out[n++] = 'l'; out[n++] = 't'; out[n++] = ';'; }
        else if (*t == '&') { out[n++] = '&'; out[n++] = 'a'; out[n++] = 'm'; out[n++] = 'p'; out[n++] = ';'; }
        else if (*t == '"') { out[n++] = '&'; out[n++] = '#'; out[n++] = '3'; out[n++] = '4'; out[n++] = ';'; }
        else out[n++] = *t;
    }
    return n;
}

static int put(char *out, int n, int cap, const char *t)
{
    while (*t && n < cap - 1) out[n++] = *t++;
    return n;
}

static void home(void)
{
    static char page[sizeof s_home + MARKS * (PAGE_HREF_MAX + 120) + 200];
    int n = 0, i, cap = (int)sizeof page, any = 0;
    const char *at = s_home;

    marks_read();
    /* The home page, with your pinned bookmarks first. */
    while (*at && !(at[0] == '<' && at[1] == 'h' && at[2] == '2')) page[n++] = *at++;
    for (i = 0; i < s_nmarks; i++) if (s_mark[i].home) any = 1;
    if (any) {
        n = put(page, n, cap, "<h2>Your bookmarks</h2><ul>");
        for (i = 0; i < s_nmarks; i++) {
            if (!s_mark[i].home) continue;
            n = put(page, n, cap, "<li><a href=\"");
            n = html_text(page, n, cap, s_mark[i].url);
            n = put(page, n, cap, "\">");
            n = html_text(page, n, cap, s_mark[i].title[0] ? s_mark[i].title : s_mark[i].url);
            n = put(page, n, cap, "</a></li>");
        }
        n = put(page, n, cap, "</ul>");
    }
    n = put(page, n, cap, at);
    page[n] = 0;
    http_page_html(page, &s_pg);
    fmt_str(s_pg.url, sizeof s_pg.url, "flopos:home");
    s_top = 0;
    s_sel = 0;
}

/* The list of them, as a page: pick one with tab, then p or d. */
static void marks_page(void)
{
    static char page[MARKS * (PAGE_HREF_MAX * 2 + 140) + 600];
    int n = 0, i, cap = (int)sizeof page;

    marks_read();
    n = put(page, n, cap, "<title>bookmarks</title><h1>Bookmarks</h1>");
    if (!s_nmarks)
        n = put(page, n, cap, "<p>None yet. On any page, b keeps it here.</p>");
    else {
        n = put(page, n, cap, "<p>tab picks one, enter goes there, p puts it on the home page "
                              "or takes it off, d deletes it.</p><ul>");
        for (i = 0; i < s_nmarks; i++) {
            n = put(page, n, cap, "<li>");
            if (s_mark[i].home) n = put(page, n, cap, "<b>home</b> ");
            n = put(page, n, cap, "<a href=\"");
            n = html_text(page, n, cap, s_mark[i].url);
            n = put(page, n, cap, "\">");
            n = html_text(page, n, cap, s_mark[i].title[0] ? s_mark[i].title : s_mark[i].url);
            n = put(page, n, cap, "</a> <i>");
            n = html_text(page, n, cap, s_mark[i].url);
            n = put(page, n, cap, "</i></li>");
        }
        n = put(page, n, cap, "</ul>");
    }
    page[n] = 0;
    http_page_html(page, &s_pg);
    fmt_str(s_pg.url, sizeof s_pg.url, "flopos:bookmarks");
}

static int on_marks_page(void)
{
    return s_pg.url[0] == 'f' && s_pg.url[7] == 'b';            /* flopos:bookmarks */
}

static void mark_this(void)
{
    Mark *m;
    int i;
    marks_read();
    if (s_pg.url[0] == 'f' && s_pg.url[1] == 'l') { say("That is one of mine; there is nothing to keep.", C_ACCENT); return; }
    if (mark_find(s_pg.url) >= 0) { say("Already in your bookmarks. m shows them.", C_DIM); return; }
    if (s_nmarks >= MARKS) { say("The bookmarks are full. d on one in m makes room.", C_ACCENT); return; }
    m = &s_mark[s_nmarks];
    for (i = 0; s_pg.url[i] && i < PAGE_HREF_MAX - 1; i++) m->url[i] = s_pg.url[i];
    m->url[i] = 0;
    for (i = 0; s_pg.title[i] && i < 63; i++) m->title[i] = s_pg.title[i] == '\n' ? ' ' : s_pg.title[i];
    m->title[i] = 0;
    m->home = 0;
    s_nmarks++;
    say(marks_write() ? "Added to your bookmarks. m lists them, and p on that list puts one on the home page."
                      : "Could not write BOOKMARK.TXT to the floppy.", C_ACCENT);
}

/* p or d on the bookmarks page, for the bookmark picked. */
static void mark_change(int key)
{
    char url[PAGE_HREF_MAX + 16];
    int i, top = s_top;
    if (!s_sel || field_of(s_sel)) { say("Pick one with tab first.", C_ACCENT); return; }
    for (i = 0; s_pg.href[s_sel - 1][i] && i < (int)sizeof url - 1; i++) url[i] = s_pg.href[s_sel - 1][i];
    url[i] = 0;
    i = mark_find(url);
    if (i < 0) return;
    if (key == 'd') {
        if (!s_del_armed) { s_del_armed = 1; say("d again deletes it.", C_ACCENT); return; }
        s_del_armed = 0;
        for (; i < s_nmarks - 1; i++) s_mark[i] = s_mark[i + 1];
        s_nmarks--;
    } else {
        s_mark[i].home = !s_mark[i].home;
    }
    if (!marks_write()) { say("Could not write BOOKMARK.TXT to the floppy.", C_ACCENT); return; }
    marks_page();
    s_top = top;
    say(key == 'd' ? "Deleted." : "Changed. h shows the home page.", C_DIM);
}

/* A page, or the answer to a form sent to url (post, of postlen bytes). */
static void go_with(const char *url, const u8 *post, int postlen)
{
    s_top = 0;
    s_sel = 0;
    s_typing = -1;
    if (url[0] == 'f' && url[1] == 'l' && url[2] == 'o' && url[3] == 'p') { home(); return; }
    s_loading = 1;
    s_pg.title[0] = 0;
    s_pg.lines = 1;
    progress();
    if (!A->net_online()) {
        page_clear(&s_pg);
        fmt_str(s_pg.note, sizeof s_pg.note, "No network. Try the network command.");
        s_pg.note_col = C_ACCENT;
        s_loading = 0;
        return;
    }
    if (!http_load(url, &s_pg, post, postlen) && s_pg.lines <= 1 && s_pg.col == 0) {
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

static void go(const char *url) { go_with(url, 0, 0); }

/* ---- forms ----------------------------------------------------------------- */
/* Bytes as a form sends them: letters and digits as they are, a space as
 * +, anything else as %XX. */
static int urlenc(u8 *out, int n, int cap, const char *p, u32 len)
{
    static const char hexd[] = "0123456789ABCDEF";
    u32 i;
    for (i = 0; i < len && n < cap - 3; i++) {
        u8 c = (u8)p[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '*') out[n++] = c;
        else if (c == ' ') out[n++] = '+';
        else if (c == '\n') { out[n++] = '%'; out[n++] = '0'; out[n++] = 'D';
                              out[n++] = '%'; out[n++] = '0'; out[n++] = 'A'; }
        else { out[n++] = '%'; out[n++] = (u8)hexd[c >> 4]; out[n++] = (u8)hexd[c & 15]; }
    }
    return n;
}

/* Send the form that field belongs to: every named box and hidden value
 * in it, and the button, if a button was what sent it. */
static void submit(PageField *by)
{
    PageForm *form = &s_pg.forms[by->form];
    char url[PAGE_HREF_MAX + 16];
    int n = 0, i, cap = (int)sizeof s_body;

    for (i = 0; i < s_pg.nfields; i++) {
        PageField *f = &s_pg.fields[i];
        if (f->form != by->form || !f->nlen) continue;
        if (f->kind == FK_SUBMIT && f != by) continue;
        if (n) s_body[n++] = '&';
        n = urlenc(s_body, n, cap, s_pg.pool + f->name, f->nlen);
        s_body[n++] = '=';
        n = urlenc(s_body, n, cap, s_pg.pool + f->val, f->vlen);
    }
    if (n >= cap - 4) {
        fmt_str(s_pg.note, sizeof s_pg.note, "That form is too big to send.");
        s_pg.note_col = C_ACCENT;
        return;
    }
    if (!url_resolve(s_pg.url, form->action[0] ? form->action : s_pg.url, url, sizeof url)) {
        fmt_str(s_pg.note, sizeof s_pg.note, "The form says to send it nowhere I can find.");
        s_pg.note_col = C_ACCENT;
        return;
    }
    push_history();
    if (form->post) {
        go_with(url, s_body, n);
    } else {
        /* A GET form: what it says goes after a ? in the address. */
        int k = 0;
        while (url[k] && url[k] != '?' && url[k] != '#') k++;
        if (k + 1 + n >= (int)sizeof url) {
            fmt_str(s_pg.note, sizeof s_pg.note, "Too much to send in an address.");
            s_pg.note_col = C_ACCENT;
            return;
        }
        url[k++] = '?';
        for (i = 0; i < n; i++) url[k++] = (char)s_body[i];
        url[k] = 0;
        go(url);
    }
}

/* One typed character into the box being typed into, as UTF-8. */
static void type_char(PageField *f, int k)
{
    char b[2];
    int n = 0, i;
    switch (k) {
    case CH_ARING:   b[0] = (char)0xC3; b[1] = (char)0x85; n = 2; break;
    case CH_ADIA:    b[0] = (char)0xC3; b[1] = (char)0x84; n = 2; break;
    case CH_ODIA:    b[0] = (char)0xC3; b[1] = (char)0x96; n = 2; break;
    case CH_ARING_L: b[0] = (char)0xC3; b[1] = (char)0xA5; n = 2; break;
    case CH_ADIA_L:  b[0] = (char)0xC3; b[1] = (char)0xA4; n = 2; break;
    case CH_ODIA_L:  b[0] = (char)0xC3; b[1] = (char)0xB6; n = 2; break;
    default:
        if (k == '\n' || (k >= 32 && k < 127)) { b[0] = (char)k; n = 1; }
    }
    if (!n || f->vlen + (u32)n > f->vcap) return;
    for (i = 0; i < n; i++) s_pg.pool[f->val + f->vlen++] = b[i];
    page_field_show(&s_pg, (int)(f - s_pg.fields));
}

static void unchar(PageField *f)
{
    if (!f->vlen) return;
    f->vlen--;
    while (f->vlen && (s_pg.pool[f->val + f->vlen] & 0xC0) == 0x80) f->vlen--;  /* the whole letter */
    page_field_show(&s_pg, (int)(f - s_pg.fields));
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
    PageField *f;
    if (!s_sel) return;
    if ((f = field_of(s_sel)) != 0) {
        if (f->kind == FK_SUBMIT) submit(f);
        else s_typing = (int)(f - s_pg.fields);
        return;
    }
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
        s_pg.cells = s_cells;
        s_pg.href = s_hrefs;
        s_pg.forms = s_forms;
        s_pg.fields = s_fields;
        s_pg.pool = s_pool;
        s_pg.link_field = s_link_field;
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

        if (s_typing >= 0) {             /* into a box on the page          */
            PageField *f = &s_pg.fields[s_typing];
            if (k == UK_ESC) s_typing = -1;
            else if (k == UK_TAB) { s_typing = -1; pick(+1); }
            else if (k == UK_BKSP) unchar(f);
            else if (k == UK_ENTER) {
                if (f->kind == FK_AREA) type_char(f, '\n');
                else { s_typing = -1; submit(f); }
            } else type_char(f, k);
            continue;
        }

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

        if (k != 'd') s_del_armed = 0;
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
        case 'b': mark_this(); break;
        case 'm': push_history(); marks_page(); s_top = 0; s_sel = 0; break;
        case 'p': case 'd':
            if (on_marks_page()) mark_change(k);
            break;
        case 'a':                        /* anyway: past a failed proof     */
            if (s_pg.untrusted && s_pg.url[0]) {
                char u[PAGE_HREF_MAX + 16]; int i;
                for (i = 0; s_pg.url[i]; i++) u[i] = s_pg.url[i];
                u[i] = 0;
                A->conn_check(0);
                go(u);
                A->conn_check(1);
            }
            break;
        default: break;
        }
    }
}

/* ---- the app ---------------------------------------------------------------- */
/* browzer [address or words], or fetch address: the words after it are
 * where to go. */
int app_main(const Api *api, int argc, char **argv)
{
    static char start[PAGE_HREF_MAX];
    int a, i, n = 0;

    A = api;
    for (a = 1; a < argc; a++) {
        if (a > 1 && n < PAGE_HREF_MAX - 1) start[n++] = ' ';
        for (i = 0; argv[a][i] && n < PAGE_HREF_MAX - 1; i++) start[n++] = argv[a][i];
    }
    start[n] = 0;

    if (argc >= 1 && argv[0][0] == 'f') {         /* fetch: into the terminal */
        if (n) http_fetch(start);
        return 0;
    }
    browzer_run(start);
    return 0;
}
