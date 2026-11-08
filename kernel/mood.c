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

}

void mood_build_palette(u8 *out) { build_palette(out); }

void mood_apply_palette(int fade_steps)
{
    build_palette(s_pal);
    if (fade_steps > 0) vga_fade_to(s_pal, fade_steps);
    else vga_set_palette(s_pal);
}

/* ---- the field ------------------------------------------------------- */
static int field(const Mood *m, int x, int y, int ph)
{
    int v, idx;

    switch (m->style) {
    case 1:                              /* slow horizontal bands          */
        v = SIN(y * 30 + ph + (SIN(x * 11 + (ph >> 2)) >> 4));
        idx = ((v + 4096) * 62) >> 13;
        break;

    case 2: {                            /* rings breathing outward        */
        int r = s_rad[y * MSTRIDE + x] >> 4;
        v = SIN(r * 24 - ph * 2) + SIN(r * 9 + ph);
        idx = ((v + 8192) * 31) >> 13;
        break;
    }

    case 3:                              /* almost nothing: the void       */
        /* Low frequencies and a wide spread of indices, because with only
         * a handful of levels this banded into hard diagonal edges. */
        v = SIN(y * 7 + ph / 4)
          + SIN(x * 4 - ph / 6)
          + SIN((x + y) * 3 + ph / 8);
        idx = ((v + 12288) * 21) >> 13;
        break;

    default:                             /* summed sines                   */
        v = SIN(x * 13 + ph)
          + SIN(y * 21 - ph)
          + SIN((x + y) * 9 + (ph >> 1));
        idx = ((v + 12288) * 21) >> 13;
        break;
    }

    if (idx < 0) idx = 0;
    if (idx > PAL_MOOD_N - 1) idx = PAL_MOOD_N - 1;
    return PAL_MOOD + idx;
}

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
