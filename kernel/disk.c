/* ===========================================================================
 * disk.c -- sectors on and off the floppy, through the machine's own BIOS.
 *
 * Everything goes via a fixed buffer at 0x70000: below the 1MB line so real
 * mode can see it, and far enough from a 64K boundary that the controller's
 * DMA will not wrap in the middle of a transfer.
 *
 * The geometry is the one every 1.44MB floppy has had since 1987 -- 80
 * cylinders, 2 heads, 18 sectors -- and the boot sector already relies on
 * it, so there is no sense discovering it twice.
 *
 * Everything about getting in and out of the BIOS safely -- the interrupt
 * controllers, the timer it reprograms behind our back, the keyboard and
 * serial lines it leaves stranded -- lives in bios_int() now, in bioscall.c,
 * so that no caller can get it wrong by forgetting a step.
 * ===========================================================================
 */
#include "flopos.h"
#include "io.h"

#define SPT   18
#define HEADS 2

static u8 s_last_status;

u8 disk_last_status(void) { return s_last_status; }

static int transfer(u32 lba, int count, int write)
{
    u32 track    = lba / SPT;
    u32 sector   = lba % SPT + 1;
    u32 head     = track % HEADS;
    u32 cylinder = track / HEADS;
    u32 r, flags;
    int tries;

    if (count < 1 || count > SPT) return 0;
    if (sector + (u32)count - 1 > SPT) return 0;   /* never cross a track  */

    flags = irq_save();

    for (tries = 0; tries < 4; tries++) {
        u16 ax = (u16)(((write ? 0x03 : 0x02) << 8) | (count & 0xFF));
        u16 cx = (u16)(((cylinder & 0xFF) << 8) | (sector & 0x3F));
        u16 dx = (u16)(((head & 0xFF) << 8) | BOOTINFO_DRIVE);

        r = bios_int(0x13, ax, 0, cx, dx, DISK_BUF_SEG, 0);

        s_last_status = (u8)((r >> 8) & 0xFF);
        if (!(r & 0x10000)) { irq_restore(flags); return 1; }

        /* Recalibrate and try again; a drive that has been idle often
         * fails the first seek and succeeds on the second. */
        (void)bios_int(0x13, 0x0000, 0, 0, (u16)BOOTINFO_DRIVE, DISK_BUF_SEG, 0);
    }

    irq_restore(flags);
    return 0;
}

int disk_read(u32 lba, int count)  { return transfer(lba, count, 0); }
int disk_write(u32 lba, int count) { return transfer(lba, count, 1); }
