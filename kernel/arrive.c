/* ===========================================================================
 * arrive.c -- the way in, told as it happens.
 *
 * By the time the kernel runs, Saffran is already on the screen: the boot
 * sector loaded her off the floppy first and painted her on as she
 * arrived, then loaded the kernel underneath her with a bar along the
 * bottom.  The kernel takes her over from there and narrates its own
 * start, one line per real step, in a dialect of its own.  Every line is
 * something the machine is actually doing at that moment, and where a step
 * finds something out -- the processor's speed, how much memory there is,
 * how many times it has woken up -- the answer is shown at the end of the
 * line.  The bar carries on from where the boot sector left it, one step
 * at a time.
 *
 * The first few steps (memory, tables, the interrupt controllers, the
 * timer) are over in the first millisecond, before there is a clock to
 * type by.  Their lines are told first, in the order they happened.
 * Everything after that is typed before its step runs.
 *
 * Any key stops the typing: the steps still all run, as fast as they can,
 * and the lines simply appear whole.
 *
 * Text over a photograph has no reliable background, so every glyph is
 * outlined on all four sides rather than given the usual single drop
 * shadow -- otherwise half the strokes land on fur of their own
 * brightness and disappear.  The palette entries this uses are kept back
 * from the photograph by tools/mksplash.py.
 * ===========================================================================
 */
#include "flopos.h"
#include "io.h"

#define LINES_SHOWN 5
#define LINE_Y0     135
#define LINE_STEP   11
#define TEXT_X      6
#define RESULT_R    314                  /* results end at this column      */
#define TYPE_MS     7                    /* per character                   */
#define AFTER_MS    70                   /* a breath after each step        */
#define HOLD_MS     900                  /* the last line, before fading    */
#define BAR_Y       198
#define BAR_FROM    160                  /* the boot sector's half          */
#define BOOT_STEPS  19                   /* lines in a full boot            */

typedef struct { char text[60]; char result[28]; } BootLine;

static BootLine s_line[LINES_SHOWN];
static int      s_n;                     /* lines so far, all of them       */
static int      s_skip;
static int      s_full;                  /* the bar: all the way            */
static u8      *s_pic;                   /* Saffran, untouched              */

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

static void draw_arm(u8 *buf, int x, int y, int dir, int ph)
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
}

static void draw_noodler(u8 *buf, int x, int y, u32 t)
{
    int ph = (int)((t * 3) >> 2) & SINMASK;

    draw_arm(buf, x, y, -1, ph);
    draw_arm(buf, x, y, +1, ph + 512);   /* half a turn out of step        */
    draw_char_outline(buf, x, y, CH_NOODLER, SPL_WHITE, SPL_SHADOW);
}

/* ---- one frame ---------------------------------------------------------- */
static BootLine *line_at(int k) { return &s_line[k % LINES_SHOWN]; }

/* typed: how much of the newest line is showing.  The newest line is the
 * step under way; the ones above it are done. */
static void frame(int typed, int noodling)
{
    int first = s_n > LINES_SHOWN ? s_n - LINES_SHOWN : 0, k, y = LINE_Y0;
    int done = s_full ? BOOT_STEPS : (s_n > 0 ? s_n - 1 : 0);
    int bar = BAR_FROM + ((SCR_W - BAR_FROM) * done) / BOOT_STEPS;

    memcpy32(g_back, s_pic, SCR_PIX / 4);

    for (k = first; k < s_n; k++, y += LINE_STEP) {
        BootLine *L = line_at(k);
        int newest = (k == s_n - 1);
        char partial[60];
        const char *t = L->text;

        if (newest && typed >= 0) {
            int i;
            for (i = 0; i < typed && L->text[i] && i < (int)sizeof partial - 1; i++)
                partial[i] = L->text[i];
            partial[i] = 0;
            t = partial;
        }
        draw_text_outline(g_back, TEXT_X, y, t, newest ? SPL_TEXT : SPL_DIM, SPL_SHADOW);

        if (L->result[0]) {
            int w = text_width(L->result), x = RESULT_R - w;
            int after = TEXT_X + text_width(L->text) + 6;
            if (x < after) x = after;
            draw_text_outline(g_back, x, y, L->result,
                              newest ? SPL_WHITE : SPL_DIM, SPL_SHADOW);
        } else if (newest && noodling) {
            int nx = TEXT_X + text_width(t) + 4 + ARM_SEGS * ARM_STEP;
            if (nx > SCR_W - 4 - ARM_SEGS * ARM_STEP)
                nx = SCR_W - 4 - ARM_SEGS * ARM_STEP;
            draw_noodler(g_back, nx, y, ms_now());
        }
    }

    if (bar > SCR_W) bar = SCR_W;
    memset8(g_back + BAR_Y * SCR_W, SPL_BRIGHT, (usize)bar);
    memset8(g_back + (BAR_Y + 1) * SCR_W, SPL_BRIGHT, (usize)bar);

    vga_present();
}

static int interrupted(void)
{
    if (s_skip) return 1;
    if (kbd_event() >= 0 || serial_getc() >= 0) s_skip = 1;
    return s_skip;
}

static void wait_ms(u32 ms, int noodling)
{
    u32 until = ms_now() + ms;
    while ((s32)(until - ms_now()) > 0) {
        if (interrupted()) return;
        frame(-1, noodling);
    }
}

/* ---- the narrator -------------------------------------------------------- */
void boot_begin(void)
{
    /* Seeded off the clock, so nothing random says the same thing every
     * boot. */
    u8 h, mi, se, mo, da; u16 yr;
    u32 seed = ms_now() * 2654435761u;
    if (rtc_read(&h, &mi, &se, &yr, &mo, &da))
        seed ^= (u32)(se * 3600 + mi * 60 + h) * 2246822519u;
    rnd_seed(seed);

    s_pic = (u8 *)halloc(SCR_PIX);
    memcpy32(s_pic, g_back, SCR_PIX / 4);
}

void boot_step(const char *fmt, ...)
{
    __builtin_va_list ap;
    BootLine *L;
    int len, typed;

    if (s_n > 0) wait_ms(AFTER_MS, 0);   /* let the last result land       */

    L = line_at(s_n++);
    __builtin_va_start(ap, fmt);
    fmt_vstr(L->text, sizeof L->text, fmt, ap);
    __builtin_va_end(ap);
    L->result[0] = 0;
    if (serial_present()) serial_printf("[boot] %s\n", L->text);

    len = strlen8(L->text);
    for (typed = 1; typed <= len && !interrupted(); typed++) {
        u32 until = ms_now() + TYPE_MS;
        frame(typed, 1);
        while ((s32)(until - ms_now()) > 0 && !interrupted()) { }
    }
    frame(-1, 1);                        /* whole, while the step runs     */
}

void boot_result(const char *fmt, ...)
{
    __builtin_va_list ap;
    BootLine *L;

    if (!s_n) return;
    L = line_at(s_n - 1);
    __builtin_va_start(ap, fmt);
    fmt_vstr(L->result, sizeof L->result, fmt, ap);
    __builtin_va_end(ap);
    if (serial_present()) serial_printf("[boot]   %s\n", L->result);
    frame(-1, 0);
}

void boot_end(void)
{
    s_full = 1;
    wait_ms(HOLD_MS, 1);
    vga_fade_out(18);
    memset8(g_back, 0, SCR_PIX);
    vga_present();
}
