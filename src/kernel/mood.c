/* ===========================================================================
 * mood.c -- what the place looks like.
 *
 * The background is computed at 80x50 and blown up four times, which costs
 * 4000 pixel evaluations and one pass of 32-bit stores for the whole
 * screen.  Cheap enough to run every frame on a 120MHz Pentium with the
 * whole terminal redrawn on top of it.
 *
 * It is deliberately dark.  Indices 1..63 are the only ones the background
 * ever uses and that ramp tops out around a third brightness, so text in
 * the upper ramps always wins.  The beat from sound.c nudges the ramp up a
 * few steps, which is what makes the room breathe.
 * ===========================================================================
 */
#include "flopos.h"
#include "io.h"

#define MW 80                            /* background grid                */
#define MH 50
#define MSTRIDE (MW + 1)                 /* one extra sample each way, so
                                          * interpolation has something to
                                          * reach towards at the edges     */
/* How bright each place is allowed to get.  Quantisation is the reason
 * this is per-mood rather than one constant: a field that only reaches a
 * third of its ramp lands several indices on the same RGB and bands into
 * visible edges, so every style spans the whole ramp and the ceiling does
 * the dimming instead. */

typedef struct {
    const char *name;
    s16  hue;                            /* brads: 0 red, 341 green, 682 blue */
    s16  hue2;
    u8   style;                          /* 0 plasma 1 bands 2 rings 3 dust */
    u8   ceiling;                        /* brightest this place may get    */
} Mood;

static const Mood s_moods[] = {
    { "STILL",  682, 790, 0, 20 },
    { "WARM",    40,   8, 1, 24 },
    { "DEEP",   400, 330, 2, 22 },
    { "DUSK",   880,  50, 0, 24 },
    { "VOID",   720, 640, 3, 11 },
};
#define MOOD_N ((int)(sizeof s_moods / sizeof s_moods[0]))

static int  s_which;
static int  s_deep;
static u16 *s_rad;                       /* radius table for the rings     */
static u8  *s_rowa, *s_rowb;             /* two source rows, interpolated  */
static u8   s_pal[768];

/* Dust, for the VOID. */
#define DUST_N 72
static struct { s16 x, y, vx, vy; u8 c; } s_dust[DUST_N];

/* Hue angle to RGB, 0..63 per gun, straight off the sine table. */
static void hue_rgb(int a, int *r, int *g, int *b)
{
    *r = (COS(a)       + 4096) * 63 >> 13;
    *g = (COS(a - 341) + 4096) * 63 >> 13;
    *b = (COS(a - 682) + 4096) * 63 >> 13;
}

static void ramp(u8 *p, int start, int n,
                 int r0, int g0, int b0, int r1, int g1, int b1)
{
    int i, d = n > 1 ? n - 1 : 1;
    for (i = 0; i < n; i++) {
        p[(start + i) * 3 + 0] = (u8)(r0 + ((r1 - r0) * i) / d);
        p[(start + i) * 3 + 1] = (u8)(g0 + ((g1 - g0) * i) / d);
        p[(start + i) * 3 + 2] = (u8)(b0 + ((b1 - b0) * i) / d);
    }
}

void mood_init(void)
{
    int x, y, i;

    s_rad = (u16 *)halloc(MSTRIDE * (MH + 1) * sizeof(u16));
    for (y = 0; y <= MH; y++) {
        for (x = 0; x <= MW; x++) {
            int dx = x - MW / 2;
            int dy = y - MH / 2;
            s_rad[y * MSTRIDE + x] =
                (u16)(isqrt32((u32)(dx * dx + dy * dy)) * 16);
        }
    }
    s_rowa = (u8 *)halloc(SCR_W);
    s_rowb = (u8 *)halloc(SCR_W);

    for (i = 0; i < DUST_N; i++) {
        s_dust[i].x = (s16)rnd_range(0, SCR_W - 1);
        s_dust[i].y = (s16)rnd_range(0, SCR_H - 1);
        s_dust[i].vx = (s16)rnd_range(-2, 2);
        s_dust[i].vy = (s16)rnd_range(-1, 1);
        s_dust[i].c = (u8)(PAL_ACCENT + rnd_range(4, PAL_ACCENT_N - 1));
    }

    s_which = 0;
}

int  mood_get(void)   { return s_which; }
int  mood_deep(void) { return s_deep; }
void mood_set_deep(int on) { s_deep = on ? 1 : 0; }
int  mood_count(void) { return MOOD_N; }

const char *mood_name(int which)
{
    if (which < 0 || which >= MOOD_N) return "?";
    return s_moods[which].name;
}

void mood_set(int which)
{
    s_which = ((which % MOOD_N) + MOOD_N) % MOOD_N;
}

/* The frames' neon: sixteen full colours round the wheel, turned a
 * little further each time it is asked for. */
/* The colours no mood changes. */
static void fixed_colours(u8 *p)
{
    static const u8 c[][4] = {
        { PAL_KEY, 63, 26, 46 },         /* keys: pink */
        { PAL_ON, 4, 50, 10 },           /* a setting that is on: a dark neon green */
        { PAL_RED, 63, 12, 14 },
        { PAL_CREAM, 63, 61, 52 },
        { PAL_OLIVE, 36, 30, 8 },
        { PAL_GOLD, 63, 46, 2 },
        { PAL_EMBER, 46, 6, 4 },
        { PAL_GREEN_D, 1, 16, 4 },
    };
    u32 i;
    for (i = 0; i < sizeof c / sizeof c[0]; i++) {
        p[c[i][0] * 3] = c[i][1]; p[c[i][0] * 3 + 1] = c[i][2]; p[c[i][0] * 3 + 2] = c[i][3];
    }
}

/* Frames' loop of colours: neon, or for a program that asks, 1: red to
 * gold to green and round again, keys yellow with it (maelcum); 2: ice,
 * cyan, teal and blue, a glint of silver, and black (browzer).  The loop
 * turns in the palette, so its shine costs no drawing at all. */
static int s_cool;
void mood_ember(int on)
{
    s_cool = on;
    g_key_colour = on == 1 ? PAL_BANANA + 6 : PAL_KEY;
}

static void neon_fill(u8 *rgb, u32 t)
{
    int i, r, g, b;
    for (i = 0; i < PAL_NEON_N; i++) {
        int a = (int)((t >> 4) + (u32)i * (1024 / PAL_NEON_N)) & 1023;
        if (s_cool) {
            static const u8 ember[4][3] = { { 63, 6, 4 }, { 63, 50, 2 }, { 4, 50, 10 }, { 63, 6, 4 } };
            /* mostly cyan, teal and blue; one glint of silver, then black */
            static const u8 ice[9][3] = { { 4, 63, 63 }, { 0, 40, 38 }, { 6, 22, 63 }, { 0, 30, 34 },
                                          { 4, 63, 63 }, { 6, 22, 63 }, { 48, 52, 58 }, { 2, 3, 8 },
                                          { 4, 63, 63 } };
            const u8 (*at)[3] = s_cool == 1 ? ember : ice;
            int n = s_cool == 1 ? 3 : 8;
            int seg = a * n >> 10, f = (a * n & 1023) >> 2, k;      /* which stretch, and 0..255 along it */
            for (k = 0; k < 3; k++) rgb[i * 3 + k] = (u8)(at[seg][k] + (at[seg + 1][k] - at[seg][k]) * f / 256);
            continue;
        }
        /* Full strength, unlike the moods' soft hues: twice the swing, cut off at the ends. */
        r = (COS(a) * 2 + 4096) * 63 >> 13;
        g = (COS(a - 341) * 2 + 4096) * 63 >> 13;
        b = (COS(a - 682) * 2 + 4096) * 63 >> 13;
        r = r < 0 ? 0 : r > 63 ? 63 : r; g = g < 0 ? 0 : g > 63 ? 63 : g; b = b < 0 ? 0 : b > 63 ? 63 : b;
        rgb[i * 3] = (u8)r; rgb[i * 3 + 1] = (u8)g; rgb[i * 3 + 2] = (u8)b;
    }
}

void mood_neon(u32 t)
{
    static u32 at;
    u8 rgb[PAL_NEON_N * 3];
    if (t - at < 66) return;                 /* fifteen times a second */
    at = t;
    neon_fill(rgb, t);
    vga_set_colours(PAL_NEON, PAL_NEON_N, rgb);
}

/* The far side of the vortex.
 *
 * Inverting the palette was the obvious thing and it was wrong: the
 * negative of a dark room is a bright one, and bright is the opposite of
 * what is wanted.  This is a different palette rather than a flipped one
 * -- cold, under pressure, almost no light in it, and what light there is
 * arriving from a long way above.  Nothing in here reaches white.
 *
 * The place keeps whichever mood it was in; this drowns it.
 */
static void build_deep(u8 *p, const Mood *m)
{
    int i, r, g, b;

    p[0] = 0; p[1] = 1; p[2] = 3;        /* not quite black, even here */

    /* The field barely lifts off black, and what colour it has leans
     * towards the green of water rather than the hue of the room. */
    for (i = 0; i < PAL_MOOD_N; i++) {
        int t = i;
        int a = m->hue + ((m->hue2 - m->hue) * t) / (PAL_MOOD_N - 1);
        int f = (t * 15) / (PAL_MOOD_N - 1);
        hue_rgb(a, &r, &g, &b);
        p[(PAL_MOOD + i) * 3 + 0] = (u8)((r * f) / 200);
        p[(PAL_MOOD + i) * 3 + 1] = (u8)(((g + 40) * f) / 110);
        p[(PAL_MOOD + i) * 3 + 2] = (u8)(((b + 70) * f) / 95);
    }

    ramp(p, PAL_SHADOW, PAL_SHADOW_N,  0, 0, 1,    1, 4, 6);
    ramp(p, PAL_TEXT,   PAL_TEXT_N,    5, 13, 16,  30, 49, 54);
    ramp(p, PAL_BRIGHT, PAL_BRIGHT_N, 14, 28, 33,  44, 60, 62);
    ramp(p, PAL_ACCENT, PAL_ACCENT_N,  1,  9,  8,  18, 54, 40);
    ramp(p, PAL_PROMPT, PAL_PROMPT_N,  1,  6, 11,  14, 42, 52);
    ramp(p, PAL_CURSOR, PAL_CURSOR_N,  3, 13, 16,  34, 56, 60);
    ramp(p, PAL_GREY,   PAL_GREY_N,    1,  3,  5,  15, 23, 26);
    /* Still yellow down here, just colder and further off. */
    ramp(p, PAL_BANANA, PAL_BANANA_N,  8,  8,  4,  44, 46, 20);
    neon_fill(p + PAL_NEON * 3, ms_now());
    fixed_colours(p);
}

static void build_palette(u8 *p)
{
    const Mood *m = &s_moods[s_which];
    int i, r, g, b;

    if (s_deep) { build_deep(p, m); return; }

    p[0] = p[1] = p[2] = 0;              /* index 0 is always true black   */
    (void)r; (void)g; (void)b;

    /* The field: black up to the place's colour, blending hue to hue2 and
     * capped well short of the text ramps. */
    for (i = 0; i < PAL_MOOD_N; i++) {
        int t = i;
        int a = m->hue + ((m->hue2 - m->hue) * t) / (PAL_MOOD_N - 1);
        int f = (t * m->ceiling) / (PAL_MOOD_N - 1);
        hue_rgb(a, &r, &g, &b);
        p[(PAL_MOOD + i) * 3 + 0] = (u8)((r * f) / 63);
        p[(PAL_MOOD + i) * 3 + 1] = (u8)((g * f) / 63);
        p[(PAL_MOOD + i) * 3 + 2] = (u8)((b * f) / 63);
    }

    /* Text is near-neutral on purpose: it has to stay legible whatever the
     * room is doing, so it does not take the place's hue. */
    ramp(p, PAL_SHADOW, PAL_SHADOW_N,  0, 0, 0,   7, 5, 10);
    ramp(p, PAL_TEXT,   PAL_TEXT_N,   10, 11, 13, 55, 56, 58);
    ramp(p, PAL_BRIGHT, PAL_BRIGHT_N, 28, 29, 32, 63, 63, 63);

    hue_rgb(m->hue + 512, &r, &g, &b);   /* the complementary hue          */
    ramp(p, PAL_ACCENT, PAL_ACCENT_N, r / 5, g / 5, b / 5, r, g, b);

    hue_rgb(m->hue, &r, &g, &b);
    ramp(p, PAL_PROMPT, PAL_PROMPT_N, r / 4, g / 4, b / 4, r, g, b);

    hue_rgb(m->hue + 512, &r, &g, &b);
    ramp(p, PAL_CURSOR, PAL_CURSOR_N, r / 3, g / 3, b / 3, 63, 63, 63);

    ramp(p, PAL_GREY, PAL_GREY_N, 6, 6, 8, 34, 34, 38);

    /* Its own ramp, because no part of a mood palette is ever yellow and
     * a banana is not negotiable about this. */
    ramp(p, PAL_BANANA, PAL_BANANA_N, 16, 12, 2, 63, 60, 20);
    ramp(p, PAL_HUD, PAL_HUD_N, 2, 4, 16, 10, 20, 63);
    neon_fill(p + PAL_NEON * 3, ms_now());
    fixed_colours(p);

}

void mood_build_palette(u8 *out) { build_palette(out); }

void mood_apply_palette(int fade_steps)
{
    build_palette(s_pal);
    if (fade_steps > 0) vga_fade_to(s_pal, fade_steps);
    else vga_set_palette(s_pal);
}

/* ---- the field ------------------------------------------------------- */
/* The field at one grid point, as a palette index with eight bits of
 * fraction: the big screen dithers between neighbouring shades, and the
 * fraction is what lets a gradient and its drift be smooth there. */
static int field8(const Mood *m, int x, int y, int ph)
{
    int v, idx;

    switch (m->style) {
    case 1:                              /* slow horizontal bands          */
        v = SIN(y * 30 + ph + (SIN(x * 11 + (ph >> 2)) >> 4));
        idx = ((v + 4096) * 62) >> 5;
        break;

    case 2: {                            /* rings breathing outward        */
        int r = s_rad[y * MSTRIDE + x] >> 4;
        v = SIN(r * 24 - ph * 2) + SIN(r * 9 + ph);
        idx = ((v + 8192) * 31) >> 5;
        break;
    }

    case 3:                              /* almost nothing: the void       */
        /* Low frequencies and a wide spread of indices, because with only
         * a handful of levels this banded into hard diagonal edges. */
        v = SIN(y * 7 + ph / 4)
          + SIN(x * 4 - ph / 6)
          + SIN((x + y) * 3 + ph / 8);
        idx = ((v + 12288) * 21) >> 5;
        break;

    default:                             /* summed sines                   */
        v = SIN(x * 13 + ph)
          + SIN(y * 21 - ph)
          + SIN((x + y) * 9 + (ph >> 1));
        idx = ((v + 12288) * 21) >> 5;
        break;
    }

    if (idx < 0) idx = 0;
    if (idx > ((PAL_MOOD_N - 1) << 8)) idx = (PAL_MOOD_N - 1) << 8;
    return (PAL_MOOD << 8) + idx;
}

static int field(const Mood *m, int x, int y, int ph) { return field8(m, x, y, ph) >> 8; }

/* One row of the field, evaluated at 81 points and stretched smoothly
 * across all 320 pixels.  Interpolating costs a few adds per pixel where
 * evaluating the field costs table lookups and multiplies, so this is far
 * cheaper than computing the field at full width. */
static void field_row(const Mood *m, int sy, int ph, int lift, u8 *out)
{
    int samp[MW + 1];
    int i;

    for (i = 0; i <= MW; i++) {
        int v = field(m, i, sy, ph) + lift;
        if (v > PAL_MOOD + PAL_MOOD_N - 1) v = PAL_MOOD + PAL_MOOD_N - 1;
        samp[i] = v;
    }

    for (i = 0; i < MW; i++) {
        int a = samp[i];
        int d = samp[i + 1] - a;
        u8 *o = out + i * 4;
        o[0] = (u8)a;
        o[1] = (u8)(a + (d >> 2));
        o[2] = (u8)(a + (d >> 1));
        o[3] = (u8)(a + ((d * 3) >> 2));
    }
}

void mood_draw(u8 *dst, u32 t)
{
    const Mood *m = &s_moods[s_which];
    int ph = (int)(t >> (s_deep ? 6 : 4));   /* slower, down there */
    int lift = g_beat >> (s_deep ? 7 : 5);              /* the room breathes with the beat */
    u8 *rowa = s_rowa, *rowb = s_rowb, *swap;
    int sy, w, x, i;

    /* Bilinear, both ways.  Block-replicating 80x50 up to 320x200 left
     * four-pixel squares, and a panel stretching that again by two and a
     * half turned them into ten-pixel squares that visibly stepped as the
     * field drifted.  Interpolating costs about two milliseconds, which
     * comes straight out of the time this loop spent idle waiting for
     * vertical retrace, so it is free in frames per second. */
    field_row(m, 0, ph, lift, rowa);

    for (sy = 0; sy < MH; sy++) {
        u8 *base = dst + (sy * 4) * SCR_W;

        field_row(m, sy + 1, ph, lift, rowb);

        memcpy32(base, rowa, SCR_W / 4);          /* weight 0: exact       */
        for (w = 1; w <= 3; w++) {
            u8 *o = base + w * SCR_W;
            for (x = 0; x < SCR_W; x++) {
                int a = rowa[x];
                o[x] = (u8)(a + (((rowb[x] - a) * w) >> 2));
            }
        }

        swap = rowa; rowa = rowb; rowb = swap;
    }

    if (m->style != 3) return;

    /* Dust drifts at full resolution, one pixel each, so it does not get
     * the blocky four-times treatment the field gets. */
    for (i = 0; i < DUST_N; i++) {
        s_dust[i].x = (s16)(s_dust[i].x + s_dust[i].vx);
        s_dust[i].y = (s16)(s_dust[i].y + s_dust[i].vy);
        if (s_dust[i].x < 0) s_dust[i].x += SCR_W;
        if (s_dust[i].x >= SCR_W) s_dust[i].x -= SCR_W;
        if (s_dust[i].y < 0) s_dust[i].y += SCR_H;
        if (s_dust[i].y >= SCR_H) s_dust[i].y -= SCR_H;
        dst[s_dust[i].y * SCR_W + s_dust[i].x] = s_dust[i].c;
    }
}

/* ---- the big screen ------------------------------------------------------
 * At 800x600 the background is drawn straight onto the big canvas rather
 * than drawn small and stretched two and a half times, which made blocks
 * and, as it drifted, steps.  The same 81x51 points of the field, each
 * with a fraction, are interpolated across the whole screen, and each
 * pixel is nudged by a 4x4 ordered dither before it is cut to a palette
 * index.  The dither reaches across one step of brightness the card can
 * actually show -- the ramp has 47 entries but only about twenty distinct
 * levels -- so neighbouring levels mix into a gradient with no bands, and
 * a band edge that drifts moves by a pixel at a time instead of jumping.
 * About the same work per pixel as stretching did. */
static int *s_sa, *s_sb, *s_line;        /* from the heap, with s_bg: the kernel's own memory is full */

static void field_samples(const Mood *m, int sy, int ph, int lift8, int lo, int hi, int *out)
{
    int i;
    for (i = 0; i <= MW; i++) {
        int v = field8(m, i, sy, ph) + lift8;
        out[i] = v < lo ? lo : v > hi ? hi : v;
    }
}

static u8  *s_bg;                        /* the big background, kept between frames */
static int  s_bg_half;                   /* which half is worked out next  */
static int *s_lv;                        /* one row's values, before the dither: 400 */

void mood_draw_big(u8 *dst, int W, int H, u32 t)
{
    static const u8 bayer[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };
    const Mood *m = &s_moods[s_which];
    static int ph, lift8;                 /* one moment for both halves      */
    int levels = s_deep ? 15 : m->ceiling;
    int amp = ((PAL_MOOD_N - 1) << 8) / (levels > 0 ? levels : 1);
    int hw = W / 2, hh = H / 2;                           /* worked out at half size */
    int lo, hi, cw = hw / MW, ch = hh / MH, sy, sy0, sy1, py, i, k, x;
    int off[4][4];
    int rcw, rch;
    int *sa, *sb, *swap;

    if (cw < 1 || ch < 1 || (W & 7) || hw > 400) return;
    if (!s_bg) {
        s_bg = (u8 *)halloc((usize)(W * H));
        memset8(s_bg, PAL_MOOD, (usize)(W * H));
        s_sa = (int *)halloc(3 * (MW + 1) * sizeof(int) + 400 * sizeof(int));
        s_sb = s_sa + MW + 1;
        s_line = s_sb + MW + 1;
        s_lv = s_line + MW + 1;
    }
    sa = s_sa; sb = s_sb;
    if (amp > 3 * 256) amp = 3 * 256;
    for (i = 0; i < 4; i++)
        for (k = 0; k < 4; k++) off[i][k] = ((bayer[i][k] * 16 + 8 - 128) * amp) >> 8;
    lo = (PAL_MOOD << 8) + amp / 2;
    hi = ((PAL_MOOD + PAL_MOOD_N) << 8) - 1 - amp / 2;
    rcw = 65536 / cw;
    rch = 65536 / ch;

    /* Half the rows a frame, top then bottom: the field drifts slowly
     * enough that each half being a frame apart never shows, and it halves
     * the work.  Each value is a 2x2 block -- two pixels in one store and
     * the row copied down -- too fine to see at 800x600 once dithered. */
    if (!s_bg_half) {                     /* the top half: a new moment     */
        ph = (int)(t >> (s_deep ? 6 : 4));
        lift8 = (g_beat << 8) >> (s_deep ? 7 : 5);
    }
    sy0 = s_bg_half ? MH / 2 : 0;
    sy1 = s_bg_half ? MH : MH / 2;
    s_bg_half = !s_bg_half;
    field_samples(m, sy0, ph, lift8, lo, hi, sa);
    for (sy = sy0; sy < sy1; sy++) {
        field_samples(m, sy + 1, ph, lift8, lo, hi, sb);
        for (py = 0; py < ch; py++) {
            int y = sy * ch + py;
            int wy = py * rch;                            /* 0..65535 */
            const int *o = off[y & 3];
            u32 *row = (u32 *)(s_bg + 2 * y * W);
            int o0 = o[0], o1 = o[1], o2 = o[2], o3 = o[3];
            for (i = 0; i <= MW; i++) s_line[i] = sa[i] + (((sb[i] - sa[i]) * wy) >> 16);
            for (i = 0, x = 0; i < MW; i++) {             /* the values, straight lines between points */
                int v = s_line[i];
                int dv = ((s_line[i + 1] - v) * rcw) >> 16;
                for (k = 0; k < cw; k++, v += dv) s_lv[x++] = v;
            }
            for (x = 0; x < hw; x += 4) {                 /* then the dither, four at a time */
                u32 a = (u32)((s_lv[x] + o0) >> 8) & 0xFF, b = (u32)((s_lv[x + 1] + o1) >> 8) & 0xFF;
                u32 c = (u32)((s_lv[x + 2] + o2) >> 8) & 0xFF, d = (u32)((s_lv[x + 3] + o3) >> 8) & 0xFF;
                row[x >> 1]       = (a | (b << 16)) * 0x101u;
                row[(x >> 1) + 1] = (c | (d << 16)) * 0x101u;
            }
            memcpy32((u8 *)row + W, row, (u32)W / 4);
        }
        swap = sa; sa = sb; sb = swap;
    }
    memcpy32(dst, s_bg, (u32)(W * H) / 4);

    if (m->style != 3) return;
    for (i = 0; i < DUST_N; i++) {                        /* dust: a speck, two pixels across */
        int x = s_dust[i].x * W / SCR_W, y = s_dust[i].y * H / SCR_H;
        s_dust[i].x = (s16)(s_dust[i].x + s_dust[i].vx);
        s_dust[i].y = (s16)(s_dust[i].y + s_dust[i].vy);
        if (s_dust[i].x < 0) s_dust[i].x += SCR_W;
        if (s_dust[i].x >= SCR_W) s_dust[i].x -= SCR_W;
        if (s_dust[i].y < 0) s_dust[i].y += SCR_H;
        if (s_dust[i].y >= SCR_H) s_dust[i].y -= SCR_H;
        if (x >= 0 && x + 1 < W && y >= 0 && y + 1 < H) {
            dst[y * W + x] = dst[y * W + x + 1] = s_dust[i].c;
            dst[(y + 1) * W + x] = dst[(y + 1) * W + x + 1] = s_dust[i].c;
        }
    }
}
