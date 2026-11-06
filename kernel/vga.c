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

void vga_present(void)
{
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
