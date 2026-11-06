/* ===========================================================================
 * term.c -- the terminal: 53 columns by 25 rows, drawn by hand.
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
#define TEXT_X0    1                     /* 1 + 53*6 = 319, just fits      */

typedef struct {
    u8 ch;
    u8 col;
} Cell;

static Cell *s_grid;                     /* SCROLLBACK rows of TERM_COLS   */
static u32   s_line;                     /* absolute row the cursor is on  */
static int   s_col;
static u8    s_colour = C_TEXT;
static int   s_view;                     /* rows scrolled back, 0 = bottom */
static int   s_zalgo;
static u8    s_style[SCROLLBACK];        /* one quirk per line of history  */
static int   s_quirks = 1;

static void style_birth(u32 abs);

static Cell *row_of(u32 abs)
{
    return s_grid + (abs % SCROLLBACK) * TERM_COLS;
}

static void clear_row(u32 abs)
{
    Cell *r = row_of(abs);
    int i;
    for (i = 0; i < TERM_COLS; i++) { r[i].ch = ' '; r[i].col = 0; }
}

void term_init(void)
{
    u32 i;
    s_grid = (Cell *)halloc(SCROLLBACK * TERM_COLS * sizeof(Cell));
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

static void style_birth(u32 abs)
{
    u32 r = rnd();
    /* Plain is weighted heavily. Every line being peculiar is just noise;
     * it is the ordinary ones in between that make the rest land. */
    s_style[abs % SCROLLBACK] =
        (u8)(((r >> 3) & 1) ? ST_PLAIN : (r % ST_COUNT));
}

void term_quirks(int on) { s_quirks = on ? 1 : 0; }
int  term_quirks_on(void) { return s_quirks; }

void term_newline(void)
{
    /* The mirror lives here rather than in term_putc, because almost
     * nothing writes a newline as a character -- they call this. */
    if (serial_echo()) serial_putc('\n');

    s_line++;
    s_col = 0;
    s_view = 0;                          /* any output returns to the end  */
    clear_row(s_line);
    style_birth(s_line);
}

void term_putc(int c)
{
    Cell *r;

    if (c == '\n') { term_newline(); return; }   /* mirrors on its own */

    /* Everything FLOPOS says to itself, it also says down the wire, so a
     * session can be watched live from another machine. */
    if (serial_echo()) serial_putc(c);

    if (c == '\r') { s_col = 0; return; }
    if (c == '\t') {
        do { term_putc(' '); } while (s_col & 7);
        return;
    }
    if (s_col >= TERM_COLS) term_newline();

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
    if (c > TERM_COLS) c = TERM_COLS;
    s_col = c;
}

void term_rule(void)
{
    int i;
    u8 save = s_colour;
    s_colour = C_GREY;
    for (i = 0; i < TERM_COLS - 2; i++) term_putc('-');
    s_colour = save;
    term_newline();
}

/* ---- scrollback ------------------------------------------------------- */
void term_scroll_view(int lines)
{
    int max = (int)(s_line > (u32)(SCROLLBACK - TERM_ROWS)
                    ? (u32)(SCROLLBACK - TERM_ROWS) : s_line);
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
    int avail = TERM_COLS - from_col;
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
    if (s_col > TERM_COLS) s_col = TERM_COLS;
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
        mx  = x + (int)((h >> 7) % 5);
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

        if (mx < 0 || mx >= SCR_W) continue;
        for (k = 0; k < len; k++) {
            int my = up ? (y - d - k) : (y + 7 + d + k);
            if (my < 0 || my >= SCR_H) break;
            dst[my * SCR_W + mx] = ink;
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
    int i;
    for (i = 0; i < 5; i++) cols[i] = 0;

    cols[2] = 0x3E;                      /* the stem                       */

    if (letter & 1)  { cols[1] |= 0x04; cols[0] |= 0x08; }
    if (letter & 2)  { cols[3] |= 0x04; cols[4] |= 0x02; }
    if (letter & 4)  { cols[1] |= 0x20; cols[0] |= 0x20; }
    if (letter & 8)  { cols[3] |= 0x10; cols[4] |= 0x20; }
    if (letter & 16) { cols[1] |= 0x02; cols[3] |= 0x02; }
    if (letter >= 16) cols[2] |= 0x41;   /* a longer stem for the back half */
}

static void blit_cols(u8 *dst, int x, int y, const u8 *g, u8 colour)
{
    int col;
    for (col = 0; col < GLYPH_W; col++) {
        u8 bits = g[col];
        u8 *q = dst + y * SCR_W + x + col;
        if (x + col < 0 || x + col >= SCR_W) continue;
        while (bits) {
            if (bits & 1) {
                if (q >= dst && q < dst + SCR_PIX) *q = colour;
            }
            bits >>= 1;
            q += SCR_W;
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
        int k = (PAL_TEXT_N - 1) - (c * (PAL_TEXT_N - 1)) / TERM_COLS;
        if (k < 2) k = 2;
        *colour = (u8)(PAL_TEXT + k);
        break;
    }

    default:
        break;
    }
    return 0;
}

static void blit_glyph(u8 *dst, int x, int y, int c, u8 colour)
{
    const u8 *g = g_font5x8[glyph_index(c)];
    u8 *p = dst + y * SCR_W + x;
    int col;

    for (col = 0; col < GLYPH_W; col++) {
        u8 bits = g[col];
        u8 *q = p + col;
        while (bits) {
            if (bits & 1) *q = colour;
            bits >>= 1;
            q += SCR_W;
        }
    }
}

void term_render(u8 *dst, u32 t)
{
    u32 base;
    int r;
    int cursor_row = -1;
    u8 st;

    base = (s_line >= (u32)(TERM_ROWS - 1)) ? s_line - (TERM_ROWS - 1) : 0;
    if ((u32)s_view <= base) base -= (u32)s_view;
    else base = 0;

    for (r = 0; r < TERM_ROWS; r++) {
        u32 abs = base + (u32)r;
        Cell *row;
        int c, y;

        if (abs > s_line) break;
        if (abs + SCROLLBACK <= s_line) continue;   /* aged out of the ring */

        row = row_of(abs);
        y = r * LINE_PITCH;
        if (abs == s_line) cursor_row = r;

        st = s_quirks ? s_style[abs % SCROLLBACK] : ST_PLAIN;

        for (c = 0; c < TERM_COLS; c++) {
            int ch = row[c].ch;
            int x, dx = 0, dy = 0, as_rune = 0;
            u8 col = row[c].col;
            u8 runes[5];
            if (ch == ' ' || ch == 0) continue;

            if (st != ST_PLAIN)
                as_rune = style_cell(st, c, abs, t, &ch, &col, &dx, &dy, runes);
            x = TEXT_X0 + c * GLYPH_ADV + dx;
            if (as_rune) {
                blit_cols(dst, x + 1, y + dy + 1, runes, C_SHADOW);
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
            blit_glyph(dst, x + 1, y + dy + 1, ch, C_SHADOW);
            blit_glyph(dst, x, y + dy, ch, col);
        }
    }

    /* The cursor: a soft block that breathes, with the character under it
     * punched back out in black so you can still read what you typed. */
    if (cursor_row >= 0 && s_view == 0 && s_col < TERM_COLS) {
        int x = TEXT_X0 + s_col * GLYPH_ADV;
        int y = cursor_row * LINE_PITCH;
        int phase = (int)((t >> 3) & (SINLEN - 1));
        int lvl = (SIN(phase * 2) + 4096) * (PAL_CURSOR_N - 1) >> 13;
        u8 col = (u8)(PAL_CURSOR + lvl);
        int ch = row_of(s_line)[s_col].ch;
        int i;

        for (i = 0; i < LINE_PITCH - 1; i++)
            memset8(dst + (y + i) * SCR_W + x, col, 5);
        if (ch != ' ' && ch != 0) blit_glyph(dst, x, y, ch, 0);
    }

    /* A thumb on the right edge while looking back through history. */
    if (s_view > 0) {
        int span = SCROLLBACK - TERM_ROWS;
        int pos = span > 0 ? (SCR_H - 24) * (span - s_view) / span : 0;
        int i;
        for (i = 0; i < 24; i++) {
            u8 *p = dst + (pos + i) * SCR_W + (SCR_W - 2);
            p[0] = C_ACCENT;
            p[1] = C_ACCENT;
        }
    }
}
