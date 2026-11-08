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
 * every other.
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

void banana_draw(u8 *dst, u32 t)
{
    int px[NV], py[NV], pz[NV];
    int ay, i;
    int cx = BOX_X + BOX_W / 2;
    int cy = BOX_Y + BOX_H / 2;

    if (!s_built) build();

    ay = (int)((t * 3) >> 4) & SINMASK;          /* round, and round       */

    for (i = 0; i < NV; i++) {
        int x = s_vx[i], y = s_vy[i], z = s_vz[i];
        int x1, z1, y2, z2, d;

        /* Spin about the upright axis. */
        x1 = ((x * COS(ay)) - (z * SIN(ay))) >> 12;
        z1 = ((x * SIN(ay)) + (z * COS(ay))) >> 12;

        /* A slight lean, and no more than that.  Tipped hard the curve of
         * the spine foreshortens into nothing and the whole thing reads as
         * a wedge; seen nearly side-on, the bend is the silhouette. */
        y2 = ((y * COS(44)) - (z1 * SIN(44))) >> 12;
        z2 = ((y * SIN(44)) + (z1 * COS(44))) >> 12;

        d = z2 + DIST;
        if (d < 40) d = 40;
        px[i] = cx + (x1 * FOCAL) / d;
        py[i] = cy + (y2 * FOCAL) / d;
        pz[i] = z2;
    }

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
