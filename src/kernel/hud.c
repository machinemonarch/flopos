/* ===========================================================================
 * hud.c -- the heads-up display, on ctrl-shift-f10.
 *
 * A panel that rolls down from the top edge, the whole width of the
 * screen, over whatever is on it: a command line on the left, and on the
 * right, behind a divider:
 *   - bars that move with whatever sound is playing
 *   - the battery, as the laptop's power management (APM) reports it
 *   - 'Thunking: how busy the processor is
 *   - 'Membering: how much of the memory is in use
 *   - a brain, turning, coloured by the two together: blue while the
 *     machine is idle, then green, yellow and orange, and red when it is
 *     working as hard as it can
 *   - a command line of its own, which takes the shell's commands
 *
 * ctrl-shift-f10 rolls it down, and its command line has the keys.  esc
 * gives the keys back to whatever had them and leaves it showing.
 * ctrl-shift-f10 again rolls it up.  With gibson on, each way chimes.
 *
 * How busy the processor is comes from the time it spends waiting for its
 * next interrupt (see idle_wait in time.c): FLOPOS halts the processor
 * whenever it has nothing to do, and counts the cycles it spent halted.
 * ===========================================================================
 */
#include "flopos.h"
#include "io.h"

extern char _bss_end[];

/* Its size, in cells: the screen's width, HUD_H rows; the right PANEL_W
 * cells are the meters'.  What commands said scrolls (pgup, pgdn, the
 * arrows). */
#define HUD_H    16
#define PANEL_W  28
#define MAXW     140                     /* the widest screen, in cells     */
#define IN_MAX   (MAXW - PANEL_W - 4)
#define LOG_N 128                        /* lines kept of what commands said */
#define HUD_LINE 160
static int s_hw = 66, s_hh = HUD_H;
static int s_back;                       /* lines scrolled back             */
static int s_clip;                       /* text stops short of this column */
static int s_roll, s_closing;            /* pixels rolled down; rolling up  */
#define XD      (s_hw - PANEL_W)         /* the divider's column            */
#define OUT_N   (s_hh - 4)               /* lines of what was said, shown   */
#define IN_W    (XD - 4)                 /* the command line's room         */

volatile int g_hud_toggle;               /* ctrl-shift-f10, from the keyboard interrupt */
u32 g_idle_cycles;                       /* halted, waiting: see idle_wait  */

static int s_on, s_keys, s_running;      /* shown; its line has the keys; a command is running */
static int s_px, s_py, s_cw, s_ch;       /* where it is, and a cell, in pixels */
static int s_drawn, s_full;              /* drawn this frame; all of it      */
static u32 s_cost;
static u32 s_tver = 1;                   /* bumped when the meters change: the top only */
static u32 s_ver = 1;                    /* bumped when the words change     */                       /* its own drawing, cycles, averaged */

/* ---- 'Thunking and 'Membering ------------------------------------------- */
static u32 s_t0, s_idle0;
static int s_cpu, s_mem, s_load = -1;    /* percent; load: the two together */

static void measure(void)
{
    u32 now, span, idle;
    if (!g_cpu_mhz) return;
    now = tsc32();
    span = now - s_t0;
    if (s_t0 && span < g_cpu_mhz * 500000u) return;   /* twice a second  */
    idle = g_idle_cycles - s_idle0;
    if (s_t0) {
        int busy = 100 - (int)(idle / (span / 100 + 1));
        busy = busy < 0 ? 0 : busy > 100 ? 100 : busy;
        if (busy != s_cpu) { s_cpu = busy; s_tver++; }
    }
    s_t0 = now; s_idle0 = g_idle_cycles;
    {
        u32 total = (BOOTINFO_EXTKB + 1024u) * 1024u;          /* all of it, as the BIOS counted */
        u32 used = heap_used() + app_bytes() + (u32)_bss_end;
        int mem = total ? (int)(used / (total / 100 + 1)) : 0;
        if (mem > 100) mem = 100;
        if (mem != s_mem) { s_mem = mem; s_tver++; }
    }
}

/* ---- the battery ------------------------------------------------------- */
static int s_apm = -1;                   /* -1 not asked yet, 0 none, 1 there */
static int s_batt = -1, s_ac = -1, s_charging;
static u32 s_batt_at;

/* APM's "get power status".  The BIOS may want an interface connected
 * first; then one is, just for the question, and let go again, so that
 * the laptop goes on managing its own power as it always has. */
static void battery(void)
{
    u32 r;
    u16 bx = 0, cx = 0, dx = 0;
    if (!s_apm) return;
    if (s_apm < 0) {
        s_apm = !(bios_int(0x15, 0x5300, 0x0000, 0, 0, 0, 0) & 0x10000);
        if (!s_apm) return;
    }
    r = bios_int(0x15, 0x530A, 0x0001, 0, 0, 0, 0);
    if ((r & 0x10000) && ((r >> 8) & 0xFF) == 0x03) {          /* not connected */
        (void)bios_int(0x15, 0x5301, 0x0000, 0, 0, 0, 0);
        r = bios_int(0x15, 0x530A, 0x0001, 0, 0, 0, 0);
        bios_regs(&bx, &cx, &dx);
        (void)bios_int(0x15, 0x5304, 0x0000, 0, 0, 0, 0);       /* let go     */
    } else
        bios_regs(&bx, &cx, &dx);
    (void)dx;
    if (r & 0x10000) { s_batt = -1; return; }
    s_ac = (bx >> 8) == 1 ? 1 : (bx >> 8) == 0 ? 0 : -1;
    s_charging = (bx & 0xFF) == 3;
    s_batt = (cx & 0xFF) <= 100 ? (cx & 0xFF) : -1;
}

/* ---- the sound bars ---------------------------------------------------- */
#define BARS 10
static int s_bar[BARS];

/* Ten Goertzel filters, an octave apart from 32 Hz to the card's ceiling
 * (it plays at most 22 kHz, so nothing above 11 kHz exists), over what the
 * card has just played.  Each listens to as many samples as two of its
 * own waves take -- 1024 (46 ms) for the lowest, a handful for the
 * highest -- so a low note is told apart from the next octave up and a
 * high one still answers quickly.  Thirty times a second. */
static void bars(void)
{
    static const u16 hz[BARS] = { 32, 63, 125, 250, 500, 1000, 2000, 4000, 7000, 10500 };
    static u8 buf[1024];
    int k, i, rate = audio_rate_now();
    if (!rate || !audio_peek(buf, (int)sizeof buf)) {
        for (k = 0; k < BARS; k++) if (s_bar[k] > 0) s_bar[k]--;
        return;
    }
    for (k = 0; k < BARS; k++) {
        s64 s1 = 0, s2 = 0, coef, p;                 /* coef: 2 cos(2 pi f / rate), in 2^24ths */
        int lvl = 0, n = 2 * rate / hz[k], ln = 0;
        if (hz[k] * 2 >= (u32)rate) { s_bar[k] = 0; continue; }
        if (n > (int)sizeof buf) n = (int)sizeof buf;
        if (n < 8) n = 8;
        while ((1 << (ln + 1)) <= n) ln++;           /* log2 of the length */
        if ((u32)hz[k] * SINLEN / (u32)rate < 16) {  /* low: the table is too coarse; cos x = 1 - x*x/2 */
            s64 x = (s64)(105414357u / (u32)rate) * hz[k];   /* 2 pi f / rate, in 2^24ths */
            coef = 2 * (((s64)1 << 24) - ((x * x) >> 25));
        } else coef = (s64)(2 * COS((int)((u32)hz[k] * SINLEN / (u32)rate))) << 12;
        for (i = (int)sizeof buf - n; i < (int)sizeof buf; i++) {
            s64 s0 = ((s64)buf[i] - 128) + ((coef * s1) >> 24) - s2;
            s2 = s1; s1 = s0;
        }
        p = s1 * s1 + s2 * s2 - ((coef * s1) >> 24) * s2;
        while (p > 1) { p >>= 1; lvl++; }            /* doublings: loudness as the ear hears it */
        lvl += 3 - 2 * ln;                           /* the same scale whatever the length */
        if (lvl < 0) lvl = 0;
        if (lvl > 12) lvl = 12;
        if (lvl > s_bar[k]) s_bar[k] = lvl;
        else if (s_bar[k] > 0) s_bar[k]--;
    }
}

/* ---- the brain ----------------------------------------------------------- */
/* A brain as it looks from outside: two halves side by side with a deep
 * cleft between them from front to back, each half folded, a cerebellum
 * tucked under the back, and the stem below it.  Rings and meridians, so
 * it is a wireframe like the banana.
 *
 * The folds are made two ways, and neither is a spike.  The surface rises
 * and falls in slow swells, never faster than the mesh can follow.  And
 * the lines themselves wander: each corner of the mesh is slid a little
 * along the surface, one way and then the other, so rings and meridians
 * run crooked like the grooves of a brain while the outline stays round. */
#define LAT 7
#define LON 12
#define CLAT 3
#define CLON 10
#define BV (2 * LAT * LON + CLAT * CLON + 2)
#define BE (2 * (LAT * LON + (LAT - 1) * LON) + CLAT * CLON + (CLAT - 1) * CLON + 1)
static s16 s_vx[BV], s_vy[BV], s_vz[BV];
static u16 s_e0[BE], s_e1[BE];
static int s_nv, s_ne, s_built;

/* side: -1 or 1 for a half of the cerebrum, 0 for a plain shape. */
static void ellipsoid(int cx, int cy, int cz, int rx, int ry, int rz, int nlat, int nlon,
                      int lat0, int lat1, int side)
{
    int i, j, first = s_nv;
    for (i = 0; i < nlat; i++) {
        for (j = 0; j < nlon; j++) {
            int th = lat0 + (lat1 - lat0) * i / (nlat - 1);       /* 1024ths of a turn */
            int ph = j * SINLEN / nlon;
            int r = 4096, x, y, z;
            if (side) {
                if (i > 0 && i < nlat - 1) {                       /* the lines wander */
                    ph += (i & 1) ? 26 : -26;
                    th += (j & 1) ? 16 : -16;
                }
                r += (SIN(ph * 2 + 200) * (s32)SIN(th * 5 + j * 90)) >> 15;   /* slow swells */
            }
            x = (int)(((s32)COS(th) * COS(ph) >> 12) * rx >> 12) * r >> 12;
            y = (int)((s32)SIN(th) * ry >> 12) * r >> 12;
            z = (int)(((s32)COS(th) * SIN(ph) >> 12) * rz >> 12) * r >> 12;
            if (side) {
                x = x * (700 - z) / 700;                           /* a little wider at the back */
                x += cx;
                /* The inner wall: neither half crosses the middle, so the
                 * cleft between them stays open from front to back. */
                if (side * x < 2) x = side * 2;
                x -= cx;
            }
            if (y < 0) y = y * 6 / 10;                             /* flat below */
            s_vx[s_nv] = (s16)(cx + x); s_vy[s_nv] = (s16)(cy - y); s_vz[s_nv] = (s16)(cz + z);
            s_nv++;
        }
    }
    for (i = 0; i < nlat; i++)
        for (j = 0; j < nlon; j++) {
            int a = first + i * nlon + j;
            s_e0[s_ne] = (u16)a; s_e1[s_ne] = (u16)(first + i * nlon + (j + 1) % nlon); s_ne++;
            if (i + 1 < nlat) { s_e0[s_ne] = (u16)a; s_e1[s_ne] = (u16)(a + nlon); s_ne++; }
        }
}

static void build(void)
{
    ellipsoid(-21, 0, 0, 27, 40, 60, LAT, LON, -150, 256, -1);     /* the left half  */
    ellipsoid( 21, 0, 0, 27, 40, 60, LAT, LON, -150, 256,  1);     /* the right half */
    ellipsoid(0, 28, -40, 28, 12, 15, CLAT, CLON, -200, 200, 0);   /* the cerebellum */
    s_vx[s_nv] = 0; s_vy[s_nv] = 22; s_vz[s_nv] = -14; s_nv++;     /* the stem      */
    s_vx[s_nv] = 0; s_vy[s_nv] = 50; s_vz[s_nv] = -22; s_nv++;
    s_e0[s_ne] = (u16)(s_nv - 2); s_e1[s_ne] = (u16)(s_nv - 1); s_ne++;
    s_built = 1;
}

/* The colour of the load: blue, green, yellow, orange, red. */
static void colours(void)
{
    static const u8 stop[5][3] = { { 8, 18, 63 }, { 8, 52, 18 }, { 60, 56, 8 }, { 63, 32, 4 }, { 63, 6, 6 } };
    static u32 s_sent_at;
    u8 rgb[PAL_HUD_N * 3];
    int load = (s_cpu + s_mem) / 2, k, seg, f, c[3], i;
    if (load == s_load && ms_now() - s_sent_at < 2000) return;   /* a fade may have overwritten it */
    s_load = load; s_sent_at = ms_now();
    seg = load * 4 / 100; if (seg > 3) seg = 3;
    f = load * 4 - seg * 100;                                       /* 0..100 within it */
    for (i = 0; i < 3; i++) c[i] = stop[seg][i] + (stop[seg + 1][i] - stop[seg][i]) * f / 100;
    for (k = 0; k < PAL_HUD_N; k++)                                 /* far and dim to near and bright */
        for (i = 0; i < 3; i++) rgb[k * 3 + i] = (u8)(c[i] * (2 + k) / (PAL_HUD_N + 1));
    vga_set_colours(PAL_HUD, PAL_HUD_N, rgb);
}

static void line(u8 *dst, int pitch, int x0, int y0, int x1, int y1, u8 col, int bx, int by, int bw, int bh)
{
    int dx = x1 - x0, dy = y1 - y0, sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1, err, n;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    err = dx - dy;
    n = (dx > dy ? dx : dy) + 1;
    if (n > 400) return;
    while (n--) {
        if (x0 >= bx && x0 < bx + bw && y0 >= by && y0 < by + bh) dst[y0 * pitch + x0] = col;
        { int e2 = err * 2; if (e2 > -dy) { err -= dy; x0 += sx; } if (e2 < dx) { err += dx; y0 += sy; } }
    }
}

static void brain(u8 *dst, int pitch, int bx, int by, int bw, int bh, u32 t)
{
    static int px[BV], py[BV], pz[BV];
    int i, ay = (int)((t * 3) >> 5) & SINMASK, ax = 60;            /* turning, tipped to show the top */
    int cx = bx + bw / 2, cy = by + bh / 2, scale = bh * 256 / 136;  /* no perspective: no dividing */
    if (!s_built) build();
    for (i = 0; i < s_nv; i++) {
        int x = s_vx[i], y = s_vy[i], z = s_vz[i], x1, z1, y2, z2;
        x1 = (x * COS(ay) - z * SIN(ay)) >> 12;
        z1 = (x * SIN(ay) + z * COS(ay)) >> 12;
        y2 = (y * COS(ax) - z1 * SIN(ax)) >> 12;
        z2 = (y * SIN(ax) + z1 * COS(ax)) >> 12;
        px[i] = cx + ((x1 * scale) >> 8);
        py[i] = cy + ((y2 * scale) >> 8);
        pz[i] = z2;
    }
    for (i = 0; i < s_ne; i++) {
        int a = s_e0[i], b = s_e1[i], lvl = (PAL_HUD_N - 1) - ((pz[a] + pz[b]) / 2 + 60) * (PAL_HUD_N - 1) / 120;
        if (lvl < 0) lvl = 0;
        if (lvl > PAL_HUD_N - 1) lvl = PAL_HUD_N - 1;
        line(dst, pitch, px[a], py[a], px[b], py[b], (u8)(PAL_HUD + lvl), bx, by, bw, bh);
    }
}

/* ---- its command line ------------------------------------------------- */
static u16  s_in[IN_MAX + 1];            /* characters, wide ones too       */
static int  s_in_len;
static u16  (*s_log)[HUD_LINE + 1];      /* LOG_N lines, from the heap; \1 marks a key */
static u32  s_log_n;                     /* lines ever ended                */
static u16  s_cur[HUD_LINE + 1];
static int  s_cur_len;

static void out_line(void)
{
    u16 *d;
    if (!s_log) s_log = (u16 (*)[HUD_LINE + 1])halloc(LOG_N * (HUD_LINE + 1) * sizeof(u16));
    d = s_log[s_log_n % LOG_N];
    int i;
    for (i = 0; i < s_cur_len; i++) d[i] = s_cur[i];
    d[i] = 0;
    s_log_n++;
    s_cur_len = 0;
    s_back = 0;
    s_ver++;
}

/* What a command prints, while it runs from here (see term_tap).  Lines
 * are kept whole, and broken to the window's width when drawn. */
static void out_char(int c)
{
    if (c == '\r') return;
    if (c == '\n') { out_line(); return; }
    if (c == '\t') c = ' ';
    if (s_cur_len >= HUD_LINE) out_line();
    s_cur[s_cur_len++] = (u16)c;
}

/* A line broken at spaces into pieces of at most w cells (the \1 marks
 * take none): where each begins and how long it is.  Returns how many. */
#define PIECES 8
static int wrap(const u16 *t, int w, int *at, int *len)
{
    int n = 0, i = 0, L = 0;
    while (t[L]) L++;
    if (!L) { at[0] = 0; len[0] = 0; return 1; }
    while (i < L && n < PIECES) {
        int e = i, vis = 0, sp = -1;
        while (e < L && (vis < w || t[e] == 1)) {
            if (t[e] != 1) { if (t[e] == ' ') sp = e; vis++; }
            e++;
        }
        at[n] = i;
        if (e >= L) { len[n++] = L - i; break; }
        if (t[e] == ' ') { len[n++] = e - i; i = e + 1; }
        else if (sp > i) { len[n++] = sp - i; i = sp + 1; }
        else { len[n++] = e - i; i = e; }
    }
    return n;
}

/* How many lines the log takes at w cells, from line `from` on. */
static int log_lines(int w, u32 from)
{
    int at[PIECES], len[PIECES], n = 0;
    u32 first = s_log_n > LOG_N ? s_log_n - LOG_N : 0, i;
    if (from < first) from = first;
    for (i = from; i < s_log_n; i++) n += wrap(s_log[i % LOG_N], w, at, len);
    return n;
}

static void say(const char *t)
{
    int c;
    while ((c = ch_next(&t)) != 0) out_char(c);
    out_char('\n');
}

static void run(void)
{
    char word[16], line[IN_MAX * 4 + 1];
    int i, k = 0, n = 0, screen = 0;
    s_in[s_in_len] = 0;
    for (i = 0; i < s_in_len; i++) n += ch_put(line + n, s_in[i]);
    line[n] = 0;
    for (i = 0; s_in[i] == ' '; i++) { }
    while (s_in[i] && s_in[i] != ' ' && k < (int)sizeof word - 1) word[k++] = s_in[i] < 0x80 ? (char)s_in[i] : '?', i++;
    word[k] = 0;
    if (!k) return;
    screen = app_exists(word) || (word[0] == 'w' && word[1] == 'i' && word[2] == 'z' && word[3] == 'a' &&
                                  word[4] == 'r' && word[5] == 'd' && !word[6]);
    out_char('>'); out_char(' ');
    for (i = 0; s_in[i]; i++) out_char(s_in[i]);
    out_char('\n');
    if (screen && app_active()) {
        say("Not while an app is open.");
    } else if (screen) {
        s_keys = 0;                          /* it takes the screen, and the keys */
        s_in_len = 0;
        shell_submit(line);
        return;
    } else {
        int was = term_muted();
        term_tap(out_char);
        term_mute(1);
        s_running = 1;
        shell_submit(line);
        s_running = 0;
        term_mute(was);
        term_tap(0);
        if (s_cur_len) out_line();
        s_back = 0;
    }
    s_in_len = 0;
}

int hud_takes_keys(void) { return s_on && s_keys && !s_running; }

void hud_key(int ev)
{
    int sc = KEV_SCAN(ev), c;
    s_ver++;
    if (sc == SC_ESC) { s_keys = 0; s_back = 0; return; }
    if ((ev & KEV_EXT) && (sc == SC_PGUP || sc == SC_PGDN || sc == SC_UP || sc == SC_DOWN)) {
        int most = log_lines(XD - 2, 0) - OUT_N, by = sc == SC_UP || sc == SC_DOWN ? 1 : OUT_N - 1;
        s_back += sc == SC_PGUP || sc == SC_UP ? by : -by;
        if (s_back > most) s_back = most;
        if (s_back < 0) s_back = 0;
        return;
    }
    if (sc == SC_ENTER) { run(); return; }
    if (sc == SC_BKSP) { if (s_in_len) s_in_len--; return; }
    c = keymap_char(ev);
    if (c >= 32 && c != 127 && s_in_len < IN_MAX) s_in[s_in_len++] = (u16)c;
}

/* ---- drawing ----------------------------------------------------------- */
static void cell(u8 *dst, int cx, int cy, int c, u8 fg)
{
    term_overlay_cell(dst, s_px + cx * s_cw, s_py + cy * s_ch, c, fg, WIN_PAPER);
}

static void text_n(u8 *dst, int cx, int cy, const char *t, int n, u8 fg, int key)
{
    const char *p = t;
    int c;
    while (n-- && cx < s_clip && (c = ch_next(&p)) != 0) {
        if (c == 1) { key = !key; continue; }            /* a key: pink */
        cell(dst, cx++, cy, c, key ? C_KEY : fg);
    }
}

/* The same for a line of the log, which holds characters, not bytes. */
static void text_w(u8 *dst, int cx, int cy, const u16 *t, int n, u8 fg, int key)
{
    u16 chs[HUD_LINE] = { 0 }, order[HUD_LINE];
    u8 col[HUD_LINE];
    int k = 0, v;
    for (; *t && n-- && k < (int)(sizeof chs / sizeof *chs); t++) {
        if (*t == 1) { key = !key; continue; }
        chs[k] = *t; col[k++] = key ? C_KEY : fg;
    }
    if (!bidi_order(chs, k, order)) for (v = 0; v < k; v++) order[v] = (u16)v;
    for (v = 0; v < k && cx < s_clip; v++) {             /* Hebrew: right to left */
        int i = order[v] & ~BIDI_MIRROR;
        cell(dst, cx++, cy, (order[v] & BIDI_MIRROR) ? bidi_mirror(chs[i]) : chs[i], col[i]);
    }
}

static void text(u8 *dst, int cx, int cy, const char *t, u8 fg)
{
    text_n(dst, cx, cy, t, -1, fg, 0);
}

static void toggle(void)
{
    s_ver++;
    if (!s_on || s_closing) {
        if (!s_on) s_roll = 0;
        s_on = 1; s_closing = 0; s_keys = 1; s_batt_at = 0;
        if (!s_log_n) {
            say("Commands, as at here>.");
            say("\1esc\1 gives the keys back.");
        }
    } else {
        s_closing = 1; s_keys = 0; s_back = 0;           /* it rolls up, then is gone */
        serial_printf("[hud] 'Thunking %u%%, 'Membering: %u%%, battery %d; it took %u us a frame, the text %u, sending %u\n",
                      (u32)s_cpu, (u32)s_mem, s_batt, g_cpu_mhz ? s_cost / g_cpu_mhz : 0, perf_us(1), perf_us(2));
    }
}

int hud_shown(void) { return s_on && !s_closing; }

/* The frame and the words, into dst: drawn again only when one of them
 * changes, into a strip of its own (see hud_frame).  meters_only: just
 * the right-hand panel, whose numbers change far more often. */
static void paint(u8 *dst, int meters_only)
{
    int x, y, x0 = meters_only ? XD : 0, px = XD + 2;
    char v[24];

    /* The frame: round the whole, a divider before the meters, and on the
     * left a rule above the command line. */
    for (y = 0; y < s_hh; y++)
        for (x = x0; x < s_hw; x++) {
            int top = y == 0, bot = y == s_hh - 1, lft = x == 0, rgt = x == s_hw - 1, mid = x == XD;
            int rule = y == s_hh - 3 && x < XD, c = ' ';
            int up = !top && (lft || rgt || mid), down = !bot && (lft || rgt || mid);
            int l = (top || bot || rule) && !lft, r = (top || bot || rule) && !rgt && !(rule && mid);
            if (rule && mid) l = 1;
            if (rule && lft) { r = 1; up = down = 1; }
            if (up || down || l || r)
                c = CH_FRAME | (up ? FR_UP : 0) | (down ? FR_DOWN : 0) | (l ? FR_LEFT : 0) | (r ? FR_RIGHT : 0);
            cell(dst, x, y, c, C_GREY);
        }
    s_clip = s_hw - 1;
    if (!meters_only) text(dst, 2, 0, " HUD ", C_DIM);

    /* The meters: the sound's bars are drawn at hud_frame's pace, in rows
     * 1 to 3; under them the battery, the processor and the memory, and
     * the brain to their right. */
    if (s_batt >= 0) {
        fmt_str(v, sizeof v, "battery %u%%", (u32)s_batt);
        text(dst, px, 5, v, C_BRIGHT);
        text(dst, px, 6, s_charging ? "charging" : s_ac == 1 ? "on the mains" : s_ac == 0 ? "on battery" : "", C_DIM);
    } else {
        text(dst, px, 5, "no battery", C_DIM);
        text(dst, px, 6, "reading", C_DIM);
    }
    text(dst, px, 8, "'Thunking", C_TEXT);
    fmt_str(v, sizeof v, "%u%%", (u32)s_cpu); text(dst, px + 15 - strlen8(v), 8, v, C_BRIGHT);
    text(dst, px, 9, "'Membering:", C_TEXT);
    fmt_str(v, sizeof v, "%u%%", (u32)s_mem); text(dst, px + 15 - strlen8(v), 9, v, C_BRIGHT);
    if (meters_only) return;

    /* What commands said, and the line being typed, on the left. */
    s_clip = XD - 1;
    {
        int at[PIECES], len[PIECES], row = OUT_N - 1, skip = s_back, n, j, m, key;
        u32 first = s_log_n > LOG_N ? s_log_n - LOG_N : 0, i = s_log_n;
        while (s_log && i > first && row >= 0) {
            const u16 *t = s_log[--i % LOG_N];
            n = wrap(t, XD - 2, at, len);
            for (j = n - 1; j >= 0 && row >= 0; j--) {
                if (skip) { skip--; continue; }
                for (m = 0, key = 0; m < at[j]; m++) key ^= t[m] == 1;   /* pink carried on */
                text_w(dst, 1, 1 + row, t + at[j], len[j], C_TEXT, key);
                row--;
            }
        }
    }
    if (s_back) text(dst, XD - 16, s_hh - 3, " \1pgup\1 \1pgdn\1 ", C_DIM);
    cell(dst, 1, s_hh - 2, '>', s_keys ? C_ACCENT : C_DIM);
    {   /* a line longer than its room slides left, keeping its end in sight */
        int sl = s_in_len > IN_W ? s_in_len - IN_W : 0;
        text_w(dst, 3, s_hh - 2, s_in + sl, s_in_len - sl, s_keys ? C_BRIGHT : C_DIM, 0);
    }
}

void hud_frame(u8 *dst, u32 t)
{
    static u8 *strip;                        /* all of it, as last drawn    */
    static u32 painted, tpainted, moved_at;
    int vw = vga_width(), vh = vga_height(), pitch = term_pitch(), k, all, y, fresh;
    u32 c0;

    s_drawn = 0;
    if (g_hud_toggle) { g_hud_toggle = 0; toggle(); }
    if (!s_on) return;
    c0 = g_cpu_mhz ? tsc32() : 0;
    term_cell_size(&s_cw, &s_ch);
    {
        int hw = vw / s_cw, hh = HUD_H;
        if (hw > MAXW) hw = MAXW;
        if (hh * s_ch > vh / 2) hh = vh / 2 / s_ch;          /* the small screen: half of it */
        if (hw != s_hw || hh != s_hh) { s_hw = hw; s_hh = hh; s_ver++; }
    }
    all = s_hh * s_ch;
    s_px = 0; s_py = 0;

    /* Rolling down or up: a sixth of its height a frame. */
    if (s_closing) {
        s_roll -= all / 6 + 1;
        term_refresh();                       /* what it uncovers, drawn again */
        if (s_roll <= 0) { s_on = 0; s_closing = 0; s_roll = 0; return; }
    } else if (s_roll < all) {
        s_roll += all / 6 + 1;
        if (s_roll > all) s_roll = all;
    }

    measure();
    if (!s_batt_at || ms_now() - s_batt_at > 30000) { s_batt_at = ms_now() | 1; battery(); s_tver++; }
    colours();

    /* Everything is drawn into a strip of its own, and copied to the screen.
     * The words only when they change; the meters' numbers when they do;
     * the bars and the brain thirty times a second. */
    if (!strip) strip = (u8 *)halloc((u32)pitch * (u32)(HUD_H * 16 + 16));
    fresh = 0;
    if (painted != s_ver) { paint(strip, 0); painted = s_ver; tpainted = s_tver; fresh = 1; }
    else if (tpainted != s_tver) { paint(strip, 1); tpainted = s_tver; }
    if (fresh || ms_now() - moved_at >= 33) {
        int bx = (XD + 2) * s_cw, by = s_ch, bw = (PANEL_W - 3) * s_cw, bh = 3 * s_ch, each = bw / BARS;
        moved_at = ms_now();
        bars();
        term_overlay_fill(strip, bx, by, bw, bh, WIN_PAPER);
        for (k = 0; k < BARS; k++) {
            int h = s_bar[k] * bh / 12;
            if (h < 1) h = 1;
            term_overlay_fill(strip, bx + k * each + 1, by + bh - h, each - 2, h, C_ACCENT);
        }
        bx = (s_hw - 10) * s_cw; by = 5 * s_ch;               /* the brain */
        term_overlay_fill(strip, bx, by, 8 * s_cw, 6 * s_ch, WIN_PAPER);
        brain(strip, pitch, bx, by, 8 * s_cw, 6 * s_ch, t);
    }

    /* To the screen.  Rolling, or the words changed, or the screen under
     * it was drawn again: all of it, its bottom edge s_roll down.  Else
     * only the meters, which move. */
    {
        static int caret_was = -1;
        int caret = s_keys && ((t >> 9) & 1), w4 = ((s_hw * s_cw) + 3) & ~3;
        int rolling = s_roll < all, from = all - s_roll;
        s_full = rolling || fresh || caret != caret_was || !term_win_covers() || g_frame_fx != 0;
        caret_was = caret;
        if (s_full) {
            for (y = 0; y < s_roll; y++)
                memcpy32(dst + y * pitch, strip + (from + y) * pitch, (u32)w4 / 4);
        } else {
            int mx = (XD * s_cw) & ~3, mw = (w4 - mx) / 4;
            for (y = 0; y < 11 * s_ch && y < all; y++)
                memcpy32(dst + y * pitch + mx, strip + y * pitch + mx, (u32)mw);
        }
        /* The caret, on the screen only. */
        if (caret && !rolling)
            term_overlay_fill(dst, (3 + (s_in_len > IN_W ? IN_W : s_in_len)) * s_cw, (s_hh - 1) * s_ch - 3, s_cw - 1, 2, C_ACCENT);
    }
    s_drawn = 1;
    if (g_cpu_mhz) s_cost = s_cost - (s_cost >> 3) + ((tsc32() - c0) >> 3);
}

/* The rectangle it drew, for sending to the card: 0 if none. */
int hud_rect(int *x0, int *y0, int *x1, int *y1)
{
    if (!s_drawn) return 0;
    *y0 = 0;
    if (s_full) { *x0 = 0; *x1 = s_hw * s_cw - 1; *y1 = s_roll - 1; }
    else        { *x0 = (XD * s_cw) & ~3; *x1 = s_hw * s_cw - 1; *y1 = 11 * s_ch - 1; }
    return 1;
}
