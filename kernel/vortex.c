/* ===========================================================================
 * vortex.c -- whaaat.
 *
 * The screen as it stands -- terminal, text, cursor, everything -- is
 * photographed, then dragged through a hole in the middle of itself and
 * turned inside out, and what you are dropped back into is the same shell
 * in the negative of the colours it had.
 *
 * It is a backwards warp: for every pixel of the output, work out where it
 * must have come from.  That is the only way round that leaves no holes.
 * Each destination pixel's radius and angle from the centre are worked out
 * once, into tables, because the per-frame job is then two table lookups,
 * two multiplies and two shifts -- about eight milliseconds for the whole
 * screen on a 120MHz Pentium, which leaves room for the blit.
 *
 * Three movements:
 *
 *   draw in   the sampling radius grows, so the picture is pulled towards
 *             the middle, twisting harder the closer it gets
 *   turn      the radius mapping is inverted: what was at the centre is
 *             now at the edge, and the screen is inside out
 *   settle    the inversion relaxes back to an ordinary view, untwisting,
 *             while the palette crosses over to its negative
 * ===========================================================================
 */
#include "flopos.h"
#include "io.h"

#define CX (SCR_W / 2)
#define CY (SCR_H / 2)
#define RMAX 208                         /* corner distance, near enough   */

static u16 *s_rad;                       /* per destination pixel          */
static u16 *s_ang;
static u8  *s_snap;                      /* the screen as it was           */
static int  s_built;

static void build_tables(void)
{
    int x, y;

    if (s_built) return;
    s_rad = (u16 *)halloc(SCR_PIX * sizeof(u16));
    s_ang = (u16 *)halloc(SCR_PIX * sizeof(u16));
    s_snap = (u8 *)halloc(SCR_PIX);

    for (y = 0; y < SCR_H; y++) {
        for (x = 0; x < SCR_W; x++) {
            int dx = x - CX;
            /* Mode 13h pixels are taller than they are wide, so the
             * vertical distance is scaled or the vortex comes out as an
             * ellipse. */
            int dy = ((y - CY) * 6) / 5;
            int i = y * SCR_W + x;
            s_rad[i] = (u16)isqrt32((u32)(dx * dx + dy * dy));
            s_ang[i] = (u16)iatan2(dy, dx);
        }
    }
    s_built = 1;
}

/* One frame of the warp.
 *
 * pull is the depth of the hole, 0 for none and 512 for as far as this
 * goes.  It bends the radius towards r squared and then towards r cubed,
 * which has the property that matters: the outermost ring maps to itself,
 * so the edges stay pinned and the screen stays full while everything
 * nearer the middle is magnified and smeared inwards.  Scaling the radius
 * uniformly instead would simply shrink the picture and leave black
 * around it, which looks like the image retreating rather than the middle
 * falling in.
 *
 * twist pushes the angle, hardest at the centre.  invert turns the radius
 * mapping inside out: what was at the middle ends up at the edge.
 */
static void warp(u8 *dst, const u8 *src, int pull, int twist, int invert)
{
    int i, x, y;

    for (y = 0; y < SCR_H; y++) {
        for (x = 0; x < SCR_W; x++) {
            int r, a, rr, sq, sx, sy;

            i = y * SCR_W + x;
            r = s_rad[i];
            a = s_ang[i];
            if (r > RMAX) r = RMAX;

            sq = (r * r) / RMAX;
            if (pull <= 256) {
                rr = r + (((sq - r) * pull) >> 8);
            } else {
                int cu = (sq * r) / RMAX;
                rr = sq + (((cu - sq) * (pull - 256)) >> 8);
            }

            if (invert) {
                int inside = RMAX - rr;
                rr = rr + (((inside - rr) * invert) >> 8);
            }
            if (rr < 0) rr = 0;

            /* Twist hardest where the hole is. */
            a += (twist * (RMAX - (r < RMAX ? r : RMAX))) >> 8;

            sx = CX + ((COS(a) * rr) >> 12);
            sy = CY + (((SIN(a) * rr) >> 12) * 5) / 6;

            if ((unsigned)sx >= SCR_W || (unsigned)sy >= SCR_H)
                dst[i] = 0;
            else
                dst[i] = src[sy * SCR_W + sx];
        }
    }
}

/* ---- the whole performance ------------------------------------------- */
#define SPAN_MS 2300

void vortex_run(void)
{
    static u8 pal_from[768], pal_to[768], pal_mix[768];
    u32 t0;
    int i;

    build_tables();
    if (!s_rad || !s_ang || !s_snap) return;

    /* Photograph the screen exactly as the user left it. */
    memcpy32(s_snap, g_back, SCR_PIX / 4);

    vga_get_palette(pal_from);
    for (i = 0; i < 768; i++) pal_to[i] = (u8)(63 - pal_from[i]);

    snd_sweep_begin();
    t0 = ms_now();

    for (;;) {
        u32 el = ms_now() - t0;
        int p, pull, twist, invert, mix;

        if (el >= SPAN_MS) break;
        p = (int)((el * 1024) / SPAN_MS);          /* 0..1023              */

        if (p < 560) {                             /* draw in              */
            pull   = (p * 512) / 560;
            twist  = (p * 700) / 560;
            invert = 0;
            mix    = 0;
        } else if (p < 760) {                      /* turn                 */
            int q = p - 560;                       /* 0..199               */
            pull   = 512;
            twist  = 700;
            invert = (q * 256) / 200;
            mix    = (q * 210) / 200;
        } else {                                   /* settle               */
            int q = p - 760;                       /* 0..263               */
            pull   = 512 - (q * 512) / 264;
            twist  = 700 - (q * 700) / 264;
            invert = 256 - (q * 256) / 264;
            mix    = 210 + (q * 46) / 264;
        }

        /* The sound follows the hole: down as it draws in, up and out of
         * hearing as it turns. */
        snd_sweep_set(p < 560 ? 900 - (p * 740) / 560
                              : 160 + ((p - 560) * 2600) / 464);

        for (i = 0; i < 768; i++)
            pal_mix[i] = (u8)(pal_from[i] + (((pal_to[i] - pal_from[i]) * mix) >> 8));

        warp(g_back, s_snap, pull, twist, invert);
        vga_set_palette(pal_mix);
        vga_present();
    }

    snd_sweep_end();
    vga_set_palette(pal_to);
}
