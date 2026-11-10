/* ===========================================================================
 * hud.c -- the heads-up display, on ctrl-shift-f10.
 *
 * A small window in the top right corner, over whatever is on the screen,
 * in the same letters and frame as everything else:
 *   - bars that move with whatever sound is playing
 *   - the battery, as the laptop's power management (APM) reports it
 *   - 'Thunking: how busy the processor is
 *   - 'Membering: how much of the memory is in use
 *   - a brain, turning, coloured by the two together: blue while the
 *     machine is idle, then green, yellow and orange, and red when it is
 *     working as hard as it can
 *   - a command line of its own, which takes the shell's commands
 *
 * ctrl-shift-f10 opens it, and its command line has the keys.  esc gives
 * the keys back to whatever had them and leaves it showing.
 * ctrl-shift-f10 again closes it.
 *
 * How busy the processor is comes from the time it spends waiting for its
 * next interrupt (see idle_wait in time.c): FLOPOS halts the processor
 * whenever it has nothing to do, and counts the cycles it spent halted.
 * ===========================================================================
 */
#include "flopos.h"
#include "io.h"

extern char _bss_end[];

#define HW 26                            /* the window, in cells            */
#define HH 16
#define OUT_N 4                          /* lines of what commands said     */
#define IN_MAX (HW - 4)

volatile int g_hud_toggle;               /* ctrl-shift-f10, from the keyboard interrupt */
u32 g_idle_cycles;                       /* halted, waiting: see idle_wait  */

static int s_on, s_keys, s_running;      /* shown; its line has the keys; a command is running */
static int s_px, s_py, s_cw, s_ch;       /* where it is, and a cell, in pixels */
static int s_drawn;                      /* drawn this frame                */
static u32 s_cost;
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
        if (busy != s_cpu) { s_cpu = busy; s_ver++; }
    }
    s_t0 = now; s_idle0 = g_idle_cycles;
    {
        u32 total = (BOOTINFO_EXTKB + 1024u) * 1024u;          /* all of it, as the BIOS counted */
        u32 used = heap_used() + app_bytes() + (u32)_bss_end;
        int mem = total ? (int)(used / (total / 100 + 1)) : 0;
        if (mem > 100) mem = 100;
        if (mem != s_mem) { s_mem = mem; s_ver++; }
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
#define BARS 8
static int s_bar[BARS];

/* Eight Goertzel filters, an octave apart, over what the card is playing
 * this moment: each says how much of the sound is near its pitch. */
static void bars(void)
{
    static const u16 hz[BARS] = { 90, 180, 360, 720, 1440, 2880, 5000, 8000 };
    static u8 buf[256];
    int k, i, rate = audio_rate_now();
    if (!rate || !audio_peek(buf, (int)sizeof buf)) {
        for (k = 0; k < BARS; k++) if (s_bar[k] > 0) s_bar[k]--;
        return;
    }
    for (k = 0; k < BARS; k++) {
        s32 s1 = 0, s2 = 0, coef, p;
        int lvl = 0;
        if (hz[k] * 2 >= (u32)rate) { s_bar[k] = 0; continue; }
        coef = 2 * COS((int)((u32)hz[k] * SINLEN / (u32)rate));
        for (i = 0; i < (int)sizeof buf; i++) {
            s32 s0 = ((s32)buf[i] - 128) + ((coef * s1) >> 12) - s2;
            s2 = s1; s1 = s0;
        }
        p = (s1 >> 4) * (s1 >> 4) + (s2 >> 4) * (s2 >> 4) - (((coef * s1) >> 12) >> 4) * (s2 >> 4);
        while (p > 1) { p >>= 1; lvl++; }   /* how many doublings: loudness as the ear hears it */
        lvl -= 4;
        if (lvl < 0) lvl = 0;
        if (lvl > 12) lvl = 12;
        if (lvl > s_bar[k]) s_bar[k] = lvl;
        else if (s_bar[k] > 0) s_bar[k]--;
    }
}

/* ---- the brain ----------------------------------------------------------- */
/* One brain: a single folded ovoid, wider at the back, with a shallow
 * fissure along the top from front to back, a cerebellum tucked under the
 * back, and the stem below it.  Rings and meridians, so it is a wireframe
 * like the banana. */
#define LAT 8
#define LON 20
#define CLAT 3
#define CLON 10
#define BV (LAT * LON + CLAT * CLON + 2)
#define BE (LAT * LON + (LAT - 1) * LON + CLAT * CLON + (CLAT - 1) * CLON + 1)
static s16 s_vx[BV], s_vy[BV], s_vz[BV];
static u16 s_e0[BE], s_e1[BE];
static int s_nv, s_ne, s_built;

static void ellipsoid(int cx, int cy, int cz, int rx, int ry, int rz, int nlat, int nlon,
                      int lat0, int lat1, int cortex)
{
    int i, j, first = s_nv;
    for (i = 0; i < nlat; i++) {
        int th = lat0 + (lat1 - lat0) * i / (nlat - 1);           /* 1024ths of a turn */
        for (j = 0; j < nlon; j++) {
            int ph = j * SINLEN / nlon;
            int r = 4096, x, y, z, c;
            if (cortex) {
                r += (SIN(ph * 6 + (SIN(th * 4) >> 8)) * (s32)SIN(th * 7 + ph * 3)) >> 15;   /* the folds */
                c = COS(ph); if (c < 0) c = -c;                    /* near the middle line */
                if (SIN(th) > 0 && c < 900) r -= (900 - c) * SIN(th) / 1400;               /* the fissure */
            }
            x = (int)(((s32)COS(th) * COS(ph) >> 12) * rx >> 12) * r >> 12;
            y = (int)((s32)SIN(th) * ry >> 12) * r >> 12;
            z = (int)(((s32)COS(th) * SIN(ph) >> 12) * rz >> 12) * r >> 12;
            if (cortex && z < 0) x = x * 11 / 10;                  /* wider at the back */
            if (y < 0) y = y * 7 / 10;                              /* flat below */
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
    ellipsoid(0, 0, 0, 44, 40, 60, LAT, LON, -150, 240, 1);        /* the cerebrum   */
    ellipsoid(0, 30, -40, 28, 12, 15, CLAT, CLON, -200, 200, 0);   /* the cerebellum */
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
static char s_in[IN_MAX + 1];
static int  s_in_len;
static char s_out[OUT_N][HW + 8];       /* \1 marks a key: pink, and no room */
static char s_cur[HW + 8];
static int  s_cur_len;

static void out_line(void)
{
    int i;
    for (i = 0; i + 1 < OUT_N; i++) memcpy8(s_out[i], s_out[i + 1], sizeof s_out[0]);
    s_cur[s_cur_len] = 0;
    memcpy8(s_out[OUT_N - 1], s_cur, sizeof s_cur);
    s_cur_len = 0;
    s_ver++;
}

/* What a command prints, while it runs from here (see term_tap). */
static void out_char(int c)
{
    if (c == '\r') return;
    if (c == '\n') { out_line(); return; }
    if (c == '\t') c = ' ';
    if (c != 1) {
        int i, seen = 0;
        for (i = 0; i < s_cur_len; i++) seen += s_cur[i] != 1;
        if (seen < HW - 2) goto room;
    } else if (s_cur_len < (int)sizeof s_cur - 2) goto room;
    {
        /* Full: break at the last space, and carry the word on. */
        char rest[HW + 8];
        int i, k = s_cur_len, n = 0;
        while (k > 0 && s_cur[k - 1] != ' ') k--;
        if (k > 0 && c != ' ') {
            for (i = k; i < s_cur_len; i++) rest[n++] = s_cur[i];
            s_cur_len = k - 1;
        }
        out_line();
        for (i = 0; i < n; i++) s_cur[s_cur_len++] = rest[i];
        if (c == ' ') return;
    }
room:
    if (s_cur_len < (int)sizeof s_cur - 1) s_cur[s_cur_len++] = (char)c;
}

static void say(const char *t)
{
    while (*t) out_char((u8)*t++);
    out_char('\n');
}

static void run(void)
{
    char word[16];
    int i, k = 0, screen = 0;
    s_in[s_in_len] = 0;
    for (i = 0; s_in[i] == ' '; i++) { }
    while (s_in[i] && s_in[i] != ' ' && k < (int)sizeof word - 1) word[k++] = s_in[i++];
    word[k] = 0;
    if (!k) return;
    screen = app_exists(word) || (word[0] == 'w' && word[1] == 'i' && word[2] == 'z' && word[3] == 'a' &&
                                  word[4] == 'r' && word[5] == 'd' && !word[6]);
    out_char('>'); out_char(' ');
    for (i = 0; s_in[i]; i++) out_char((u8)s_in[i]);
    out_char('\n');
    if (screen && app_active()) {
        say("Not while an app is open.");
    } else if (screen) {
        s_keys = 0;                          /* it takes the screen, and the keys */
        s_in_len = 0;
        shell_submit(s_in);
        return;
    } else {
        int was = term_muted();
        term_tap(out_char);
        term_mute(1);
        s_running = 1;
        shell_submit(s_in);
        s_running = 0;
        term_mute(was);
        term_tap(0);
        if (s_cur_len) out_line();
    }
    s_in_len = 0;
}

int hud_takes_keys(void) { return s_on && s_keys && !s_running; }

void hud_key(int ev)
{
    int sc = KEV_SCAN(ev), c;
    s_ver++;
    if (sc == SC_ESC) { s_keys = 0; return; }
    if (sc == SC_ENTER) { run(); return; }
    if (sc == SC_BKSP) { if (s_in_len) s_in_len--; return; }
    c = keymap_char(ev);
    if (c >= 32 && c != 127 && s_in_len < IN_MAX) s_in[s_in_len++] = (char)c;
}

/* ---- drawing ----------------------------------------------------------- */
static void cell(u8 *dst, int cx, int cy, int c, u8 fg)
{
    term_overlay_cell(dst, s_px + cx * s_cw, s_py + cy * s_ch, c, fg, WIN_PAPER);
}

static void text(u8 *dst, int cx, int cy, const char *t, u8 fg)
{
    int key = 0;
    for (; *t && cx < HW - 1; t++) {
        if (*t == 1) { key = !key; continue; }           /* a key: pink */
        cell(dst, cx++, cy, (u8)*t, key ? C_KEY : fg);
    }
}

static void toggle(void)
{
    s_ver++;
    if (!s_on) {
        s_on = 1; s_keys = 1; s_batt_at = 0;
        if (!s_out[OUT_N - 1][0] && !s_out[OUT_N - 2][0]) {
            say("Commands, as at here>.");
            say("\1esc\1 gives the keys back.");
        }
    } else {
        s_on = 0; s_keys = 0;
        term_refresh();                      /* what was under it, drawn again */
        serial_printf("[hud] 'Thunking %u%%, 'Membering: %u%%, battery %d; it took %u us a frame, the text %u, sending %u\n",
                      (u32)s_cpu, (u32)s_mem, s_batt, g_cpu_mhz ? s_cost / g_cpu_mhz : 0, perf_us(1), perf_us(2));
    }
}

/* The frame and the words, into dst: drawn again only when one of them
 * changes, into a strip of its own (see hud_frame). */
static void paint(u8 *dst)
{
    int x, y, k;
    char v[HW];

    /* The frame, with a rule above the command line, on paper. */
    for (y = 0; y < HH; y++)
        for (x = 0; x < HW; x++) {
            int c = ' ';
            int top = y == 0, bot = y == HH - 1, rule = y == HH - OUT_N - 3, lft = x == 0, rgt = x == HW - 1;
            if ((top || bot || rule) && !lft && !rgt) c = CH_FRAME | FR_LEFT | FR_RIGHT;
            else if ((lft || rgt) && !top && !bot && !rule) c = CH_FRAME | FR_UP | FR_DOWN;
            else if (top && lft) c = CH_FRAME | FR_RIGHT | FR_DOWN;
            else if (top && rgt) c = CH_FRAME | FR_LEFT | FR_DOWN;
            else if (bot && lft) c = CH_FRAME | FR_RIGHT | FR_UP;
            else if (bot && rgt) c = CH_FRAME | FR_LEFT | FR_UP;
            else if (rule && lft) c = CH_FRAME | FR_UP | FR_DOWN | FR_RIGHT;
            else if (rule && rgt) c = CH_FRAME | FR_UP | FR_DOWN | FR_LEFT;
            cell(dst, x, y, c, C_GREY);
        }
    text(dst, 2, 0, " HUD ", C_DIM);

    /* The battery. */
    if (s_batt >= 0) {
        fmt_str(v, sizeof v, "battery %u%%", (u32)s_batt);
        text(dst, 13, 1, v, C_BRIGHT);
        text(dst, 13, 2, s_charging ? "charging" : s_ac == 1 ? "on the mains" : s_ac == 0 ? "on battery" : "", C_DIM);
    } else {
        text(dst, 13, 1, "no battery", C_DIM);
        text(dst, 13, 2, "reading", C_DIM);
    }

    /* The processor and the memory. */
    text(dst, 1, 4, "'Thunking", C_TEXT);
    fmt_str(v, sizeof v, "%u%%", (u32)s_cpu); text(dst, 16 - strlen8(v), 4, v, C_BRIGHT);
    text(dst, 1, 5, "'Membering:", C_TEXT);
    fmt_str(v, sizeof v, "%u%%", (u32)s_mem); text(dst, 16 - strlen8(v), 5, v, C_BRIGHT);

    /* What commands said, and the line being typed. */
    for (k = 0; k < OUT_N; k++) text(dst, 1, HH - OUT_N - 2 + k, s_out[k], C_TEXT);
    cell(dst, 1, HH - 2, '>', s_keys ? C_ACCENT : C_DIM);
    for (k = 0; k < s_in_len; k++) cell(dst, 3 + k, HH - 2, (u8)s_in[k], s_keys ? C_BRIGHT : C_DIM);
}

void hud_frame(u8 *dst, u32 t)
{
    static u8 *strip;                        /* the frame and words, as last drawn */
    static u32 painted;
    int vw = vga_width(), vh = vga_height(), pitch = term_pitch(), k, px, py;
    u32 c0;

    s_drawn = 0;
    if (g_hud_toggle) { g_hud_toggle = 0; toggle(); }
    if (!s_on) return;
    c0 = g_cpu_mhz ? tsc32() : 0;
    term_cell_size(&s_cw, &s_ch);
    px = (vw - (HW + 1) * s_cw) & ~3; if (px < 0) px = 0;
    py = s_ch;
    if (py + HH * s_ch > vh) py = 0;
    if (px != s_px || py != s_py) { s_px = px; s_py = py; s_ver++; }

    measure();
    if (!s_batt_at || ms_now() - s_batt_at > 30000) { s_batt_at = ms_now() | 1; battery(); s_ver++; }
    colours();

    /* Everything is drawn into a strip of its own, and the strip copied
     * to the screen each frame.  The frame and the words are drawn into
     * it only when they change, a few times a second at most; the bars
     * and the brain fifteen times a second. */
    if (!strip) strip = (u8 *)halloc((u32)pitch * (u32)(HH * s_ch));
    {
        static u32 moved_at;
        u8 *to = strip ? strip - s_py * pitch : dst;
        int fresh = !strip;
        if (painted != s_ver || !strip) { paint(to); painted = s_ver; fresh = 1; }
        if (fresh || ms_now() - moved_at >= 66) {
            int bx = s_px + s_cw, by = s_py + s_ch, bw = 10 * s_cw, bh = 2 * s_ch, each = bw / BARS;
            moved_at = ms_now();
            bars();
            term_overlay_fill(to, bx, by, bw, bh, WIN_PAPER);          /* the sound, as bars */
            for (k = 0; k < BARS; k++) {
                int h = s_bar[k] * bh / 12;
                if (h < 1) h = 1;
                term_overlay_fill(to, bx + k * each + 1, by + bh - h, each - 2, h, C_ACCENT);
            }
            bx = s_px + 17 * s_cw; by = s_py + 3 * s_ch;               /* the brain */
            term_overlay_fill(to, bx, by, 8 * s_cw, 6 * s_ch, WIN_PAPER);
            brain(to, pitch, bx, by, 8 * s_cw, 6 * s_ch, t);
        }
        if (strip) {
            int w4 = (HW * s_cw + 3) & ~3, y;
            for (y = 0; y < HH * s_ch; y++)
                memcpy32(dst + (s_py + y) * pitch + s_px, strip + y * pitch + s_px, (u32)w4 / 4);
        }
    }

    /* The caret. */
    if (s_keys && ((t >> 9) & 1))
        term_overlay_fill(dst, s_px + (3 + s_in_len) * s_cw, s_py + (HH - 1) * s_ch - 3, s_cw - 1, 2, C_ACCENT);
    s_drawn = 1;
    if (g_cpu_mhz) s_cost = s_cost - (s_cost >> 3) + ((tsc32() - c0) >> 3);
}

/* The rectangle it drew, for sending to the card: 0 if none. */
int hud_rect(int *x0, int *y0, int *x1, int *y1)
{
    if (!s_drawn) return 0;
    *x0 = s_px; *y0 = s_py; *x1 = s_px + HW * s_cw - 1; *y1 = s_py + HH * s_ch - 1;
    return 1;
}
