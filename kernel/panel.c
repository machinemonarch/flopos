/* ===========================================================================
 * panel.c -- make the picture fill the laptop's screen.
 *
 * A Satellite Pro 430CDT has an 800x600 panel, but mode 13h is 320x200,
 * which the VGA hardware outputs as 640x400.  The Chips & Technologies
 * 65550 then *centres* that in the panel rather than scaling it, which is
 * why FLOPOS sits in a letterbox with black all around it.  Toshiba's own
 * setup exposes "Text Mode Stretch" and nothing for graphics.
 *
 * The chip can do it anyway.  Its flat-panel register file is reached
 * through an index at 0x3D0 and data at 0x3D1, and two registers in it
 * turn graphics expansion on:
 *
 *      FR40   0x07 -> 0x3F
 *      FR48   0x13 -> 0x1F
 *
 * with FR41, FR4D and FR4E carrying setup that is written either way.
 *
 * Provenance: these register numbers and values were determined by
 * disassembling VEXP 1.3, a 1997 freeware utility by Adrian Gradwell that
 * does the same job for DOS.  What was taken is what the *hardware* does --
 * facts about a C&T chip, not his program.  None of his code is here; this
 * is written from scratch and works differently, because it can.  VEXP
 * needed a memory-resident program to keep the settings, since the video
 * BIOS rewrites these registers on every mode change.  FLOPOS sets mode 13h
 * once, in the boot sector, and never changes mode again -- so it simply
 * sets them afterwards and they stay put.
 *
 * Nothing here assumes the chip is present.  The old values are read back
 * and kept, so this is always reversible, and if the register file does not
 * answer at all the whole thing declines to act.
 * ===========================================================================
 */
#include "flopos.h"
#include "io.h"

#define FP_INDEX 0x3D0
#define FP_DATA  0x3D1

#define FR_PANEL_FORMAT  0x40
#define FR_PANEL_CTRL    0x41
#define FR_STRETCH       0x48
#define FR_HORZ_RATIO    0x4D
#define FR_VERT_RATIO    0x4E

static const u8 s_regs[] = {
    FR_PANEL_FORMAT, FR_PANEL_CTRL, FR_STRETCH, FR_HORZ_RATIO, FR_VERT_RATIO
};
#define NREGS ((int)(sizeof s_regs / sizeof s_regs[0]))

/* Expansion on, graphics only.  The font-stretch variant sets FR48 to 0x17
 * instead, which matters for text modes; FLOPOS has no text mode. */
static const u8 s_wide[NREGS] = { 0x3F, 0x07, 0x1F, 0x40, 0x3F };

static u8 s_saved[NREGS];
static int s_present;
static int s_on;

static u8 fp_read(u8 reg)
{
    outb(FP_INDEX, reg);
    io_wait();
    return inb(FP_DATA);
}

static void fp_write(u8 reg, u8 val)
{
    outb(FP_INDEX, reg);
    io_wait();
    outb(FP_DATA, val);
    io_wait();
}

void panel_init(void)
{
    int i, all_ff = 1;

    /* 0x3D0/0x3D1 are not standard VGA registers, so on a machine with no
     * flat-panel controller behind them the reads come back off an open
     * bus as 0xFF.  That is the only evidence available without a chip ID
     * worth trusting, and it is enough to keep us from poking at hardware
     * that is not there. */
    for (i = 0; i < NREGS; i++) {
        s_saved[i] = fp_read(s_regs[i]);
        if (s_saved[i] != 0xFF) all_ff = 0;
    }

    s_present = !all_ff;
    s_on = 0;
}

int panel_present(void) { return s_present; }
int panel_wide(void)    { return s_on; }

void panel_set_wide(int on)
{
    int i;

    if (!s_present) return;

    if (on) {
        for (i = 0; i < NREGS; i++) fp_write(s_regs[i], s_wide[i]);
    } else {
        /* Put back exactly what was there at boot, rather than what the
         * manual says should have been. */
        for (i = 0; i < NREGS; i++) fp_write(s_regs[i], s_saved[i]);
    }
    s_on = on;
}
