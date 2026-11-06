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
 * The timer is reprogrammed after every call.  The BIOS owns channel 0 of
 * the 8254 as far as it is concerned and sets it back to its own 18.2Hz
 * while servicing the disk, which leaves FLOPOS's clock running thirteen
 * times slow afterwards.  Everything timed off that clock -- the typing on
 * the boot screen, the sequencer, every delay -- then crawls, which looks
 * exactly like the machine having frozen.
 * ===========================================================================
 */
#include "flopos.h"
#include "io.h"

#define SPT   18
#define HEADS 2

static u8 s_last_status;

u8 disk_last_status(void) { return s_last_status; }

/* The trip through real mode begins with cli and cannot end with sti --
 * the caller might have had interrupts off for its own reasons.  So the
 * flag is saved here and put back exactly as it was found.  Forgetting
 * this leaves the timer and the keyboard dead the moment anything touches
 * the disk, which presents as the whole machine freezing. */
static u32 irq_save(void)
{
    u32 f;
    __asm__ __volatile__("pushfl; popl %0" : "=r"(f) :: "memory");
    return f;
}

static void irq_restore(u32 f)
{
    if (f & 0x200) sti();
}

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

        pic_to_bios();
        r = bios_disk(ax, cx, dx, DISK_BUF_SEG);
        pic_to_kernel();
        time_init();                     /* see below                      */
        kbd_resync();                    /* and see kbd_resync             */

        s_last_status = (u8)((r >> 8) & 0xFF);
        if (!(r & 0x10000)) { irq_restore(flags); return 1; }

        /* Recalibrate and try again; a drive that has been idle often
         * fails the first seek and succeeds on the second. */
        pic_to_bios();
        (void)bios_disk(0x0000, 0, (u16)BOOTINFO_DRIVE, DISK_BUF_SEG);
        pic_to_kernel();
        time_init();
        kbd_resync();
    }

    irq_restore(flags);
    return 0;
}

int disk_read(u32 lba, int count)  { return transfer(lba, count, 0); }
int disk_write(u32 lba, int count) { return transfer(lba, count, 1); }
