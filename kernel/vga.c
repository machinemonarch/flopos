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

    for (y = 0; y < SCR_H; y++) {
        expand_row(d, g_back + y * SCR_W);
        memcpy32(d + HI_W, d, HI_W / 4);
        memcpy32(d + 2 * HI_W, d, HI_W / 4);
        d += 3 * HI_W;
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

    /* 0x4000 on the mode number is what asks for the flat stretch. */
    r = bios_int(0x10, 0x4F02, (u16)(m.mode | 0x4000), 0, 0, 0, 0);
    if ((r & 0xFFFF) != 0x004F) { s_hi = 0; return 0; }

    s_lfb = (volatile u8 *)m.lfb;
    s_hmode = m.mode;
    s_retrace_ok = 0;                    /* a different chip drives it now */
    vga_set_palette(s_current);          /* the mode change cleared it     */
    return 1;
}

void vga_present(void)
{
    if (s_hi) {
        memcpy32((void *)s_lfb, s_hi, (HI_W * HI_H) / 4);
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
    }
}

void vga_fade_out(int steps)
{
    static u8 black[768];
    int i;
    for (i = 0; i < 768; i++) black[i] = 0;
    vga_fade_to(black, steps);
}
