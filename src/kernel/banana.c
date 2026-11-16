/* ===========================================================================
 * banana.c -- a banana, in wireframe, turning.
 *
 * Built rather than drawn: a spine that curves through an arc, with a
 * tapering elliptical cross-section swept along it.  Rings around, lines
 * along, nine by six, which is as much mesh as seventy-odd pixels of
 * corner can show without becoming a smudge.
 *
 * The cross-sections are placed on a proper frame -- tangent, in-plane
 * normal, and binormal -- so the rings stay perpendicular to the spine as
 * it bends.  Sweeping circles in a fixed plane instead would give
 * something that reads as a bent tube seen from one angle and a mess from
 * every other -- which matters more now it turns on two axes at once and
 * is seen from all of them.
 *
 * Fixed point throughout at 1.0 == 256 for the model, and the sine table's
 * 1.0 == 4096 for the rotation.  Nothing here touches the FPU.
 * ===========================================================================
 */
#include "flopos.h"
#include "io.h"

#define NR    9                          /* rings along the spine          */
#define NS    6                          /* points around each ring        */
#define NV    (NR * NS)
#define NE    (NR * NS + (NR - 1) * NS)

#define BOX_X 238                        /* the corner it lives in         */
#define BOX_Y 3
#define BOX_W 79
#define BOX_H 74

#define FOCAL 132
#define DIST  420

static s16 s_vx[NV], s_vy[NV], s_vz[NV];
static u8  s_e0[NE], s_e1[NE];
static int s_ne;
static int s_built;

static void build(void)
{
    int i, j;

    s_ne = 0;

    for (i = 0; i < NR; i++) {
        /* Along the spine, kept off the very tips so the end rings do not
         * collapse to a single point. */
        int t   = 24 + (i * (1024 - 48)) / (NR - 1);        /* 0..1024     */
        int arc = ((t - 512) * 180) / 512;                  /* brads       */
        int nx, ny;

        /* The spine: an arc, so the thing has a bend in it.  Offset so the
         * curve sits centred rather than hanging off the bottom. */
        int sx = (SIN(arc) * 90) >> 12;
        int sy = -((COS(arc) * 90) >> 12) + 67;

        /* Fat in the middle, blunt rather than needle-pointed at the ends. */
        int rad = 6 + ((SIN((t * 512) / 1024) * 21) >> 12);

        /* The frame the cross-section is drawn on.  The normal is
         * perpendicular to the spine, not along it -- sweeping the rings
         * down the tangent instead gives a flat lens that only reads as
         * anything from one angle. */
        nx = -((SIN(arc) * 1024) >> 12);
        ny =  ((COS(arc) * 1024) >> 12);

        for (j = 0; j < NS; j++) {
            int th = (j * SINLEN) / NS;
            int cn = (COS(th) * rad) >> 12;      /* along the normal       */
            int cb = (SIN(th) * rad) >> 12;      /* along the binormal     */
            int k = i * NS + j;

            /* Squashed a little across the bend, the way a banana is. */
            s_vx[k] = (s16)(sx + ((cn * nx) >> 10) * 4 / 5);
            s_vy[k] = (s16)(sy + ((cn * ny) >> 10) * 4 / 5);
            s_vz[k] = (s16)cb;

            /* Around. */
            s_e0[s_ne] = (u8)k;
            s_e1[s_ne] = (u8)(i * NS + ((j + 1) % NS));
            s_ne++;

            /* And along, except from the last ring. */
            if (i < NR - 1) {
                s_e0[s_ne] = (u8)k;
                s_e1[s_ne] = (u8)((i + 1) * NS + j);
                s_ne++;
            }
        }
    }
    s_built = 1;
}

/* The corners on a screen W x H (320x200 or 800x600), placed at that
 * screen's own precision so a big one turns a pixel at a time. */
static void project(u32 t, int *px, int *py, int *pz, int W, int H)
{
    int ay, ax, i;
    int cx = BOX_X + BOX_W / 2;
    int cy = BOX_Y + BOX_H / 2;

    if (!s_built) build();

    /* Two turns at once.  The rates are deliberately not multiples of one
     * another, so the two circuits drift against each other and the thing
     * tumbles instead of rocking back into the same pose every time round. */
    ay = (int)((t * 3) >> 4) & SINMASK;          /* round, and round       */
    ax = (int)(44 + ((t * 2) >> 4)) & SINMASK;   /* and over the top       */

    for (i = 0; i < NV; i++) {
        int x = s_vx[i], y = s_vy[i], z = s_vz[i];
        int x1, z1, y2, z2, d;

        /* Spin about the upright axis. */
        x1 = ((x * COS(ay)) - (z * SIN(ay))) >> 12;
        z1 = ((x * SIN(ay)) + (z * COS(ay))) >> 12;

        /* Over the top, as well.  There are two poses in every tumble where
         * the spine foreshortens into nothing and the banana reads as a
         * wedge -- but it is moving through them, not sitting in them, and
         * the moment either side is the one where the bend is clearest. */
        y2 = ((y * COS(ax)) - (z1 * SIN(ax))) >> 12;
        z2 = ((y * SIN(ax)) + (z1 * COS(ax))) >> 12;

        d = z2 + DIST;
        if (d < 40) d = 40;
        px[i] = cx * W / SCR_W + (x1 * FOCAL * W) / (d * SCR_W);
        py[i] = cy * H / SCR_H + (y2 * FOCAL * H) / (d * SCR_H);
        pz[i] = z2;
    }
}

/* How bright an edge is: nearer, brighter. */
static u8 edge_colour(const int *pz, int a, int b)
{
    int zz = (pz[a] + pz[b]) / 2;
    int lvl = (PAL_BANANA_N - 1) - ((zz + 120) * (PAL_BANANA_N - 1)) / 240;
    if (lvl < 1) lvl = 1;
    if (lvl > PAL_BANANA_N - 1) lvl = PAL_BANANA_N - 1;
    return (u8)(PAL_BANANA + lvl);
}

void banana_draw(u8 *dst, u32 t)
{
    int px[NV], py[NV], pz[NV], i;
    if (!s_built) build();
    project(t, px, py, pz, SCR_W, SCR_H);
    for (i = 0; i < s_ne; i++) {
        int a = s_e0[i], b = s_e1[i];
        /* Nearer edges brighter: it is the only depth cue a wireframe
         * has, and without it the far side reads as the near side. */
        int zz = (pz[a] + pz[b]) / 2;
        int lvl = (PAL_BANANA_N - 1) - ((zz + 120) * (PAL_BANANA_N - 1)) / 240;
        if (lvl < 1) lvl = 1;
        if (lvl > PAL_BANANA_N - 1) lvl = PAL_BANANA_N - 1;

        draw_line_clipped(dst, px[a], py[a], px[b], py[b],
                          (u8)(PAL_BANANA + lvl),
                          BOX_X, BOX_Y, BOX_W, BOX_H);
    }
}

/* At 800x600: the same banana, its corners placed two and a half times
 * further apart and its edges drawn at the screen's own pixels with a pen
 * two pixels across, instead of drawn small and stretched into blocks. */
static void line_big(u8 *dst, int W, int H, int x0, int y0, int x1, int y1, u8 colour,
                     int bx, int by, int bw, int bh)
{
    int dx = x1 - x0, dy = y1 - y0;
    int sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;
    int err, n;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    err = dx - dy;
    n = (dx > dy ? dx : dy) + 1;
    if (n > 2048) return;
    while (n--) {
        if (x0 >= bx && x0 + 1 < bx + bw && y0 >= by && y0 + 1 < by + bh &&
            x0 >= 0 && x0 + 1 < W && y0 >= 0 && y0 + 1 < H) {
            u8 *p = dst + y0 * W + x0;
            p[0] = p[1] = p[W] = p[W + 1] = colour;
        }
        {
            int e2 = err * 2;
            if (e2 > -dy) { err -= dy; x0 += sx; }
            if (e2 <  dx) { err += dx; y0 += sy; }
        }
    }
}

void banana_draw_big(u8 *dst, int W, int H, u32 t)
{
    int px[NV], py[NV], pz[NV], i;
    if (!s_built) build();
    project(t, px, py, pz, W, H);
    for (i = 0; i < s_ne; i++) {
        int a = s_e0[i], b = s_e1[i];
        line_big(dst, W, H, px[a], py[a], px[b], py[b], edge_colour(pz, a, b),
                 BOX_X * W / SCR_W, BOX_Y * H / SCR_H, BOX_W * W / SCR_W, BOX_H * H / SCR_H);
    }
}
