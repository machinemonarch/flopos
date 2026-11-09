/* ===========================================================================
 * cyberdelia.c -- the way in.
 *
 * Two things happen when the wizard lets you through.
 *
 * First a jingle: a secret has been found.  It is in the manner of the
 * little fanfare an old handheld plays when a chest opens -- a quick climb
 * of square-wave notes and one bright note held at the top -- but the
 * notes are our own: three rising arpeggios, minor, minor, then major,
 * landing on a high A.  Square waves because that is what the PC speaker
 * makes, the same kind of sound a Game Boy makes; the FM chip doubles it.
 *
 * Then the entrance.  The screen ripples -- every row slid sideways along
 * two sine waves, swelling up and dying away -- while bells climb a
 * pentatonic run and the colours glow and drift between cyan and magenta.
 * The wizard dissolves into it partway through.
 *
 * The glow is kept gentle on purpose, for anyone sensitive to flashing
 * light: it rises and falls smoothly about once every 0.8 seconds (well
 * under the three-flashes-a-second line), it never goes more than a ninth
 * of the way towards white, nothing is red, and nothing ever switches
 * abruptly from dark to bright.
 * ===========================================================================
 */
#include "flopos.h"

/* Semitones above A4, in whole hertz, for three octaves. */
static const u16 s_hz[37] = {
     440,  466,  494,  523,  554,  587,  622,  659,  698,  740,  784,  831,
     880,  932,  988, 1047, 1109, 1175, 1245, 1319, 1397, 1480, 1568, 1661,
    1760, 1865, 1976, 2093, 2217, 2349, 2489, 2637, 2794, 2960, 3136, 3322,
    3520,
};

static void wait_ms(u32 ms, void (*each)(void))
{
    u32 until = ms_now() + ms;
    while ((s32)(until - ms_now()) > 0) {
        if (each) each();
        main_pump(0);
    }
}

/* ---- a secret, found ------------------------------------------------------ */
void cyberdelia_jingle(void (*each)(void))
{
    /* A5 C6 E6, B5 D6 F#6, C#6 E6, and A6 held. */
    static const u8 run[] = { 12, 15, 19,  14, 17, 21,  16, 19,  24 };
    int i, n = (int)sizeof run;

    snd_fx_begin();
    for (i = 0; i < n; i++) {
        int last = (i == n - 1);
        snd_fx_square(s_hz[run[i]]);
        wait_ms(last ? 560 : 76, each);
        snd_fx_square(0);
        wait_ms(last ? 120 : 12, each);  /* each note its own, as a chip does */
    }
    snd_fx_end();
}

/* ---- the ripple ------------------------------------------------------------- */
static int s_amp;                        /* how far a row may slide, pixels */
static int s_ph;                         /* where along the waves we are    */

static void ripple(u8 *cv, int w, int h, u32 t)
{
    static u8 row[1024];
    int k1 = (SINLEN * 3) / h + 1;       /* three long waves down the screen */
    int k2 = (SINLEN * 7) / h + 1;       /* and seven short ones against them */
    int pitch = vga_pitch(), x, y;

    (void)t;
    if (!s_amp || w > (int)sizeof row) return;
    for (y = 0; y < h; y++) {
        u8 *r = cv + y * pitch;
        int off = (s_amp * SIN(y * k1 + s_ph) + (s_amp / 3) * SIN(y * k2 - 2 * s_ph)) >> 12;

        off %= w;
        if (off < 0) off += w;
        if (!off) continue;
        for (x = 0; x < w; x++) row[x] = r[x];
        for (x = 0; x < w - off; x++) r[x + off] = row[x];
        for (x = w - off; x < w; x++) r[x + off - w] = row[x];
    }
}

/* ---- the way in ------------------------------------------------------------- */
#define ENTER_MS   2800
#define CLOSE_AT   (ENTER_MS * 45 / 100)  /* when the wizard dissolves      */
#define BELL_EVERY 105
#define CHORD_AT   1300
#define GLOW_MS    800                   /* one soft glow, rise and fall    */
#define GLOW_MAX   28                    /* of 256: a ninth of the way      */
#define TINT_MAX   40                    /* of 256                          */

void cyberdelia_enter(void (*each)(void))
{
    static u8 base[768], pal[768];
    /* Cyan and magenta, in the DAC's six bits. */
    static const u8 tint[2][3] = { { 16, 52, 63 }, { 58, 24, 60 } };
    static const u8 bells[] = { 0, 3, 5, 7, 10, 12, 15, 17, 19, 22, 24 };
    int nb = 0, chord = 0, closed = 0, fm = snd_has_fm();
    u32 t0 = ms_now(), blip = 0;

    vga_get_palette(base);
    snd_fx_begin();
    g_frame_fx = ripple;

    for (;;) {
        u32 el = ms_now() - t0;
        int env, glow, k, tk, m, i;

        if (el >= ENTER_MS) break;

        /* Everything swells in and dies away over the whole span. */
        env = SIN((int)((el * (SINLEN / 2)) / ENTER_MS));        /* 0..4096..0 */
        s_amp = (env * (vga_width() / 20)) >> 12;
        s_ph  = (int)el;

        glow = (4096 - COS((int)((el * SINLEN) / GLOW_MS))) / 2; /* 0..4096    */
        k  = (((glow * GLOW_MAX) >> 12) * env) >> 12;
        tk = (TINT_MAX * env) >> 12;
        m  = (4096 + SIN((int)((el * SINLEN) / 2000))) / 2;      /* cyan..magenta */
        for (i = 3; i < 768; i++) {                              /* 0 stays put */
            int b = base[i], c = i % 3;
            int tc = (tint[0][c] * (4096 - m) + tint[1][c] * m) >> 12;
            int v = b + (((63 - b) * k) >> 8);
            v += ((tc - v) * tk) >> 8;
            pal[i] = (u8)(v < 0 ? 0 : v > 63 ? 63 : v);
        }
        pal[0] = base[0]; pal[1] = base[1]; pal[2] = base[2];
        vga_set_palette(pal);

        /* Bells: a rising run while the ripple grows, then two together at
         * the top, left to ring out.  On the speaker alone each one is a
         * short blip, stopped here. */
        while (nb < (int)sizeof bells && el >= (u32)nb * BELL_EVERY) {
            snd_fx_bell(s_hz[bells[nb++]]);
            blip = el;
        }
        if (!chord && el >= CHORD_AT) {
            snd_fx_bell(s_hz[24]);
            snd_fx_bell(s_hz[31]);
            chord = 1;
            blip = el;
        }
        if (!fm && blip && el - blip > 45) { snd_fx_square(0); blip = 0; }

        if (!closed && el >= CLOSE_AT) { term_win_close(); closed = 1; }
        if (each && !closed) each();
        main_pump(1);
    }

    g_frame_fx = 0;
    s_amp = 0;
    vga_set_palette(base);
    snd_fx_end();
    main_pump(1);
}
