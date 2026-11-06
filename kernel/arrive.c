/* ===========================================================================
 * arrive.c -- the way in.
 *
 * Saffran comes up out of black, and the machine reports what it is
 * pretending to do while it gets ready.  None of the lines are true.  The
 * machine is in fact ready almost immediately; this exists because a
 * computer that simply appears is less fun than one that arrives.
 *
 * Four of the ten lines are chosen at random each boot, so it is not the
 * same sequence twice, and any key cuts the whole thing short.
 *
 * Text over a photograph has no reliable background, so every glyph is
 * outlined on all four sides rather than given the usual single drop
 * shadow -- otherwise half the strokes land on fur of their own
 * brightness and disappear.  The eight palette entries this uses are
 * reserved by tools/mksplash.py and are not part of the picture.
 * ===========================================================================
 */
#include "flopos.h"
#include "io.h"

#define LINES_SHOWN 4
#define LINE_Y0     146
#define LINE_STEP   11
#define TEXT_X      6
#define TYPE_MS     22                   /* per character                  */
#define NOODLE_MS   820                  /* how long each line "works"     */

static const char *s_chatter[] = {
    "Aligning the realm to the beat of the crystal...",
    "Negotiating with the silicone...",
    "Questioning Normalcy",
    "Weighing the Sympathetic with the Para-Sympathetic.",
    "Searching for stuff...",
    "Flippely-Flopping",
    "Schnipskiing the Schnoodlz",
    "Forgot where I placed stuff.",
    "Schnoodling the Schnabbleyblobs.",
    "Being generally cool.",
};
#define CHATTER_N ((int)(sizeof s_chatter / sizeof s_chatter[0]))

/* ---- the figure -------------------------------------------------------
 * One glyph for head, body and legs, and four tildes either side for the
 * arms.  The arms are rubber hose: no elbow, no bones, just a line that
 * swings in an arc from the shoulder.
 *
 * Two details do all the work.  Each segment swings further than the one
 * inboard of it, so the shoulder barely moves while the far end whips.
 * And each segment lags the one before, so the swing travels outward
 * instead of the whole arm moving as a rigid stick.  Together that reads
 * as a limb with no joints in it, flopping.
 *
 * The segments are placed on an arc rather than in a row, so the arm
 * genuinely sweeps up and down rather than just rippling in place -- and
 * it foreshortens on the way, which is what sells the arc.
 */
#define ARM_SEGS  4
#define ARM_STEP  4                      /* close enough to read as a line */
#define ARM_SWING 190                    /* brads of sweep, about 65 deg   */

static void draw_arm(u8 *buf, int x, int y, int dir, int ph, u32 unused)
{
    int i;

    for (i = 0; i < ARM_SEGS; i++) {
        int lag   = (i + 1) * 85;        /* the swing travels outward      */
        int reach = (i + 1) * ARM_STEP;
        int swing = (ARM_SWING * (i + 1)) / ARM_SEGS;   /* floppier out    */
        int ang   = (SIN(ph + lag) * swing) >> 12;
        int dx    = (COS(ang) * reach) >> 12;
        int dy    = (SIN(ang) * reach) >> 12;

        draw_char_outline(buf, x + dir * dx, y + 2 + dy, '~',
                          SPL_BRIGHT, SPL_SHADOW);
    }
    (void)unused;
}

static void draw_noodler(u8 *buf, int x, int y, u32 t)
{
    int ph = (int)((t * 3) >> 2) & SINMASK;

    draw_arm(buf, x, y, -1, ph, 0);
    draw_arm(buf, x, y, +1, ph + 512, 0);   /* half a turn out of step     */
    draw_char_outline(buf, x, y, CH_NOODLER, SPL_WHITE, SPL_SHADOW);
}

/* ---- one frame of the whole thing ------------------------------------- */
static void frame(const char **done, int ndone,
                  const char *typing, int typed, u32 t, int noodling)
{
    char partial[80];
    int i, y;

    memcpy32(g_back, (void *)g_saffran_pix, SCR_PIX / 4);

    y = LINE_Y0;
    for (i = 0; i < ndone; i++) {
        draw_text_outline(g_back, TEXT_X, y, done[i], SPL_DIM, SPL_SHADOW);
        y += LINE_STEP;
    }

    if (typing) {
        for (i = 0; i < typed && i < (int)sizeof partial - 1; i++)
            partial[i] = typing[i];
        partial[i] = 0;
        draw_text_outline(g_back, TEXT_X, y, partial, SPL_TEXT, SPL_SHADOW);

        if (noodling) {
            /* Just past the end of the text, but never so far right that
             * the figure walks off the screen. */
            int nx = TEXT_X + text_width(partial) + 4 + ARM_SEGS * ARM_STEP;
            if (nx > SCR_W - 4 - ARM_SEGS * ARM_STEP)
                nx = SCR_W - 4 - ARM_SEGS * ARM_STEP;
            draw_noodler(g_back, nx, y, t);
        }
    }

    vga_present();
}

static int interrupted(void)
{
    if (kbd_event() >= 0) return 1;
    if (serial_getc() >= 0) return 1;
    return 0;
}

void splash_show(void)
{
    static u8 black[768];
    const char *shown[LINES_SHOWN];
    u8 picked[CHATTER_N];
    int n, i, len, typed;
    u32 t0;

    /* Seeded off the clock, or every boot says the same things. */
    {
        u8 h, mi, se, mo, da; u16 yr;
        u32 seed = ms_now() * 2654435761u;
        if (rtc_read(&h, &mi, &se, &yr, &mo, &da))
            seed ^= (u32)(se * 3600 + mi * 60 + h) * 2246822519u;
        rnd_seed(seed);
    }

    for (i = 0; i < CHATTER_N; i++) picked[i] = 0;

    memcpy32(g_back, (void *)g_saffran_pix, SCR_PIX / 4);
    vga_set_palette(black);
    vga_present();
    vga_fade_to(g_saffran_pal, 22);

    for (n = 0; n < LINES_SHOWN; n++) {
        int which;
        do { which = rnd_range(0, CHATTER_N - 1); } while (picked[which]);
        picked[which] = 1;

        len = strlen8(s_chatter[which]);

        for (typed = 1; typed <= len; typed++) {
            u32 until = ms_now() + TYPE_MS;
            frame(shown, n, s_chatter[which], typed, ms_now(), 0);
            while (ms_now() < until)
                if (interrupted()) goto done;
            if (interrupted()) goto done;
        }

        t0 = ms_now();
        while (ms_now() - t0 < NOODLE_MS) {
            frame(shown, n, s_chatter[which], len, ms_now(), 1);
            if (interrupted()) goto done;
        }

        shown[n] = s_chatter[which];
    }

done:
    vga_fade_out(18);
    memset8(g_back, 0, SCR_PIX);
    vga_present();
}
