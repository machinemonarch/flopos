/* ===========================================================================
 * term.c -- the terminal, drawn by hand, on whichever grid is in use.
 *
 * The face is a runtime choice, not a build-time one: the cell grid is
 * always allocated at the largest face's width and only as much of each
 * row is shown as the current face has columns.  So changing face costs
 * nothing and loses nothing, which matters because the only way to find
 * out how small text can get on a 1996 panel is to sit in front of one.
 *
 * A ring of cells holds both the visible screen and the scrollback, each
 * cell a character and a palette index.  The whole visible window is
 * redrawn every frame over whatever the background is doing, because the
 * background is always doing something.  Blank cells are skipped, which is
 * most of them, so a full redraw costs far less than the worst case.
 *
 * Every glyph gets a one-pixel drop shadow.  That single detail is what
 * keeps text readable against a moving field of colour, and it is not
 * optional on a laptop panel.
 * ===========================================================================
 */
#include "flopos.h"
#include "io.h"

#define SCROLLBACK 200
/* Wide enough for the densest face on the biggest screen, since the grid
 * is allocated once and the screen can change under it. */
#define STRIDE     100

typedef struct {
    u8 ch;
    u8 col;
} Cell;

static const Face *s_face = &g_faces[1];   /* close, by default      */
static Cell *s_grid;                     /* SCROLLBACK rows of STRIDE      */
static u32   s_line;                     /* absolute row the cursor is on  */
static int   s_col;
static u8    s_colour = C_TEXT;
static int   s_view;                     /* rows scrolled back, 0 = bottom */
static int   s_zalgo;
static u8    s_style[SCROLLBACK];        /* one quirk per line of history  */
static int   s_quirks = 1;
static int   s_mute;                     /* say it down the wire only      */
static int   s_nocursor;                 /* nothing to type into here      */

static void style_birth(u32 abs);

/* Where the picture currently is and how big a glyph is drawn.  All of it
 * can change while the machine is running, so none of it is a constant. */
static int s_pitch = SCR_W, s_vw = SCR_W, s_vh = SCR_H, s_scale = 1;

/* Every glyph again at double size, built once.  Doing the smoothing per
 * glyph per frame would be a million comparisons a second for a picture
 * that never changes; doing it once costs two kilobytes. */
static u16 s_big[FONT_GLYPHS][10];
static int s_big_for = -1;

static void build_big(void)
{
    const Face *f = s_face;
    int w = f->w, h = f->h, g, col, row;

    for (g = 0; g < FONT_GLYPHS; g++) {
        const u8 *src = f->glyphs + g * w;
        for (col = 0; col < w * 2; col++) s_big[g][col] = 0;

        for (col = 0; col < w; col++) {
            for (row = 0; row < h; row++) {
#define ON(cx, cy) ((cx) >= 0 && (cx) < w && (cy) >= 0 && (cy) < h && \
                    ((src[cx] >> (cy)) & 1))
                int p = ON(col, row);
                int a = ON(col, row - 1), b = ON(col + 1, row);
                int c = ON(col - 1, row), d = ON(col, row + 1);
                int e0 = p, e1 = p, e2 = p, e3 = p;
#undef ON
                /* Where a diagonal steps, fill the inside corner.  Blowing
                 * a letter up without this just makes bigger staircases. */
                if (c == a && c != d && a != b) e0 = a;
                if (a == b && a != c && b != d) e1 = b;
                if (d == c && d != b && c != a) e2 = c;
                if (b == d && b != a && d != c) e3 = d;

                if (e0) s_big[g][col * 2]     |= (u16)(1u << (row * 2));
                if (e1) s_big[g][col * 2 + 1] |= (u16)(1u << (row * 2));
                if (e2) s_big[g][col * 2]     |= (u16)(1u << (row * 2 + 1));
                if (e3) s_big[g][col * 2 + 1] |= (u16)(1u << (row * 2 + 1));
            }
        }
    }
    s_big_for = (int)(s_face - &g_faces[0]);
}

static void geometry(void)
{
    s_pitch = vga_pitch();
    s_vw    = vga_width();
    s_vh    = vga_height();
    s_scale = vga_scale();
    if (s_scale > 1 && s_big_for != (int)(s_face - &g_faces[0])) build_big();
}

static int cols_now(void)
{
    int n = (s_vw - s_face->x0 * s_scale) / (s_face->adv * s_scale);
    return n > STRIDE ? STRIDE : n;
}

static int rows_now(void)
{
    int n = s_vh / (s_face->pitch * s_scale);
    return n > SCROLLBACK ? SCROLLBACK : n;
}

#define COLS cols_now()
#define ROWS rows_now()

const Face *term_face(void) { return s_face; }
int  term_cols(void) { geometry(); return COLS; }
int  term_rows(void) { geometry(); return ROWS; }
int  term_face_index(void) { return (int)(s_face - &g_faces[0]); }

int term_set_face(int n)
{
    if (n < 0) n = 0;
    if (n >= FACE_COUNT) n = FACE_COUNT - 1;
    s_face = &g_faces[n];
    geometry();
    if (s_scale > 1) build_big();
    if (s_col > COLS) s_col = COLS;
    s_view = 0;
    return n;
}

static Cell *row_of(u32 abs)
{
    return s_grid + (abs % SCROLLBACK) * STRIDE;
}

static void clear_row(u32 abs)
{
    Cell *r = row_of(abs);
    int i;
    for (i = 0; i < STRIDE; i++) { r[i].ch = ' '; r[i].col = 0; }
}

void term_init(void)
{
    u32 i;
    s_grid = (Cell *)halloc(SCROLLBACK * STRIDE * sizeof(Cell));
    for (i = 0; i < SCROLLBACK; i++) clear_row(i);
    s_line = 0;
    s_col = 0;
    s_view = 0;
    s_colour = C_TEXT;
    style_birth(0);
}

void term_clear(void)
{
    u32 i;
    for (i = 0; i < SCROLLBACK; i++) clear_row(i);
    s_line = 0;
    s_col = 0;
    s_view = 0;
    style_birth(0);
}

void term_colour(u8 c) { s_colour = c; }

void term_zalgo(int on) { s_zalgo = on ? 1 : 0; }
int  term_zalgo_on(void) { return s_zalgo; }

/* ---- every line is born with an affliction ----------------------------
 * A line is given its treatment at the moment it comes into existence,
 * not when it is drawn, so it keeps the same one for as long as it stays
 * in the scrollback.  Styles that move are animated off the frame clock
 * rather than re-rolled, or the screen would simply seethe.
 */
#define ST_PLAIN   0
#define ST_GLITCH  1
#define ST_SHINE   2
#define ST_BOUNCE  3
#define ST_MONO    4
#define ST_RUNES   5
#define ST_SHIVER  6
#define ST_FADE    7
#define ST_COUNT   8

/* Text that came from somewhere else -- a web page -- is for reading, so
 * lines born while this is on are plain whatever the quirks are doing. */
static int s_plain;
void term_plain(int on) { s_plain = on; }

static void style_birth(u32 abs)
{
    u32 r = rnd();
    if (s_plain) { s_style[abs % SCROLLBACK] = ST_PLAIN; return; }
    /* Plain is weighted heavily. Every line being peculiar is just noise;
     * it is the ordinary ones in between that make the rest land. */
    s_style[abs % SCROLLBACK] =
        (u8)(((r >> 3) & 1) ? ST_PLAIN : (r % ST_COUNT));
}

void term_quirks(int on) { s_quirks = on ? 1 : 0; }
int  term_quirks_on(void) { return s_quirks; }

static void (*s_tap)(int c);              /* the HUD, while a command of its runs */
void term_tap(void (*fn)(int c)) { s_tap = fn; }

void term_newline(void)
{
    if (s_tap) s_tap('\n');
    /* The mirror lives here rather than in term_putc, because almost
     * nothing writes a newline as a character -- they call this. */
    if (serial_echo()) serial_putc('\n');
    if (s_mute) return;

    s_line++;
    s_col = 0;
    s_view = 0;                          /* any output returns to the end  */
    clear_row(s_line);
    style_birth(s_line);
}

/* While muted, everything still goes down the wire and nothing lands on
 * the screen.  The report is a page of hex and register values written
 * for me, and there is no reason to make somebody sit and watch it
 * scroll past on the machine itself. */
void term_mute(int on) { s_mute = on; }
int  term_muted(void) { return s_mute; }
void term_cursor(int on) { s_nocursor = !on; }

/* Hidden, only windows are drawn: the shell, locked behind the wizard. */
static int s_hidden;
void term_hide(int on) { s_hidden = on; }

void term_putc(int c)
{
    Cell *r;

    if (c == '\n') { term_newline(); return; }   /* mirrors on its own */
    if (s_tap) s_tap(c);

    /* Everything FLOPOS says to itself, it also says down the wire, so a
     * session can be watched live from another machine. */
    if (serial_echo()) serial_putc(c);

    if (s_mute) return;                  /* the report is not for here     */

    if (c == '\r') { s_col = 0; return; }
    if (c == '\t') {
        do { term_putc(' '); } while (s_col & 7);
        return;
    }
    if (s_col >= COLS) term_newline();

    r = row_of(s_line);
    r[s_col].ch  = (u8)c;
    r[s_col].col = s_colour;
    s_col++;
    s_view = 0;
}

void term_print(const char *s)
{
    while (*s) term_putc((u8)*s++);
}

void term_printf(const char *fmt, ...)
{
    char buf[192];
    __builtin_va_list ap;

    __builtin_va_start(ap, fmt);
    fmt_vstr(buf, sizeof buf, fmt, ap);
    __builtin_va_end(ap);
    term_print(buf);
}

int  term_cursor_col(void) { return s_col; }

/* True when nothing has been written since the last clear, so callers can
 * avoid opening with a blank line on an empty screen. */
int  term_at_origin(void) { return s_line == 0 && s_col == 0; }

void term_set_cursor_col(int c)
{
    if (c < 0) c = 0;
    if (c > COLS) c = COLS;
    s_col = c;
}

void term_rule(void)
{
    int i;
    u8 save = s_colour;
    s_colour = C_GREY;
    for (i = 0; i < COLS - 2; i++) term_putc(FR_H);
    s_colour = save;
    term_newline();
}

/* ---- frames ----------------------------------------------------------- *
 * The terminal is a stream, not a surface, so a frame here is three calls
 * that print their own rows into it rather than something drawn at a
 * coordinate.  That is enough to put a border round a block of text, which
 * is where every text interface starts, and it needs no cursor addressing
 * to go wrong.
 */
static void frame_run(int w, int left, int mid, int right)
{
    int i;
    term_putc(left);
    for (i = 0; i < w - 2; i++) term_putc(mid);
    term_putc(right);
}

void term_frame_top(int w, const char *title, int dbl)
{
    int base = dbl ? CH_FRAME2 : CH_FRAME;
    int i, n = 0;

    if (w > COLS) w = COLS;
    if (w < 4) w = 4;
    if (title) while (title[n]) n++;
    if (n > w - 6) n = w - 6;

    term_putc(base | FR_DOWN | FR_RIGHT);
    if (n > 0) {
        term_putc(base | FR_LEFT | FR_RIGHT);
        term_putc(' ');
        for (i = 0; i < n; i++) term_putc((u8)title[i]);
        term_putc(' ');
    }
    for (i = 0; i < w - 2 - (n > 0 ? n + 3 : 0); i++)
        term_putc(base | FR_LEFT | FR_RIGHT);
    term_putc(base | FR_DOWN | FR_LEFT);
    term_newline();
}

void term_frame_row(int w, const char *text, int dbl)
{
    int base = dbl ? CH_FRAME2 : CH_FRAME;
    int i, n = 0;

    if (w > COLS) w = COLS;
    if (w < 4) w = 4;
    if (text) while (text[n]) n++;
    if (n > w - 4) n = w - 4;

    term_putc(base | FR_UP | FR_DOWN);
    term_putc(' ');
    for (i = 0; i < n; i++) term_putc((u8)text[i]);
    for (i = n; i < w - 4; i++) term_putc(' ');
    term_putc(' ');
    term_putc(base | FR_UP | FR_DOWN);
    term_newline();
}

void term_frame_bottom(int w, int dbl)
{
    int base = dbl ? CH_FRAME2 : CH_FRAME;
    if (w > COLS) w = COLS;
    if (w < 4) w = 4;
    frame_run(w, base | FR_UP | FR_RIGHT,
                 base | FR_LEFT | FR_RIGHT,
                 base | FR_UP | FR_LEFT);
    term_newline();
}

/* ---- scrollback ------------------------------------------------------- */
void term_scroll_view(int lines)
{
    int max = (int)(s_line > (u32)(SCROLLBACK - ROWS)
                    ? (u32)(SCROLLBACK - ROWS) : s_line);
    s_view += lines;
    if (s_view < 0) s_view = 0;
    if (s_view > max) s_view = max;
}

void term_scroll_home(void) { s_view = 0; }
int  term_scrolled(void) { return s_view; }

/* ---- the input line lives in the terminal's current row ---------------
 * The shell writes a prompt, remembers the column it stopped at, and from
 * then on this rewrites everything to the right of it from the line editor
 * on every frame.  That way the input is part of the scrollback the moment
 * it is committed, and there is only one renderer.
 */
void term_input_line(int from_col, const char *s, int caret, u8 colour)
{
    Cell *r = row_of(s_line);
    int avail = COLS - from_col;
    int len = 0, off = 0, i;

    if (avail <= 0) return;
    while (s[len]) len++;

    /* Slide the window so the caret stays on screen on a long line. */
    if (caret >= avail) off = caret - avail + 1;

    for (i = 0; i < avail; i++) {
        int src = off + i;
        r[from_col + i].ch  = (src < len) ? (u8)s[src] : ' ';
        r[from_col + i].col = (src < len) ? colour : 0;
    }
    s_col = from_col + (caret - off);
    if (s_col > COLS) s_col = COLS;
}


/* What accretes around a character once the place has turned.
 *
 * The glyph itself is never drawn differently -- that is the whole
 * difference between this and illegibility.  Everything here grows into
 * the space between the lines and over its neighbours: strokes rather
 * than specks, several per character, some of them long enough to reach
 * into the row above or below.
 *
 * Every mark is derived from the character, where it sits, and a phase
 * that turns over about twice a second.  So it writhes slowly rather than
 * seething at frame rate, which is both far more unpleasant to look at
 * and much easier to read through.
 */
static void zalgo_marks(u8 *dst, int x, int y, int ch, int col, int row, u32 t)
{
    u32 h;
    int n, i;

    if (ch == ' ' || ch == 0) return;

    h = (u32)(ch * 2654435761u) ^ (u32)(col * 40503u)
      ^ (u32)(row * 2246822519u);
    h ^= (t >> 9) * 2654435761u;         /* the slow writhe                */

    n = 4 + (int)(h & 7);

    for (i = 0; i < n; i++) {
        int mx, up, d, len, k;
        u8 ink;

        h = h * 1664525u + 1013904223u;
        mx  = x + (int)((h >> 7) % (u32)s_face->w);
        up  = (int)(h & 1);
        d   = 1 + (int)((h >> 9) % 7);
        len = 1 + (int)((h >> 13) % 3);
        if (((h >> 17) & 15) == 0) len += 6;   /* occasionally, a tendril */
        /* Text-weight ink, not shadow.  Once the place has turned, the
         * palette is its own negative and the shadow colour comes back as
         * near-white -- which makes every mark vanish against the lighter
         * parts of the screen.  These two invert to something that still
         * contrasts wherever it lands. */
        ink = ((h >> 21) & 3) ? C_DIM : C_TEXT;

        if (mx < 0 || mx >= s_pitch) continue;
        for (k = 0; k < len; k++) {
            int my = up ? (y - d - k) : (y + s_face->pitch - 1 + d + k);
            if (my < 0 || my >= s_vh) break;
            dst[my * s_pitch + mx] = ink;
        }
    }
}

/* An alphabet of our own.
 *
 * A line in this style is not translated into anything -- the letters are
 * all still there, in order.  They are simply written in a script this
 * machine made up: a stem with ticks hung off it, the pattern of ticks
 * decided by which letter it is.  Consistent enough to work out, foreign
 * enough to stop you reading it by accident.
 */
static void rune_cols(int letter, u8 *cols)
{
    int w = s_face->w, h = s_face->h;
    int mid = w / 2;
    int t = 1, b = h - 3;                /* the stem's extent              */
    int md = (t + b) / 2;
    int l1 = mid - 1, l2 = mid >= 2 ? mid - 2 : 0;
    int r1 = mid + 1, r2 = mid + 2 < w ? mid + 2 : w - 1;
    int i, k;

    for (i = 0; i < w; i++) cols[i] = 0;
    for (k = t; k <= b; k++) cols[mid] |= (u8)(1 << k);

    if (letter & 1)  { cols[l1] |= (u8)(1 << md); cols[l2] |= (u8)(1 << (md + 1)); }
    if (letter & 2)  { cols[r1] |= (u8)(1 << md); cols[r2] |= (u8)(1 << (md - 1)); }
    if (letter & 4)  { cols[l1] |= (u8)(1 << b);  cols[l2] |= (u8)(1 << b); }
    if (letter & 8)  { cols[r1] |= (u8)(1 << t);  cols[r2] |= (u8)(1 << t); }
    if (letter & 16) { cols[l1] |= (u8)(1 << t);  cols[r1] |= (u8)(1 << t); }
    if (letter >= 16) {                  /* a longer stem for the back half */
        cols[mid] |= (u8)(1 << (t - 1 > 0 ? t - 1 : 0));
        cols[mid] |= (u8)(1 << (b + 1 < h ? b + 1 : h - 1));
    }
}

static void blit_cols(u8 *dst, int x, int y, const u8 *g, u8 colour)
{
    int col, row, sx, sy, h = s_face->h;

    for (col = 0; col < s_face->w; col++) {
        u8 bits = g[col];
        for (row = 0; row < h; row++) {
            if (!((bits >> row) & 1)) continue;
            for (sy = 0; sy < s_scale; sy++) {
                int py = y + row * s_scale + sy;
                if (py < 0 || py >= s_vh) continue;
                for (sx = 0; sx < s_scale; sx++) {
                    int px = x + col * s_scale + sx;
                    if (px < 0 || px >= s_vw) continue;
                    dst[py * s_pitch + px] = colour;
                }
            }
        }
    }
}

/* Work out how one cell of a line is to be drawn.  Returns 1 if the glyph
 * has been replaced wholesale and sits in runes. */
static int style_cell(u8 st, int c, u32 abs, u32 t,
                      int *ch, u8 *colour, int *dx, int *dy, u8 *runes)
{
    u32 h;

    switch (st) {
    case ST_GLITCH:
        /* Runs of the line tear sideways, and re-tear a few times a
         * second rather than every frame. */
        h = (u32)((c >> 2) * 2654435761u) ^ (abs * 40503u)
          ^ ((t >> 7) * 2246822519u);
        if ((h & 7) == 0) {
            *dx = (int)((h >> 3) & 3) - 1;
            *colour = (u8)(PAL_ACCENT + ((h >> 5) & 15) + 8);
        }
        break;

    case ST_SHINE:
        *colour = (u8)(PAL_BRIGHT + (((c * 3) + (int)(t >> 5)) & (PAL_BRIGHT_N - 1)));
        break;

    case ST_BOUNCE: {
        int ph = (int)((t >> 3) + (u32)(c & 1) * (SINLEN / 2));
        *dy = (SIN(ph) * 2) >> 12;
        break;
    }

    case ST_MONO:
        *colour = (u8)(PAL_GREY + PAL_GREY_N - 4);
        break;

    case ST_RUNES:
        if (*ch >= 'a' && *ch <= 'z') { rune_cols(*ch - 'a', runes); return 1; }
        if (*ch >= 'A' && *ch <= 'Z') { rune_cols(*ch - 'A', runes); return 1; }
        break;

    case ST_SHIVER:
        h = (abs * 2654435761u) ^ ((t >> 6) * 40503u);
        *dx = (int)(h & 1) - (int)((h >> 1) & 1);
        *dy = (int)((h >> 2) & 1) - (int)((h >> 3) & 1);
        break;

    case ST_FADE: {
        int k = (PAL_TEXT_N - 1) - (c * (PAL_TEXT_N - 1)) / COLS;
        if (k < 2) k = 2;
        *colour = (u8)(PAL_TEXT + k);
        break;
    }

    default:
        break;
    }
    return 0;
}

/* Frames shine: every frame piece, whoever draws it, is coloured from a
 * loop of sixteen neon colours by where it is on the screen (down the
 * diagonal, so neighbouring cells join), with every other pixel taken
 * half a band on, which blends the bands.  The loop itself turns in the
 * palette (mood_neon), so the shimmer costs no drawing at all. */
#define NEON(x, y) ((u8)(PAL_NEON + ((((x) + (y) + ((((x) ^ (y)) & 1) << 3)) >> 4) & (PAL_NEON_N - 1))))

static void hrun(u8 *dst, int x0, int x1, int y, u8 colour)
{
    (void)colour;
    if (y < 0 || y >= s_vh) return;
    if (x0 < 0) x0 = 0;
    if (x1 >= s_pitch) x1 = s_pitch - 1;
    for (; x0 <= x1; x0++) dst[y * s_pitch + x0] = NEON(x0, y);
}

static void vrun(u8 *dst, int x, int y0, int y1, u8 colour)
{
    (void)colour;
    if (x < 0 || x >= s_pitch) return;
    if (y0 < 0) y0 = 0;
    if (y1 >= s_vh) y1 = s_vh - 1;
    for (; y0 <= y1; y0++) dst[y0 * s_pitch + x] = NEON(x, y0);
}

/* Frames, rules and shading are drawn to the cell instead of being carried
 * in a glyph.  A stroke the width of the ink cannot reach the stroke in the
 * next cell, and a frame with a seam every few pixels is not a frame; and
 * doing it as geometry means every face gets frames for free, at whatever
 * size it happens to be, with no art to redraw.
 *
 * The low four bits say which arms the cell puts out, so a corner is two
 * of them, a tee is three, and there is no table to keep straight. */
static void blit_cell(u8 *dst, int x, int y, int ch, u8 colour)
{
    int w = s_face->adv * s_scale, h = s_face->pitch * s_scale;
    int mx = x + (w - 1) / 2, my = y + (h - 1) / 2;
    int m = ch & 15;
    int px, py;

    if (ch >= CH_SHADE1) {
        for (py = 0; py < h; py++) {
            int ay = y + py;
            if (ay < 0 || ay >= s_vh) continue;
            for (px = 0; px < w; px++) {
                int ax = x + px, on;
                if (ax < 0 || ax >= s_pitch) continue;
                /* Keyed to absolute position, not to the cell, so that
                 * neighbouring cells of the same shade tile seamlessly. */
                switch (ch) {
                case CH_SHADE1: on = !(ax & 1) && !(ay & 1);  break;
                case CH_SHADE2: on = ((ax ^ ay) & 1) == 0;    break;
                case CH_SHADE3: on = (ax & 1) || (ay & 1);    break;
                default:        on = 1;                       break;
                }
                if (on) dst[ay * s_pitch + ax] = colour;
            }
        }
        return;
    }

    if ((ch & 0xF0) == CH_FRAME2) {
        /* Two strokes a pixel either side of where the single one goes.
         * Each runs a pixel past the centre, so a corner closes instead of
         * leaving its elbow hanging open. */
        if (m & FR_LEFT)  { hrun(dst, x, mx + 1, my - 1, colour);
                            hrun(dst, x, mx + 1, my + 1, colour); }
        if (m & FR_RIGHT) { hrun(dst, mx - 1, x + w - 1, my - 1, colour);
                            hrun(dst, mx - 1, x + w - 1, my + 1, colour); }
        if (m & FR_UP)    { vrun(dst, mx - 1, y, my + 1, colour);
                            vrun(dst, mx + 1, y, my + 1, colour); }
        if (m & FR_DOWN)  { vrun(dst, mx - 1, my - 1, y + h - 1, colour);
                            vrun(dst, mx + 1, my - 1, y + h - 1, colour); }
        return;
    }

    if (m & FR_LEFT)  hrun(dst, x, mx, my, colour);
    if (m & FR_RIGHT) hrun(dst, mx, x + w - 1, my, colour);
    if (m & FR_UP)    vrun(dst, mx, y, my, colour);
    if (m & FR_DOWN)  vrun(dst, mx, my, y + h - 1, colour);
}

static void blit_glyph(u8 *dst, int x, int y, int c, u8 colour)
{
    const Face *f = s_face;
    u8 *p = dst + y * s_pitch + x;
    int col;

    if (s_scale > 1) {
        const u16 *g = s_big[glyph_index(c)];
        for (col = 0; col < f->w * 2; col++) {
            u16 bits = g[col];
            u8 *q = p + col;
            while (bits) {
                if (bits & 1) *q = colour;
                bits >>= 1;
                q += s_pitch;
            }
        }
        return;
    }

    {
        const u8 *g = f->glyphs + glyph_index(c) * f->w;
        for (col = 0; col < f->w; col++) {
            u8 bits = g[col];
            u8 *q = p + col;
            while (bits) {
                if (bits & 1) *q = colour;
                bits >>= 1;
                q += s_pitch;
            }
        }
    }
}

/* ---- a window over the terminal ----------------------------------------
 * The terminal is a stream: things are printed into it and scroll away.
 * A window is the other thing a text interface needs -- a block of cells
 * at fixed places that can be drawn, changed and redrawn while the stream
 * carries on underneath.  It is drawn on top of the terminal every frame,
 * opaque, with a shadow, at whatever size the current face and screen are,
 * so it looks the same at 320x200 and 800x600.
 *
 * One at a time, which is all a modal menu needs.  Cells carry their own
 * background as well as their own colour, so a highlighted row is just a
 * row of cells with a lighter background.
 */
#define WIN_W 100
#define WIN_H 50
static u8  s_win_ch[WIN_H][WIN_W], s_win_fg[WIN_H][WIN_W], s_win_bg[WIN_H][WIN_W];
static u8  s_win_at[WIN_H][WIN_W];        /* bold, italic, underline        */
static int s_win_on, s_win_c, s_win_r, s_win_w, s_win_h;
static int s_win_bare;                   /* no paper, no shadow             */

/* Drawing only what changed.  When a window with paper covers the whole
 * screen -- browzer, write -- nothing under it can be seen, so nothing
 * under it is drawn (main.c), and the window itself is drawn only where a
 * cell differs from how it was drawn last time; the rows touched are
 * kept, so only those go to the card.  The picture is the same to the
 * pixel; the machine does a small part of the work. */
static u8  s_dr_ch[WIN_H][WIN_W], s_dr_fg[WIN_H][WIN_W], s_dr_bg[WIN_H][WIN_W], s_dr_at[WIN_H][WIN_W];
static int s_win_fresh = 1;              /* draw every cell next time       */
static int s_open_w, s_open_h, s_open_c, s_open_r;   /* the window as last opened */
static int s_dirty_y0 = -1, s_dirty_y1 = -1;   /* pixel rows drawn this frame */


int term_win_open(int w, int h)
{
    int x, y;

    geometry();
    if (w > COLS) w = COLS;              /* the whole screen, if asked     */
    if (h > ROWS) h = ROWS;
    if (w > WIN_W) w = WIN_W;
    if (h > WIN_H) h = WIN_H;
    s_win_w = w;
    s_win_h = h;
    s_win_c = (COLS - w) / 2;
    s_win_r = (ROWS - h) / 2;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            s_win_ch[y][x] = ' ';
            s_win_fg[y][x] = C_TEXT;
            s_win_bg[y][x] = WIN_PAPER;
            s_win_at[y][x] = 0;
        }
    /* Drawn whole only when it is new, or has moved or changed size.  An
     * app opening the same window again to redraw its contents -- browzer
     * does, every time -- needs only the cells that differ, which the
     * shadow arrays find. */
    if (!s_win_on || s_win_bare || w != s_open_w || h != s_open_h || s_win_c != s_open_c || s_win_r != s_open_r)
        s_win_fresh = 1;
    s_open_w = w; s_open_h = h; s_open_c = s_win_c; s_open_r = s_win_r;
    s_win_on = 1;
    s_win_bare = 0;
    return w;
}

/* A window with nothing behind it: every cell starts clear, so the screen
 * shows through wherever nothing is put, and the window casts no shadow.
 * Cells given a paper of their own still get it. */
void term_win_bare(void)
{
    int x, y;
    for (y = 0; y < s_win_h; y++)
        for (x = 0; x < s_win_w; x++) s_win_at[y][x] = ATTR_CLEAR;
    s_win_bare = 1;
}

void term_win_close(void) { s_win_on = 0; s_win_fresh = 1; }

/* Does a window with paper cover every cell of the screen?  A cell left
 * clear would show what is under it, so then it does not count. */
int term_win_covers(void)
{
    int x, y;
    if (!s_win_on || s_win_bare || s_win_w < COLS || s_win_h < ROWS || s_view) return 0;
    for (y = 0; y < s_win_h; y++)
        for (x = 0; x < s_win_w; x++)
            if (s_win_at[y][x] & ATTR_CLEAR) return 0;
    return 1;
}

/* The rows of pixels drawn since last asked: 0 if none. */
int term_dirty_rows(int *y0, int *y1)
{
    int any = s_dirty_y0 >= 0;
    *y0 = s_dirty_y0; *y1 = s_dirty_y1;
    s_dirty_y0 = s_dirty_y1 = -1;
    return any;
}

/* The same, without forgetting them: for the HUD, which draws over them. */
int term_dirty_peek(int *y0, int *y1)
{
    *y0 = s_dirty_y0; *y1 = s_dirty_y1;
    return s_dirty_y0 >= 0;
}

static void dirty(int y0, int y1)
{
    if (y0 < 0) y0 = 0;
    if (y1 >= s_vh) y1 = s_vh - 1;
    if (y1 < y0) return;
    if (s_dirty_y0 < 0 || y0 < s_dirty_y0) s_dirty_y0 = y0;
    if (y1 > s_dirty_y1) s_dirty_y1 = y1;
}
int  term_win_width(void)  { return s_win_w; }
int  term_win_height(void) { return s_win_h; }

void term_win_put_at(int x, int y, int ch, u8 fg, u8 bg, u8 attr)
{
    if (x < 0 || y < 0 || x >= s_win_w || y >= s_win_h) return;
    s_win_ch[y][x] = (u8)ch;
    s_win_fg[y][x] = fg;
    s_win_bg[y][x] = bg;
    s_win_at[y][x] = attr;
}

void term_win_put(int x, int y, int ch, u8 fg, u8 bg)
{
    term_win_put_at(x, y, ch, fg, bg, 0);
}


u8 g_key_colour = PAL_KEY;              /* pink, unless a program has asked for its own look */
/* \1 in the text turns pink on, for a key to press, and off again. */
int term_win_text(int x, int y, const char *t, u8 fg, u8 bg)
{
    int key = 0;
    for (; *t && x < s_win_w; t++) {
        if (*t == 1) { key = !key; continue; }
        term_win_put(x++, y, (u8)*t, key ? g_key_colour : fg, bg);
    }
    return x;
}

/* A row cleared to one background, from x to the right edge less one. */
void term_win_band(int y, int x0, int x1, u8 bg)
{
    int x;
    for (x = x0; x <= x1; x++) term_win_put(x, y, ' ', C_TEXT, bg);
}

/* The border, with a title let into the top edge. */
void term_win_frame(const char *title, u8 fg)
{
    int x, y, w = s_win_w, h = s_win_h;

    for (x = 1; x < w - 1; x++) {
        term_win_put(x, 0, FR_H, fg, WIN_PAPER);
        term_win_put(x, h - 1, FR_H, fg, WIN_PAPER);
    }
    for (y = 1; y < h - 1; y++) {
        term_win_put(0, y, FR_V, fg, WIN_PAPER);
        term_win_put(w - 1, y, FR_V, fg, WIN_PAPER);
    }
    term_win_put(0, 0, FR_TL, fg, WIN_PAPER);
    term_win_put(w - 1, 0, FR_TR, fg, WIN_PAPER);
    term_win_put(0, h - 1, FR_BL, fg, WIN_PAPER);
    term_win_put(w - 1, h - 1, FR_BR, fg, WIN_PAPER);
    if (title && *title) {
        term_win_put(2, 0, ' ', fg, WIN_PAPER);
        x = term_win_text(3, 0, title, C_BRIGHT, WIN_PAPER);
        term_win_put(x, 0, ' ', fg, WIN_PAPER);
    }
}

/* A glyph, bold and/or italic.  Bold is the classic trick: drawn twice,
 * one pixel apart.  Italic slants it: the top third shifted right by two
 * pixels' worth, the middle by one, the bottom left where it was -- the
 * bottom stays put so letters still sit on their line. */
static void blit_styled(u8 *dst, int x, int y, int c, u8 colour, u8 attr)
{
    const Face *f = s_face;
    int cols = f->w * s_scale, rows = f->h * s_scale, col, row;

    if (!(attr & (ATTR_BOLD | ATTR_ITALIC))) { blit_glyph(dst, x, y, c, colour); return; }

    for (col = 0; col < cols; col++) {
        u32 bits = s_scale > 1 ? s_big[glyph_index(c)][col]
                               : f->glyphs[glyph_index(c) * f->w + col];
        for (row = 0; row < rows && bits; row++, bits >>= 1) {
            int px, py;
            if (!(bits & 1)) continue;
            px = x + col;
            py = y + row;
            if (attr & ATTR_ITALIC) px += s_scale * (row < rows / 3 ? 2 : row < 2 * rows / 3 ? 1 : 0);
            if (px >= 0 && px < s_vw && py >= 0 && py < s_vh) {
                dst[py * s_pitch + px] = colour;
                if ((attr & ATTR_BOLD) && px + 1 < s_vw) dst[py * s_pitch + px + 1] = colour;
            }
        }
    }
}

static void fill_rect(u8 *dst, int x, int y, int w, int h, u8 colour)
{
    int r;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > s_vw) w = s_vw - x;
    if (y + h > s_vh) h = s_vh - y;
    if (w <= 0 || h <= 0) return;
    for (r = 0; r < h; r++) memset8(dst + (y + r) * s_pitch + x, colour, (usize)w);
}

static void render_window(u8 *dst, int only_changed)
{
    const Face *f = s_face;
    int cw = f->adv * s_scale, ch = f->pitch * s_scale;
    int ox = (f->x0 + s_win_c * f->adv) * s_scale;
    int oy = s_win_r * f->pitch * s_scale;
    int x, y;

    if (!only_changed) dirty(0, s_vh - 1);

    /* A shadow down the right and along the bottom, half a cell deep, so
     * the window sits on the screen rather than in it. */
    if (!s_win_bare && !only_changed) {
        fill_rect(dst, ox + s_win_w * cw, oy + ch / 2, cw / 2, s_win_h * ch, PAL_SHADOW);
        fill_rect(dst, ox + cw / 2, oy + s_win_h * ch, s_win_w * cw, ch / 2, PAL_SHADOW);
    }

    for (y = 0; y < s_win_h; y++) {
        for (x = 0; x < s_win_w; x++) {
            int px = ox + x * cw, py = oy + y * ch;
            int c = s_win_ch[y][x];
            u8 at = s_win_at[y][x], fg = s_win_fg[y][x];
            if (only_changed) {
                if (s_dr_ch[y][x] == (u8)c && s_dr_fg[y][x] == fg &&
                    s_dr_bg[y][x] == s_win_bg[y][x] && s_dr_at[y][x] == at) continue;
                dirty(py, py + ch - 1);
            }
            s_dr_ch[y][x] = (u8)c; s_dr_fg[y][x] = fg;
            s_dr_bg[y][x] = s_win_bg[y][x]; s_dr_at[y][x] = at;
            if (at & ATTR_CLEAR) {
                /* Over the moving background: a drop shadow, as the
                 * terminal's own text has, or it sinks into the colours. */
                if (c == ' ' || c == 0) continue;
                if (CH_IS_CELL(c)) { blit_cell(dst, px, py, c, fg); continue; }
                blit_styled(dst, px + s_scale, py + s_scale, c, C_SHADOW, at);
                blit_styled(dst, px, py, c, fg, at);
                continue;
            }
            fill_rect(dst, px, py, cw, ch, s_win_bg[y][x]);
            /* Underline: a line under the letters, the width of the cell,
             * so an underlined word is one line rather than dashes. */
            if (at & ATTR_UNDERLINE)
                fill_rect(dst, px, py + (s_face->pitch - 1) * s_scale, cw, s_scale, fg);
            if (c == ' ' || c == 0) continue;
            if (CH_IS_CELL(c)) { blit_cell(dst, px, py, c, fg); continue; }
            blit_styled(dst, px, py, c, fg, at);
        }
    }
}

void term_render(u8 *dst, u32 t)
{
    const Face *f = s_face;
    u32 base;
    int r;
    int cursor_row = -1;
    u8 st;

    geometry();

    /* A window covering everything: only it, and only where it changed
     * (every cell the first time, and after anything else drew). */
    if (term_win_covers()) {
        render_window(dst, !s_win_fresh);
        s_win_fresh = 0;
        return;
    }

    base = (s_line >= (u32)(ROWS - 1)) ? s_line - (ROWS - 1) : 0;
    if ((u32)s_view <= base) base -= (u32)s_view;
    else base = 0;

    for (r = 0; r < ROWS && !s_hidden; r++) {
        u32 abs = base + (u32)r;
        Cell *row;
        int c, y;

        if (abs > s_line) break;
        if (abs + SCROLLBACK <= s_line) continue;   /* aged out of the ring */

        row = row_of(abs);
        y = r * f->pitch * s_scale;
        if (abs == s_line) cursor_row = r;

        st = s_quirks ? s_style[abs % SCROLLBACK] : ST_PLAIN;

        for (c = 0; c < COLS; c++) {
            int ch = row[c].ch;
            int x, dx = 0, dy = 0, as_rune = 0;
            u8 col = row[c].col;
            u8 runes[GLYPH_W];
            if (ch == ' ' || ch == 0) continue;

            /* A frame is never afflicted.  The quirks are there to make
             * the prose uneasy, and a border that shivers is just broken. */
            if (st != ST_PLAIN && !CH_IS_CELL(ch))
                as_rune = style_cell(st, c, abs, t, &ch, &col, &dx, &dy, runes);
            x = (f->x0 + c * f->adv) * s_scale + dx;

            if (CH_IS_CELL(ch)) {
                if (f->shadow && ch < CH_SHADE1)
                    blit_cell(dst, x + s_scale, y + s_scale, ch, C_SHADOW);
                blit_cell(dst, x, y, ch, col);
                continue;
            }
            if (as_rune) {
                blit_cols(dst, x + s_scale, y + dy + s_scale, runes, C_SHADOW);
                blit_cols(dst, x, y + dy, runes, col);
                continue;
            }
            if (s_zalgo) {
                /* A character in eight sits a pixel off true, which keeps
                 * the line from ever looking quite settled. */
                u32 j = (u32)(ch * 40503u) ^ (u32)(c * 2246822519u)
                      ^ ((t >> 10) * 2654435761u);
                if ((j & 7) == 0) x += (int)((j >> 3) & 1) ? 1 : -1;
                zalgo_marks(dst, x, y + dy, ch, c, (int)abs, t);
            }
            if (f->shadow)
                blit_glyph(dst, x + s_scale, y + dy + s_scale, ch, C_SHADOW);
            blit_glyph(dst, x, y + dy, ch, col);
        }
    }

    /* The cursor: a soft block that breathes, with the character under it
     * punched back out in black so you can still read what you typed. */
    if (cursor_row >= 0 && s_view == 0 && s_col < COLS && !s_nocursor) {
        int x = (f->x0 + s_col * f->adv) * s_scale;
        int y = cursor_row * f->pitch * s_scale;
        int phase = (int)((t >> 3) & (SINLEN - 1));
        int lvl = (SIN(phase * 2) + 4096) * (PAL_CURSOR_N - 1) >> 13;
        u8 col = (u8)(PAL_CURSOR + lvl);
        int ch = row_of(s_line)[s_col].ch;
        int i;

        for (i = 0; i < f->pitch * s_scale - s_scale; i++)
            memset8(dst + (y + i) * s_pitch + x, col, f->w * s_scale);
        if (ch != ' ' && ch != 0) blit_glyph(dst, x, y, ch, 0);
    }

    if (s_win_on) render_window(dst, 0);
    s_win_fresh = 1;                     /* drawn over a fresh background    */

    /* A thumb on the right edge while looking back through history. */
    if (s_view > 0) {
        int span = SCROLLBACK - ROWS;
        int pos = span > 0 ? (s_vh - 24) * (span - s_view) / span : 0;
        int i;
        for (i = 0; i < 24; i++) {
            u8 *p = dst + (pos + i) * s_pitch + (s_pitch - 2);
            p[0] = C_ACCENT;
            p[1] = C_ACCENT;
        }
    }
}

/* ---- drawing over everything: the HUD ------------------------------------
 * The same face, the same frame pieces, on paper, at any place on the
 * canvas -- so something laid over the screen looks like the rest of it. */
void term_refresh(void) { s_win_fresh = 1; }
int  term_pitch(void) { return s_pitch; }

void term_cell_size(int *w, int *h)
{
    *w = s_face->adv * s_scale;
    *h = s_face->pitch * s_scale;
}

void term_overlay_fill(u8 *dst, int px, int py, int w, int h, u8 colour)
{
    if (px < 0 || py < 0 || w <= 0 || h <= 0 || px + w > s_vw || py + h > s_vh) return;
    fill_rect(dst, px, py, w, h, colour);
}

void term_overlay_cell(u8 *dst, int px, int py, int c, u8 fg, u8 bg)
{
    int w = s_face->adv * s_scale, h = s_face->pitch * s_scale;
    if (px < 0 || py < 0 || px + w > s_vw || py + h > s_vh) return;
    fill_rect(dst, px, py, w, h, bg);
    if (c == ' ' || c == 0) return;
    if (CH_IS_CELL(c)) { blit_cell(dst, px, py, c, fg); return; }
    blit_styled(dst, px, py, c, fg, 0);
}
