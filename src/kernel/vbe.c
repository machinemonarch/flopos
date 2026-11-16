/* ===========================================================================
 * vbe.c -- ask the video BIOS what this card can actually do.
 *
 * Mode 13h is 320x200, which the VGA sends out as 640x400, which the panel
 * then stretches to 800x600 at 1.25 across and 1.5 down.  Neither ratio is
 * a whole number and they are not the same as each other, so a one-pixel
 * stroke lands on one panel row here and two there.  That is why the text
 * looks chewed, and no amount of redrawing the font fixes it.
 *
 * The way out is to stop asking the panel to scale anything: run at the
 * panel's own 800x600 and have every pixel be a pixel.  The Chips 65550
 * can do it -- Windows 95 did exactly this on this machine -- but what it
 * offers has to be asked for rather than assumed, because a mode the BIOS
 * does not list is a mode that does not exist no matter what the datasheet
 * says.
 *
 * Nothing here changes the mode.  It reads the VESA tables and reports,
 * and that is deliberate: switching the mode on hardware I cannot see,
 * before knowing what the choices are, is how you end up with a black
 * panel and no way to find out why.
 *
 * The one fact everything else depends on is whether a mode has a linear
 * frame buffer.  With one, the whole screen is a flat array somewhere
 * above the 32MB of RAM and can be written to like any other memory.
 * Without one, it is a 64K window at 0xA0000 that has to be wound along a
 * bank at a time, and 800x600 needs eight winds per frame.
 * ===========================================================================
 */
#include "flopos.h"

/* VbeInfoBlock, by offset.  Reading fields out of the buffer by hand
 * rather than laying a struct over it: the block is packed and the
 * compiler's idea of alignment is not worth arguing with. */
#define VI_SIG        0
#define VI_VERSION    4
#define VI_OEM_PTR    6
#define VI_CAPS       10
#define VI_MODES_PTR  14
#define VI_MEM_64K    18

/* ModeInfoBlock, likewise. */
#define MI_ATTR       0
#define MI_WIN_A_SEG  8
#define MI_PITCH      16
#define MI_WIDTH      18
#define MI_HEIGHT     20
#define MI_PLANES     24
#define MI_BPP        25
#define MI_MODEL      27
#define MI_LFB_BASE   40

#define ATTR_SUPPORTED 0x0001
#define ATTR_GRAPHICS  0x0010
#define ATTR_LFB       0x0080

static int s_present;
static u16 s_version;
static u32 s_vram_kb;
static char s_oem[64];

/* The mode list, copied out rather than pointed at.  Plenty of BIOSes put
 * it inside the information block itself, which the next call -- asking
 * about one of those very modes -- then writes straight over.  Reading it
 * in place gives exactly one mode and then silence, which looks like a
 * card with nothing to offer. */
static u16 s_modes[128];
static int s_nmodes;

static u16 rd16(const volatile u8 *p, int off)
{
    return (u16)(p[off] | (p[off + 1] << 8));
}

static u32 rd32(const volatile u8 *p, int off)
{
    return (u32)p[off] | ((u32)p[off + 1] << 8) |
           ((u32)p[off + 2] << 16) | ((u32)p[off + 3] << 24);
}

/* A real-mode far pointer, as a flat address.  Everything the BIOS hands
 * back points into the first megabyte, which a flat segment reaches. */
static const volatile u8 *farp(u32 fp)
{
    u32 seg = fp >> 16, off = fp & 0xFFFF;
    return (const volatile u8 *)(seg * 16 + off);
}

int vbe_probe(void)
{
    volatile u8 *buf = VBE_BUF;
    u32 r;
    int i;

    s_present = 0;
    s_oem[0] = 0;

    /* "VBE2" in the signature asks for the 2.0 block, which is the one
     * that carries a linear frame buffer address.  A 1.x BIOS ignores it
     * and fills in the smaller block, which is fine -- it just means there
     * will be no LFB to find. */
    for (i = 0; i < 512; i++) buf[i] = 0;
    buf[0] = 'V'; buf[1] = 'B'; buf[2] = 'E'; buf[3] = '2';

    r = bios_int(0x10, 0x4F00, 0, 0, 0, VBE_BUF_SEG, 0);
    if ((r & 0xFFFF) != 0x004F) return 0;
    if (buf[0] != 'V' || buf[1] != 'E' || buf[2] != 'S' || buf[3] != 'A')
        return 0;

    s_version  = rd16(buf, VI_VERSION);
    s_vram_kb  = (u32)rd16(buf, VI_MEM_64K) * 64;

    {
        const volatile u8 *p = farp(rd32(buf, VI_MODES_PTR));
        s_nmodes = 0;
        while (s_nmodes < (int)(sizeof s_modes / sizeof s_modes[0])) {
            u16 m = rd16(p, s_nmodes * 2);
            if (m == 0xFFFF) break;
            s_modes[s_nmodes++] = m;
        }
    }

    /* The OEM string has to be copied now.  It lives in the BIOS's own
     * scratch space, which the next call is entitled to reuse. */
    {
        const volatile u8 *p = farp(rd32(buf, VI_OEM_PTR));
        for (i = 0; i < (int)sizeof s_oem - 1 && p[i]; i++) s_oem[i] = (char)p[i];
        s_oem[i] = 0;
    }

    s_present = 1;
    return 1;
}

int  vbe_present(void)  { return s_present; }
u16  vbe_version(void)  { return s_version; }
u32  vbe_vram_kb(void)  { return s_vram_kb; }
const char *vbe_oem(void) { return s_oem; }

int vbe_mode_at(int n)
{
    if (!s_present || n < 0 || n >= s_nmodes) return -1;
    return (int)s_modes[n];
}

int vbe_mode_info(u16 mode, VbeMode *out)
{
    volatile u8 *buf = VBE_BUF;
    u32 r;
    int i;

    if (!s_present) return 0;

    for (i = 0; i < 256; i++) buf[i] = 0;
    r = bios_int(0x10, 0x4F01, 0, mode, 0, VBE_BUF_SEG, 0);
    if ((r & 0xFFFF) != 0x004F) return 0;

    out->mode   = mode;
    out->attr   = rd16(buf, MI_ATTR);
    out->width  = rd16(buf, MI_WIDTH);
    out->height = rd16(buf, MI_HEIGHT);
    out->pitch  = rd16(buf, MI_PITCH);
    out->bpp    = buf[MI_BPP];
    out->planes = buf[MI_PLANES];
    out->model  = buf[MI_MODEL];
    out->win_seg = rd16(buf, MI_WIN_A_SEG);
    out->lfb    = rd32(buf, MI_LFB_BASE);

    if (!(out->attr & ATTR_SUPPORTED)) return 0;
    return 1;
}

int vbe_is_graphics(const VbeMode *m) { return (m->attr & ATTR_GRAPHICS) != 0; }
int vbe_has_lfb(const VbeMode *m)     { return (m->attr & ATTR_LFB) != 0; }
