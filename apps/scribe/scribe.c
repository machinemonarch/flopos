/* ===========================================================================
 * scribe.c -- a small word processor, and the first program FLOPOS loads
 * from the floppy rather than carrying in its kernel.
 *
 * Paragraphs, each one Body, Title, or Heading 1 to 4.  Characters, each
 * one any mix of bold, italic and underline.  Text wraps at word breaks;
 * a Title is centred.  Shift with the arrows selects; the formatting keys
 * apply to the selection, or with nothing selected switch the style of
 * whatever is typed next, the way word processors always have.
 *
 *   ctrl-b  bold        ctrl-i  italic       ctrl-u  underline
 *   ctrl-p  next paragraph style, or ctrl-0 body, ctrl-1..4 headings,
 *           ctrl-t title
 *   ctrl-m  Markdown: the same document as the text it is kept as, marks
 *           and all, to read or edit directly; ctrl-m again for the page
 *   ctrl-s  save        ctrl-o  open         ctrl-n  new
 *   esc     leave (twice, if there is something unsaved)
 *
 * Documents live in the inventory, under names of your choosing: plain
 * text any other computer can read, each as long as it is.  `scribe` alone
 * is a new document; `scribe NOTES.MD` or `scribe LETTERS/ANNA.MD` opens
 * that one, or starts it if it is not there; ctrl-o goes to the inventory
 * to pick one.  See doc.c.
 * ===========================================================================
 */
#include "app.h"
#include "doc.h"

#define ATTRS (AT_BOLD | AT_ITALIC | AT_UNDERLINE)

static const Api *A;
static Doc  s_doc;
static int  s_cur, s_anchor = -1, s_want = -1;
static u8   s_pen;
static int  s_dirty, s_esc;
static char s_path[INV_PATH];            /* where it is kept; "" until saved */
static char s_dir[INV_PATH];             /* the folder a new one will go in */
static int  s_back;                      /* came from the inventory: go back there */
static int  s_top;
static int  W, H, TW, TH;
static char s_msg[80];
static u8   s_msg_col;
static u8   s_file[FILE_MAX];
static int  s_md;                        /* showing the Markdown, not the page */
static Doc  s_tmp;
static u8   s_look_fg[DOC_MAX], s_look_at[DOC_MAX];   /* Markdown colouring */
static u8   s_look_own[DOC_MAX];        /* ...and the character's own styling */

static void md_look(void);

typedef struct { int start, len, x0; u8 style; } Line;
static Line s_line[DOC_MAX + 1];
static int  s_nlines;

static const char *s_style_name[ST_COUNT] =
    { "Body", "Title", "Heading 1", "Heading 2", "Heading 3", "Heading 4" };

static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }

static void say(const char *t, u8 col)
{
    int i;
    for (i = 0; t[i] && i < (int)sizeof s_msg - 1; i++) s_msg[i] = t[i];
    s_msg[i] = 0;
    s_msg_col = col;
}

/* ---- where things are ------------------------------------------------------ */
static int para_end(int pos)   { while (s_doc.c[pos].ch != '\n') pos++; return pos; }
static int para_start(int pos) { while (pos > 0 && s_doc.c[pos - 1].ch != '\n') pos--; return pos; }

/* Lay the document out into screen lines: words wrapped at the width, a
 * Title centred.  An empty paragraph is still one line, so the cursor has
 * somewhere to be. */
static void layout(void)
{
    int i = 0;
    s_nlines = 0;
    while (i < s_doc.len) {
        int e = para_end(i), ls = i;
        u8 st = s_doc.c[e].at;
        do {
            int take = e - ls, k;
            Line *L = &s_line[s_nlines++];
            if (take > TW) {
                int brk = -1;
                for (k = ls + TW - 1; k > ls; k--) if (s_doc.c[k].ch == ' ') { brk = k; break; }
                take = brk > 0 ? brk - ls + 1 : TW;
            }
            L->start = ls; L->len = take; L->style = st; L->x0 = 0;
            if (st == ST_TITLE) {
                int vis = take;
                while (vis > 0 && s_doc.c[ls + vis - 1].ch == ' ') vis--;
                L->x0 = (TW - vis) / 2;
                if (L->x0 < 0) L->x0 = 0;
            }
            ls += take;
        } while (ls < e);
        i = e + 1;
    }
    if (s_md) md_look();
}

/* ---- Markdown ------------------------------------------------------------- */
/* The marks at the start of a Markdown paragraph, and the style they give. */
static int md_prefix(int ps, int *st)
{
    static const char *pre[ST_COUNT] = { "", "% ", "# ", "## ", "### ", "#### " };
    int s, k;
    for (s = ST_H4; s > ST_BODY; s--) {
        for (k = 0; pre[s][k] && s_doc.c[ps + k].ch == (u8)pre[s][k]; k++) { }
        if (!pre[s][k]) { if (st) *st = s; return k; }
    }
    if (st) *st = ST_BODY;
    return 0;
}

static int cells_are(int p, int e, const char *t)
{
    for (; *t; t++, p++) if (p >= e || s_doc.c[p].ch != (u8)*t) return 0;
    return 1;
}

static u8 style_fg(u8 st);
static u8 style_at(u8 st);

/* Colour the Markdown the way it will look: the marks dim, the words
 * between them already bold, slanted, underlined or a heading.  The same
 * reading doc.c gives the file, so what is shown is what will come back. */
static void md_look(void)
{
    int ps = 0;
    while (ps < s_doc.len) {
        int e = para_end(ps), st, p = ps + md_prefix(ps, &st), k;
        u8 at = 0, fg = style_fg((u8)st), sat = style_at((u8)st);
        for (k = ps; k < p; k++) { s_look_fg[k] = A_DIM; s_look_at[k] = 0; }
        while (p < e) {
            u8 ch = s_doc.c[p].ch;
            int n = 0;
            if (ch == '\\' && p + 1 < e) n = 1;
            else if (cells_are(p, e, "<u>"))  { n = 3; at |= AT_UNDERLINE; }
            else if (cells_are(p, e, "</u>")) { n = 4; at &= (u8)~AT_UNDERLINE; }
            else if (ch == '*') {
                while (p + n < e && s_doc.c[p + n].ch == '*') n++;
                if ((n / 2) & 1) at ^= AT_BOLD;
                if (n & 1) at ^= AT_ITALIC;
            }
            for (k = 0; k < n; k++) { s_look_fg[p + k] = A_DIM; s_look_at[p + k] = 0; s_look_own[p + k] = at; }
            p += n;
            if (n && ch != '\\') continue;
            s_look_fg[p] = fg;
            s_look_at[p] = (u8)(at | sat);
            s_look_own[p] = at;
            p++;
        }
        s_look_fg[e] = A_DIM; s_look_at[e] = 0;
        ps = e + 1;
    }
}

static int line_of(int pos)
{
    int l;
    for (l = 0; l < s_nlines; l++) {
        Line *L = &s_line[l];
        if (pos >= L->start && pos < L->start + L->len) return l;
        if (pos == L->start + L->len && s_doc.c[pos].ch == '\n') return l;
    }
    return s_nlines - 1;
}

/* The position in line l nearest screen column col. */
static int pos_at(int l, int col)
{
    Line *L = &s_line[l];
    int k = col - L->x0, end = L->start + L->len;
    if (k < 0) k = 0;
    if (k >= L->len) return s_doc.c[end].ch == '\n' ? end : end - 1;
    return L->start + k;
}

/* ---- styling -------------------------------------------------------------- */
static u8 style_fg(u8 st)
{
    return st == ST_BODY ? A_TEXT : (st == ST_H3 || st == ST_H4) ? A_ACCENT : A_BRIGHT;
}

static u8 style_at(u8 st)
{
    switch (st) {
    case ST_TITLE: return AT_BOLD;
    case ST_H1:    return AT_BOLD | AT_UNDERLINE;
    case ST_H2:
    case ST_H3:    return AT_BOLD;
    default:       return 0;
    }
}

static int sel_from(void) { return s_anchor < s_cur ? s_anchor : s_cur; }
static int sel_to(void)   { return s_anchor < s_cur ? s_cur : s_anchor; }
static int have_sel(void) { return s_anchor >= 0 && s_anchor != s_cur; }

/* What gets typed next looks like what it is typed after. */
static void pen_from_cursor(void)
{
    s_pen = (s_cur > 0 && s_doc.c[s_cur - 1].ch != '\n') ? (u8)(s_doc.c[s_cur - 1].at & ATTRS) : 0;
}

/* ---- changing the text ----------------------------------------------------- */
static void remove_range(int a, int b)            /* [a, b), never the last end */
{
    int first_style, k;
    if (b > s_doc.len - 1) b = s_doc.len - 1;
    if (a >= b) return;
    first_style = s_doc.c[para_end(a)].at;         /* the merged paragraph keeps it */
    for (k = b; k < s_doc.len; k++) s_doc.c[k - (b - a)] = s_doc.c[k];
    s_doc.len -= b - a;
    s_doc.c[para_end(a)].at = (u8)first_style;
    s_cur = a;
    s_anchor = -1;
    s_dirty = 1;
}

static void insert(u8 ch, u8 at)
{
    int k;
    if (have_sel()) remove_range(sel_from(), sel_to());
    s_anchor = -1;
    if (s_doc.len >= DOC_MAX - 1) { say("The document is as long as it can be.", A_ACCENT); return; }
    for (k = s_doc.len; k > s_cur; k--) s_doc.c[k] = s_doc.c[k - 1];
    s_doc.c[s_cur].ch = ch;
    s_doc.c[s_cur].at = at;
    s_doc.len++;
    s_cur++;
    s_dirty = 1;
}

/* Enter: the paragraph is split, and both halves keep its style -- unless
 * the cursor was at the end of a title or heading, in which case what
 * comes next is ordinary text, which is nearly always what was meant. */
static void new_paragraph(void)
{
    int e, at_end;
    if (have_sel()) remove_range(sel_from(), sel_to());
    e = para_end(s_cur);
    at_end = (s_cur == e);
    insert('\n', s_doc.c[e].at);
    if (at_end) s_doc.c[para_end(s_cur)].at = ST_BODY;
}

/* Raw cells in and out, for the Markdown keys.  Positions after the change
 * move along with the text. */
static int moved(int p, int at, int by)
{
    if (p < 0) return p;
    if (by > 0) return p >= at ? p + by : p;
    return p >= at - by ? p + by : p > at ? at : p;
}

static int cells_put(int pos, const char *t, int *a, int *b)
{
    int n = slen(t), k;
    if (s_doc.len + n >= DOC_MAX) { say("The document is as long as it can be.", A_ACCENT); return 0; }
    for (k = s_doc.len - 1; k >= pos; k--) s_doc.c[k + n] = s_doc.c[k];
    for (k = 0; k < n; k++) { s_doc.c[pos + k].ch = (u8)t[k]; s_doc.c[pos + k].at = 0; }
    s_doc.len += n;
    s_cur = moved(s_cur, pos, n); s_anchor = moved(s_anchor, pos, n);
    if (a) *a = moved(*a, pos, n);
    if (b) *b = moved(*b, pos, n);
    s_dirty = 1;
    return 1;
}

static void cells_cut(int pos, int n, int *a, int *b)
{
    int k;
    for (k = pos + n; k < s_doc.len; k++) s_doc.c[k - n] = s_doc.c[k];
    s_doc.len -= n;
    s_cur = moved(s_cur, pos, -n); s_anchor = moved(s_anchor, pos, -n);
    if (a) *a = moved(*a, pos, -n);
    if (b) *b = moved(*b, pos, -n);
    s_dirty = 1;
}

static int stars_before(int p) { int n = 0; while (p - n > 0 && s_doc.c[p - n - 1].ch == '*') n++; return n; }
static int stars_after(int p)  { int n = 0; while (s_doc.c[p + n].ch == '*') n++; return n; }

/* In Markdown, ^B ^I ^U put the marks around the selection, or take them
 * off if they are already there; with nothing selected they put down an
 * empty pair with the cursor inside. */
static void md_toggle(u8 attr)
{
    const char *open = attr == AT_BOLD ? "**" : attr == AT_ITALIC ? "*" : "<u>";
    const char *close = attr == AT_UNDERLINE ? "</u>" : open;
    int a = have_sel() ? sel_from() : s_cur, b = have_sel() ? sel_to() : s_cur;
    int lo = slen(open), lc = slen(close), on;

    if (attr == AT_UNDERLINE)
        on = a >= 3 && cells_are(a - 3, a, "<u>") && cells_are(b, s_doc.len, "</u>");
    else {
        int l = stars_before(a), r = stars_after(b);
        on = attr == AT_BOLD ? (((l / 2) & 1) && ((r / 2) & 1)) : ((l & 1) && (r & 1));
    }
    s_anchor = -1;
    if (on) {                                    /* the close first: it is later */
        cells_cut(b, lc, 0, 0);
        cells_cut(a - lo, lo, 0, 0);
        a -= lo; b -= lo;
    } else {
        if (!cells_put(b, close, 0, 0)) return;
        if (!cells_put(a, open, 0, 0)) return;
        a += lo; b += lo;
    }
    if (a == b) { s_cur = a; s_anchor = -1; }
    else        { s_anchor = a; s_cur = b; }
}

/* And ^P, ^T, ^0 to ^4 change the marks at the start of the paragraph. */
static void md_style(int st, int cycle)
{
    static const char *pre[ST_COUNT] = { "", "% ", "# ", "## ", "### ", "#### " };
    int a = have_sel() ? sel_from() : s_cur, b = have_sel() ? sel_to() : s_cur, p;
    for (p = para_start(a); p <= b && p < s_doc.len; p = para_end(p) + 1) {
        int old, n = md_prefix(p, &old);
        int want = cycle ? (old + 1) % ST_COUNT : st;
        if (n) cells_cut(p, n, &a, &b);
        if (pre[want][0]) cells_put(p, pre[want], &a, &b);
        st = want;
    }
    say(s_style_name[st], A_DIM);
}

/* Between the page and its Markdown.  The cursor stays where it was. */
static int to_markdown(void)
{
    int n, at;
    n = doc_to_text_at(&s_doc, s_file, FILE_MAX, s_cur, &at);
    if (n < 0 || !doc_from_plain(&s_tmp, s_file, n, at, &at)) {
        say("Too long to show as Markdown.", A_ACCENT);
        return 0;
    }
    s_doc = s_tmp;
    s_cur = at;
    return 1;
}

static int to_page(void)
{
    int n, at;
    n = doc_to_plain(&s_doc, s_file, FILE_MAX, s_cur, &at);
    if (n < 0) { say("Too long to read back.", A_ACCENT); return 0; }
    doc_from_text_at(&s_tmp, s_file, n, at, &at);
    s_doc = s_tmp;
    s_cur = at;
    return 1;
}

static void flip(void)
{
    if (s_md ? !to_page() : !to_markdown()) return;
    s_md = !s_md;
    s_anchor = -1;
    s_pen = 0;
    if (!s_md) pen_from_cursor();
    say(s_md ? "Markdown: type the marks. ^M for the page." : "The page.", A_DIM);
}

static void toggle(u8 attr)
{
    if (s_md) { md_toggle(attr); return; }
    if (have_sel()) {
        int a = sel_from(), b = sel_to(), k, all = 1;
        for (k = a; k < b; k++) if (s_doc.c[k].ch != '\n' && !(s_doc.c[k].at & attr)) all = 0;
        for (k = a; k < b; k++) {
            if (s_doc.c[k].ch == '\n') continue;
            if (all) s_doc.c[k].at &= (u8)~attr; else s_doc.c[k].at |= attr;
        }
        s_dirty = 1;
    } else {
        s_pen ^= attr;
    }
}

static void set_style(int st)
{
    int a = have_sel() ? sel_from() : s_cur, b = have_sel() ? sel_to() : s_cur, p;
    if (s_md) { md_style(st < 0 ? 0 : st, st < 0); return; }
    for (p = para_start(a); p <= b && p < s_doc.len; p = para_end(p) + 1)
        s_doc.c[para_end(p)].at = (u8)st;
    s_dirty = 1;
    say(s_style_name[st], A_DIM);
}

/* ---- files ------------------------------------------------------------------ */
static void draw(int blink);

static int load(const char *path)
{
    char m[INV_PATH + 16];
    int n = A->inv_read(path, s_file, FILE_MAX);
    if (n < 0) { say(A->inv_why(), A_ACCENT); return 0; }
    doc_from_text(&s_doc, s_file, n);
    s_cur = 0; s_anchor = -1; s_top = 0; s_dirty = 0;
    A->format(s_path, INV_PATH, "%s", path);
    if (s_md && !to_markdown()) s_md = 0;
    s_cur = 0;
    pen_from_cursor();
    A->format(m, sizeof m, "Opened %s.", path);
    say(m, A_DIM);
    return 1;
}

/* A name for a new document, typed on the bottom line.  It gets .MD if it
 * has no ending of its own.  0 if esc. */
static int ask_name(char *out)
{
    char name[13];
    int n = 0, key, mods, x, i, dot = 0;
    name[0] = 0;
    for (;;) {
        draw(0);
        A->win_band(H - 2, 1, W - 2, A_PAPER);
        x = A->win_text(2, H - 2, "Save as:", A_BRIGHT, A_PAPER);
        x = A->win_text(x + 1, H - 2, name, A_TEXT, A_HILITE);
        A->win_put(x, H - 2, '_', A_BRIGHT, A_HILITE, 0);
        A->win_text(x + 3, H - 2, "\1Enter\1 save  \1Esc\1 not now", A_DIM, A_PAPER);
        do { key = A->key(&mods); if (key < 0) A->pump(0); } while (key < 0);
        if (key == K_ESC) return 0;
        if (key == K_BKSP) { if (n) name[--n] = 0; continue; }
        if (key == K_ENTER) {
            if (!n) continue;
            for (i = 0, dot = 0; i < n; i++) dot |= name[i] == '.';
            if (slen(s_dir) + n + 5 >= INV_PATH) { say("That is too many folders deep.", A_ACCENT); return 0; }
            A->format(out, INV_PATH, "%s%s%s%s", s_dir, s_dir[0] ? "/" : "", name, dot ? "" : ".MD");
            return 1;
        }
        if (key >= 'a' && key <= 'z') key -= 32;
        if (n < 12 && ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9') || key == '.' || key == '_' || key == '-')) {
            name[n++] = (char)key;
            name[n] = 0;
        }
    }
}

static int save(void)
{
    char path[INV_PATH], m[INV_PATH + 32];
    InvItem it;
    int n;

    n = s_md ? doc_to_plain(&s_doc, s_file, FILE_MAX, -1, 0)
             : doc_to_text(&s_doc, s_file, FILE_MAX);
    if (n < 0) {
        A->format(m, sizeof m, "Too long to keep: a document holds %u bytes.", (u32)FILE_MAX);
        say(m, A_ACCENT);
        return 0;
    }
    if (s_path[0]) A->format(path, INV_PATH, "%s", s_path);
    else {
        if (!ask_name(path)) { if (!s_msg[0]) say("Not saved.", A_DIM); return 0; }
        if (A->inv_stat(path, &it)) { say("There is already something by that name. Pick another.", A_ACCENT); return 0; }
    }
    say("Writing to the floppy...", A_DIM);
    draw(0);
    A->pump(1);
    if (!A->inv_write(path, s_file, (u32)n)) { say(A->inv_why(), A_ACCENT); return 0; }
    A->format(s_path, INV_PATH, "%s", path);
    s_dirty = 0;
    A->format(m, sizeof m, "Saved as %s, %u bytes.", path, (u32)n);
    say(m, A_BRIGHT);
    A->format(m, sizeof m, "[scribe] saved %s %u\n", path, (u32)n);
    A->wire(m);
    return 1;
}

/* ---- drawing ---------------------------------------------------------------- */
static void doc_name(char *out, int cap)
{
    int e = para_end(0), k = s_md ? md_prefix(0, 0) : 0, n = 0;
    for (; k < e && n < cap - 1; k++) {
        if (s_md && cells_are(k, e, "<u>"))  { k += 2; continue; }
        if (s_md && cells_are(k, e, "</u>")) { k += 3; continue; }
        if (!s_md || (s_doc.c[k].ch != '*' && s_doc.c[k].ch != '\\')) out[n++] = (char)s_doc.c[k].ch;
    }
    out[n] = 0;
    if (!n) A->format(out, (u32)cap, "%s", "untitled");
}

static void put_hint(int y, const char *t)
{
    int n = 0, i;
    for (i = 0; t[i]; i++) if (t[i] != 1) n++;   /* \1 marks a key, in pink; no room */
    if (n < W - 4) A->win_text((W - n) / 2, y, t, A_DIM, A_PAPER);
}

static void draw(int blink)
{
    char name[40], m[48];
    int x, y, i, cl, a = -1, b = -1;
    u8 pp = A_PAPER;

    A->win_open(A->screen_cols(), A->screen_rows());
    A->win_frame(0, A_GREY);

    doc_name(name, (int)sizeof name);
    x = A->win_text(2, 0, " scribe ", A_BRIGHT, pp);
    x = A->win_text(x, 0, A->os_version(), A_DIM, pp);   /* which FLOPOS it came with */
    x = A->win_text(x, 0, " ", A_GREY, pp);
    A->win_put(x++, 0, A_FR_H, A_GREY, pp, 0);
    x = A->win_text(x, 0, " ", A_GREY, pp);
    for (i = 0; name[i] && x < W - 6; i++) A->win_put(x++, 0, (u8)name[i], A_TEXT, pp, 0);
    if (s_dirty) A->win_put(x++, 0, '*', A_ACCENT, pp, 0);
    A->win_put(x, 0, ' ', A_GREY, pp, 0);

    /* The toolbar: what typing will look like now, and this paragraph's style. */
    {
        static const char *lab[3] = { "B", "I", "U" };
        static const u8 bit[3] = { AT_BOLD, AT_ITALIC, AT_UNDERLINE };
        u8 cur_at = s_md ? (u8)(s_cur > 0 && s_doc.c[s_cur - 1].ch != '\n' ? s_look_own[s_cur - 1] : 0) : s_pen;
        for (i = 0; i < 3; i++) {
            int on = cur_at & bit[i];
            u8 bg = on ? A_HILITE : pp;
            A->win_put(2 + i * 4, 1, '[', A_DIM, bg, 0);
            A->win_put(3 + i * 4, 1, (u8)lab[i][0], on ? A_BRIGHT : A_DIM, bg, bit[i]);
            A->win_put(4 + i * 4, 1, ']', A_DIM, bg, 0);
        }
        if (s_md) {
            int st;
            md_prefix(para_start(s_cur), &st);
            A->win_text(15, 1, s_style_name[st], A_BRIGHT, pp);
            A->win_text(W - 12, 1, "Markdown", A_ACCENT, pp);
        } else
            A->win_text(15, 1, s_style_name[s_doc.c[para_end(s_cur)].at % ST_COUNT],
                        A_BRIGHT, pp);
    }
    for (x = 1; x < W - 1; x++) { A->win_put(x, 2, A_FR_H, A_GREY, pp, 0); A->win_put(x, H - 3, A_FR_H, A_GREY, pp, 0); }
    A->win_put(0, 2, A_FR_TEE_L, A_GREY, pp, 0);     A->win_put(W - 1, 2, A_FR_TEE_R, A_GREY, pp, 0);
    A->win_put(0, H - 3, A_FR_TEE_L, A_GREY, pp, 0); A->win_put(W - 1, H - 3, A_FR_TEE_R, A_GREY, pp, 0);

    /* The text. */
    if (have_sel()) { a = sel_from(); b = sel_to(); }
    for (y = 0; y < TH && s_top + y < s_nlines; y++) {
        Line *L = &s_line[s_top + y];
        u8 fg = style_fg(L->style), sat = style_at(L->style);
        for (i = 0; i < L->len; i++) {
            int pos = L->start + i;
            DCell *c = &s_doc.c[pos];
            int sel = pos >= a && pos < b;
            u8 cfg = s_md ? s_look_fg[pos] : fg;
            u8 cat = s_md ? s_look_at[pos] : (u8)((c->at & ATTRS) | sat);
            A->win_put(2 + L->x0 + i, 3 + y, c->ch, sel ? A_BRIGHT : cfg,
                       sel ? A_SELECT : pp, cat);
        }
    }

    /* The cursor: the cell under it lit, blinking. */
    cl = line_of(s_cur);
    if (blink && cl >= s_top && cl < s_top + TH) {
        Line *L = &s_line[cl];
        int cx = 2 + L->x0 + (s_cur - L->start);
        u8 ch = s_doc.c[s_cur].ch == '\n' ? ' ' : s_doc.c[s_cur].ch;
        if (cx > W - 2) cx = W - 2;
        A->win_put(cx, 3 + cl - s_top, ch, A_PAPER, A_BRIGHT, 0);
    }

    /* Status. */
    if (s_msg[0]) A->win_text(2, H - 2, s_msg, s_msg_col, pp);
    {
        int from = slen(s_path) > 30 ? slen(s_path) - 30 : 0;     /* the end of a long path */
        if (s_path[0]) A->format(m, sizeof m, "%s", s_path + from);
        else           A->format(m, sizeof m, "not saved yet");
    }
    A->win_text(W - 2 - slen(m), H - 2, m, A_DIM, pp);

    if (s_md)
        put_hint(H - 1, W >= 76 ? " **bold**  *italic*  <u>underline</u>  # heading  % title  \1^M\1 the page "
                                : " **bold** *italic* <u>under</u>  # head  % title  \1^M\1 page ");
    else
        put_hint(H - 1, W >= 84 ? " \1^B\1 bold  \1^I\1 italic  \1^U\1 underline  \1^P\1 style  \1^M\1 markdown  \1^S\1 save  \1^O\1 open  \1esc\1 "
                                : " \1^B\1 \1^I\1 \1^U\1  \1^P\1 style  \1^M\1 markdown  \1^S\1 save  \1^O\1 open  \1esc\1 ");
}

static void keep_cursor_visible(void)
{
    int cl = line_of(s_cur);
    if (cl < s_top) s_top = cl;
    if (cl >= s_top + TH) s_top = cl - TH + 1;
    if (s_top < 0) s_top = 0;
}

/* ---- the loop ---------------------------------------------------------------- */
static void move_to(int pos, int extend)
{
    if (extend) { if (s_anchor < 0) s_anchor = s_cur; }
    else s_anchor = -1;
    if (pos < 0) pos = 0;
    if (pos > s_doc.len - 1) pos = s_doc.len - 1;
    s_cur = pos;
    pen_from_cursor();
}

static void vertical(int by, int extend)
{
    int cl = line_of(s_cur), tl = cl + by;
    Line *L = &s_line[cl];
    if (s_want < 0) s_want = L->x0 + (s_cur - L->start);
    if (tl < 0) tl = 0;
    if (tl > s_nlines - 1) tl = s_nlines - 1;
    move_to(pos_at(tl, s_want), extend);
}

int app_main(const Api *api, int argc, char **argv)
{
    int key, mods, changed = 1, blink = -1;

    A = api;
    A->win_open(A->screen_cols(), A->screen_rows());
    W = A->win_width(); H = A->win_height();
    TW = W - 4; TH = H - 6;

    doc_empty(&s_doc);
    s_cur = 0; s_anchor = -1; s_dirty = 0; s_pen = 0; s_top = 0;
    s_path[0] = s_dir[0] = 0;
    say("A new document.", A_DIM);

    /* `scribe NAME`: that document, or a new one that will be saved there.
     * `scribe FOLDER/` (the inventory's "write new"): a new one for that
     * folder.  A third word, "back", is the inventory saying it wants to
     * be returned to. */
    s_back = argc >= 3 && argv[2][0] == 'b';
    if (argc >= 2 && argv[1][0]) {
        InvItem it;
        int i, n = slen(argv[1]), cut = -1, found;
        char arg[INV_PATH];
        if (n >= INV_PATH) n = INV_PATH - 1;
        for (i = 0; i < n; i++) arg[i] = (char)(argv[1][i] >= 'a' && argv[1][i] <= 'z' ? argv[1][i] - 32 : argv[1][i]);
        arg[n] = 0;
        while (n && arg[n - 1] == '/') arg[--n] = 0;
        found = n ? A->inv_stat(arg, &it) : 0;
        if (n && !found && n + 4 < INV_PATH) {       /* no ending given: a document's */
            int dot = 0;
            for (i = n; i > 0 && arg[i - 1] != '/'; i--) dot |= arg[i - 1] == '.';
            if (!dot) {
                arg[n] = '.'; arg[n + 1] = 'M'; arg[n + 2] = 'D'; arg[n + 3] = 0;
                n += 3;
                found = A->inv_stat(arg, &it);
            }
        }
        if (!n || (found && it.folder) || argv[1][slen(argv[1]) - 1] == '/') {
            A->format(s_dir, INV_PATH, "%s", arg);          /* a new document, for this folder */
        } else {
            for (i = 0; i < n; i++) if (arg[i] == '/') cut = i;
            for (i = 0; i < cut; i++) s_dir[i] = arg[i];
            s_dir[cut < 0 ? 0 : cut] = 0;
            if (found) load(arg);
            else {
                char m[INV_PATH + 40];
                A->format(s_path, INV_PATH, "%s", arg);
                A->format(m, sizeof m, "A new document; ^S saves it as %s.", arg);
                say(m, A_DIM);
            }
        }
    }

    for (;;) {
        int b = (int)((A->ms() / 450) & 1) == 0;
        if (changed) { layout(); keep_cursor_visible(); }
        if (changed || b != blink) { draw(b); blink = b; changed = 0; }

        key = A->key(&mods);
        if (key < 0) { A->pump(0); continue; }
        changed = 1;
        if (key != K_ESC) s_esc = 0;
        if (key != K_UP && key != K_DOWN && key != K_PGUP && key != K_PGDN) s_want = -1;

        if (mods & M_CTRL) {
            switch (key) {
            case 'b': toggle(AT_BOLD); break;
            case 'i': toggle(AT_ITALIC); break;
            case 'u': toggle(AT_UNDERLINE); break;
            case 'p': set_style(s_md ? -1 : (s_doc.c[para_end(s_cur)].at + 1) % ST_COUNT); break;
            case 'm': flip(); break;
            case 't': set_style(ST_TITLE); break;
            case '0': set_style(ST_BODY); break;
            case '1': set_style(ST_H1); break;
            case '2': set_style(ST_H2); break;
            case '3': set_style(ST_H3); break;
            case '4': set_style(ST_H4); break;
            case 's': save(); break;
            case 'o': {
                char m[INV_PATH + 16];
                if (s_dirty) { say("Save this first (^S), or ^N to throw it away.", A_ACCENT); break; }
                /* The inventory is where documents are picked from; it comes
                 * back here with the one chosen. */
                A->format(m, sizeof m, "inventory %s", s_path[0] ? s_path : s_dir);
                A->then(m);
                A->win_close();
                return 0;
            }
            case 'n':
                if (s_dirty && s_esc != 2) { say("Unsaved. ^N again throws it away.", A_ACCENT); s_esc = 2; break; }
                doc_empty(&s_doc);
                s_cur = 0; s_anchor = -1; s_dirty = 0; s_pen = 0; s_top = 0; s_esc = 0;
                s_path[0] = 0;
                say("A new document.", A_DIM);
                break;
            default: break;
            }
            continue;
        }

        switch (key) {
        case K_ESC:
            if (s_dirty && !s_esc) { say("Unsaved. ^S saves; esc again leaves without saving.", A_ACCENT); s_esc = 1; break; }
            if (s_back) {                        /* back to the inventory it was opened from */
                char m[INV_PATH + 16];
                A->format(m, sizeof m, "inventory %s", s_path[0] ? s_path : s_dir);
                A->then(m);
            }
            A->win_close();
            return 0;
        case K_LEFT:  move_to(s_cur - 1, mods & M_SHIFT); break;
        case K_RIGHT: move_to(s_cur + 1, mods & M_SHIFT); break;
        case K_UP:    vertical(-1, mods & M_SHIFT); break;
        case K_DOWN:  vertical(+1, mods & M_SHIFT); break;
        case K_PGUP:  vertical(-(TH - 1), mods & M_SHIFT); break;
        case K_PGDN:  vertical(TH - 1, mods & M_SHIFT); break;
        case K_HOME:  move_to(s_line[line_of(s_cur)].start, mods & M_SHIFT); break;
        case K_END: {
            Line *L = &s_line[line_of(s_cur)];
            int end = L->start + L->len;
            move_to(s_doc.c[end].ch == '\n' ? end : end - 1, mods & M_SHIFT);
            break;
        }
        case K_ENTER: new_paragraph(); break;
        case K_BKSP:
            if (have_sel()) remove_range(sel_from(), sel_to());
            else if (s_cur > 0) remove_range(s_cur - 1, s_cur);
            pen_from_cursor();
            break;
        case K_DEL:
            if (have_sel()) remove_range(sel_from(), sel_to());
            else remove_range(s_cur, s_cur + 1);
            break;
        case K_TAB: { int k; for (k = 0; k < 4; k++) insert(' ', s_pen); break; }
        default:
            if ((key >= 32 && key < 127) || (key >= 0x80 && key <= 0x88)) insert((u8)key, s_pen);
            else changed = 0;
            break;
        }
    }
}
