/* ===========================================================================
 * vga.c -- mode 13h: 320x200, 256 colours, one byte per pixel.
 *
 * The BIOS already put the card in mode 13h back in the boot sector, so
 * there is no register programming here at all -- just a back buffer, a
 * retrace wait and a 64000-byte blit.  Everything draws into g_back and
 * appears when vga_present() is called, which keeps the effects free of
 * tearing and lets them read back what they drew last frame.
 *
 * The DAC is six bits per channel, so palette entries run 0..63.
 * ===========================================================================
 */
#include "flopos.h"
#include "io.h"

#define DAC_WRITE_INDEX 0x3C8
#define DAC_DATA        0x3C9
#define INPUT_STATUS_1  0x3DA

u8 *g_back;

static u8 s_current[768];                /* what the DAC is holding now    */

/* ---- the big screen ---------------------------------------------------
 * Mode 13h draws 320x200 and the panel has to stretch it to 800x600, by
 * two and a half across and three down.  Neither is a whole number, so a
 * one-pixel line lands on one row of the panel here and two there, and
 * everything looks chewed.
 *
 * At 800x600 nothing is stretched at all.  What changes is where things
 * are drawn: the moving background stays at 320x200, because it is a
 * gradient and nobody can tell it was blown up, and the text is drawn at
 * the full size on top of it, where a letter has four times the pixels to
 * be a letter with.
 *
 * Only exactly 800x600 is taken.  The scaling from 320x200 is then two
 * source pixels to five across and one row to three down, which is a
 * handful of stores and no arithmetic at all per pixel.
 */
static u8  *s_hi;                        /* the 800x600 canvas             */
static volatile u8 *s_lfb;               /* where the card displays it     */
static u16  s_hmode;
static int  s_composed;          /* the background is already up    */
static u32  s_frames;            /* how many have reached the card  */
static u32  s_worst;             /* the slowest one of them         */
static u32  s_said;              /* when we last mentioned it       */
#define HI_W 800
#define HI_H 600

void vga_init(void)
{
    g_back = (u8 *)halloc(SCR_PIX + 1024);   /* slack for sloppy writes    */
    memset8(g_back, 0, SCR_PIX);
}

/* Park on the leading edge of vertical retrace.  Two waits: first for any
 * retrace already in progress to end, then for the next one to start, so we
 * always get a full frame of quiet time afterwards.
 *
 * Not every chip drives this bit the way a standard VGA does, especially
 * one feeding an LCD through its own scaler.  A frame is about 14ms, so
 * anything past 100,000 polls -- the far side of 100ms -- means the bit is
 * never going to change, and we stop asking.  Tearing is a far better
 * outcome than spending a fifth of a second a frame waiting for nothing. */
static int s_retrace_ok = 1;
static u32 s_retrace_polls;

void vga_wait_retrace(void)
{
    u32 guard;

    if (!s_retrace_ok) return;

    guard = 0;
    while ((inb(INPUT_STATUS_1) & 0x08) && ++guard < 100000) { }
    if (guard >= 100000) { s_retrace_ok = 0; return; }

    guard = 0;
    while (!(inb(INPUT_STATUS_1) & 0x08) && ++guard < 100000) { }
    if (guard >= 100000) { s_retrace_ok = 0; return; }

    s_retrace_polls = guard;
}

int vga_retrace_works(void) { return s_retrace_ok; }
u32 vga_retrace_polls(void) { return s_retrace_polls; }

int vga_is_big(void)  { return s_hi != 0; }
u8 *vga_canvas(void)  { return s_hi ? s_hi : g_back; }
int vga_width(void)   { return s_hi ? HI_W : SCR_W; }
int vga_height(void)  { return s_hi ? HI_H : SCR_H; }
int vga_pitch(void)   { return s_hi ? HI_W : SCR_W; }
int vga_scale(void)   { return s_hi ? 2 : 1; }
u16 vga_big_mode(void){ return s_hmode; }

/* 320 across becomes 800: every two source pixels become five. */
static void expand_row(u8 *dst, const u8 *src)
{
    int i;
    for (i = 0; i < SCR_W; i += 2) {
        u8 a = src[i], b = src[i + 1];
        dst[0] = a; dst[1] = a; dst[2] = a;
        dst[3] = b; dst[4] = b;
        dst += 5;
    }
}

/* Blow the small buffer up into the big one.  Each source row is expanded
 * once and then copied twice, because 200 into 600 is exactly three. */
void vga_compose(void)
{
    u8 *d = s_hi;
    int y;

    if (!s_hi) return;
    s_composed = 1;

    for (y = 0; y < SCR_H; y++) {
        expand_row(d, g_back + y * SCR_W);
        memcpy32(d + HI_W, d, HI_W / 4);
        memcpy32(d + 2 * HI_W, d, HI_W / 4);
        d += 3 * HI_W;
    }
}

/* The big picture, back down to 320x200: the vortex photographs the small
 * buffer and swirls that, and at 800x600 the text is not in it -- it was
 * drawn straight onto the big one.  Every fifth-of-two-and-a-halfth pixel
 * is enough for something about to be spun into a whirlpool. */
void vga_shrink(void)
{
    int x, y;
    if (!s_hi) return;
    for (y = 0; y < SCR_H; y++) {
        const u8 *src = s_hi + (y * 3 + 1) * HI_W;
        u8 *dst = g_back + y * SCR_W;
        for (x = 0; x < SCR_W; x++) dst[x] = src[(x * 5) / 2 + 1];
    }
}

/* Ask the card for 800x600 in 256 colours with the whole screen as one
 * flat stretch of memory.  Returns 0 and changes nothing if it cannot,
 * which leaves everything exactly as it was. */
int vga_go_big(void)
{
    VbeMode m;
    int i;
    u32 r;

    if (s_hi) return 1;
    if (!vbe_probe()) return 0;

    for (i = 0; i < 128; i++) {
        int mode = vbe_mode_at(i);
        if (mode < 0) return 0;
        if (!vbe_mode_info((u16)mode, &m)) continue;
        if (!vbe_is_graphics(&m) || !vbe_has_lfb(&m)) continue;
        if (m.width != HI_W || m.height != HI_H || m.bpp != 8) continue;
        if (m.pitch != HI_W) continue;   /* a padded row would need care   */
        break;
    }
    if (i >= 128) return 0;

    /* halloc does not come back empty-handed, it stops the machine, so
     * the room has to be checked before asking for it.  Half a megabyte is
     * nothing against 32MB and everything against a machine that fell back
     * to conventional memory. */
    if (heap_total() - heap_used() < HI_W * HI_H + 2048) return 0;
    s_hi = (u8 *)halloc(HI_W * HI_H + 1024);
    memset8(s_hi, 0, HI_W * HI_H);

    /* Stop the panel stretching before changing mode, not after.  At
     * 800x600 the picture is already the size of the screen, so there is
     * nothing to stretch, and a Chips part left holding a ratio that no
     * longer describes anything is a good way to wedge the part that
     * drives the screen -- which is also the part we are about to write
     * half a megabyte to. */
    if (panel_present()) panel_set_wide(0);

    /* 0x4000 on the mode number is what asks for the flat stretch. */
    r = bios_int(0x10, 0x4F02, (u16)(m.mode | 0x4000), 0, 0, 0, 0);
    if ((r & 0xFFFF) != 0x004F) {
        s_hi = 0;
        if (panel_present()) panel_set_wide(1);
        return 0;
    }

    s_lfb = (volatile u8 *)m.lfb;
    s_hmode = m.mode;
    s_composed = 0;

    /* Sixteen bytes before half a million.  If the card's memory does not
     * answer, this is where the machine stops, and the wire will have said
     * so -- which is the difference between a bug I can find and a bug I
     * can only speculate about. */
    {
        int i, ok = 1;
        for (i = 0; i < 16; i++) s_lfb[i] = (u8)(i * 17);
        for (i = 0; i < 16; i++) if (s_lfb[i] != (u8)(i * 17)) ok = 0;
        if (!ok) {
            /* Memory that does not hold what was written to it is not a
             * screen.  Back to 13h before anything else trusts it. */
            serial_printf("[vga] frame buffer at %x does not hold data\n",
                          (u32)m.lfb);
            bios_int(0x10, 0x0013, 0, 0, 0, 0, 0);
            s_hi = 0;
            if (panel_present()) panel_set_wide(1);
            vga_set_palette(s_current);
            return 0;
        }
    }
    /* The retrace bit still works in VESA modes on this family; if it ever
     * does not, the first wait times out and stops asking. */
    s_retrace_ok = 1;
    vga_set_palette(s_current);          /* the mode change cleared it     */
    serial_printf("[vga] 800x600, mode %x, frame buffer at %x\n",
                  (u32)m.mode, (u32)m.lfb);
    s_frames = 0;
    s_worst = 0;
    s_said = 0;
    return 1;
}

/* Back to 320x200, which is a safety valve rather than a feature: if the
 * big mode comes out wrong on a panel I cannot see, there has to be a way
 * to say so from the keyboard rather than from the power switch. */
void vga_go_small(void)
{
    if (!s_hi) return;
    bios_int(0x10, 0x0013, 0, 0, 0, 0, 0);
    s_hi = 0;
    if (panel_present()) panel_set_wide(1);   /* 13h needs it again       */
    s_retrace_ok = 1;
    vga_set_palette(s_current);
}

void vga_present(void)
{
    if (s_hi) {
        /* Anything that drew into the small buffer and then presented --
         * the boot screen, the arrival, the vortex -- never knew about the
         * big one.  Blow it up for them. */
        if (!s_composed) vga_compose();
        s_composed = 0;

        /* The first three frames each say so before and after.  If the
         * machine dies on the way to the screen, the wire ends mid-pair
         * and names the frame it died on. */
        /* Timed, because a machine too slow for this should say so and
         * step back down rather than crawl.  Three frames is enough to
         * tell.  Two seconds in, the rate goes down the wire once, which
         * is the number worth having from a machine I cannot watch. */
        {
            u32 a = ms_now(), took;

            memcpy32((void *)s_lfb, s_hi, (HI_W * HI_H) / 4);
            took = ms_now() - a;
            s_frames++;
            if (s_frames <= 3 && took > s_worst) s_worst = took;

            if (!s_said) s_said = ms_now();
            else if (s_said != 1 && ms_now() - s_said >= 2000) {
                serial_printf("[vga] %u frames in 2 seconds, %u ms to send one\n",
                              s_frames, took);
                s_said = 1;
            }

            if (s_frames == 3 && s_worst > 400) {
                serial_printf("[vga] %u ms a frame is too slow, back to 320x200\n",
                              s_worst);
                vga_go_small();
            }
        }
        return;
    }
    vga_wait_retrace();
    memcpy32((void *)VGA_FB, g_back, SCR_PIX / 4);
}

void vga_clear(u8 colour)
{
    u32 v = colour;
    v |= v << 8; v |= v << 16;
    memset32(g_back, v, SCR_PIX / 4);
}

void vga_set_palette(const u8 *pal768)
{
    int i;

    /* Loading the DAC mid-scanline shows up as speckle on real hardware,
     * so do it in the retrace gap. */
    vga_wait_retrace();
    outb(DAC_WRITE_INDEX, 0);
    for (i = 0; i < 768; i++) {
        s_current[i] = pal768[i];
        outb(DAC_DATA, pal768[i]);
    }
}

void vga_get_palette(u8 *out768)
{
    int i;
    for (i = 0; i < 768; i++) out768[i] = s_current[i];
}

/* Cross-fade the DAC from wherever it is to a new palette. */
void vga_fade_to(const u8 *pal768, int steps)
{
    static u8 from[768], tmp[768];
    int i, s;

    if (steps < 1) steps = 1;
    for (i = 0; i < 768; i++) from[i] = s_current[i];

    for (s = 1; s <= steps; s++) {
        for (i = 0; i < 768; i++) {
            int a = from[i], b = pal768[i];
            tmp[i] = (u8)(a + ((b - a) * s) / steps);
        }
        vga_set_palette(tmp);
        /* Each step is paced by the screen's own refresh.  Where that
         * cannot be seen, by the clock instead -- otherwise a fade that
         * should take a quarter of a second is over before it is visible. */
        if (!s_retrace_ok) delay_ms(14);
    }
}

void vga_fade_out(int steps)
{
    static u8 black[768];
    int i;
    for (i = 0; i < 768; i++) black[i] = 0;
    vga_fade_to(black, steps);
}
