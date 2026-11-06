/* ===========================================================================
 * fault.c -- the last thing FLOPOS does.
 *
 * Something has gone wrong early enough that the terminal does not exist
 * yet, so this draws straight into the VGA aperture.  It sets the three
 * palette entries it uses rather than trusting whatever is in the DAC:
 * early on that is the BIOS default, but later it would be some dim mood
 * colour, and an unreadable error message is no message at all.
 *
 * The point is that a failure on hardware I cannot see still produces a
 * sentence somebody can read out to me, rather than a hang.
 * ===========================================================================
 */
#include "flopos.h"
#include "io.h"

void fatal(const char *what, const char *detail)
{
    u8 *fb = (u8 *)VGA_FB;               /* no back buffer to be had        */
    int i;

    cli();
    snd_silence();

    /* Claim the three indices this uses: black, grey, white. */
    outb(0x3C8, 0);  outb(0x3C9, 0);  outb(0x3C9, 0);  outb(0x3C9, 0);
    outb(0x3C8, 7);  outb(0x3C9, 34); outb(0x3C9, 34); outb(0x3C9, 40);
    outb(0x3C8, 15); outb(0x3C9, 63); outb(0x3C9, 63); outb(0x3C9, 63);

    memset8(fb, 0, SCR_PIX);
    for (i = 0; i < SCR_W; i++) fb[8 * SCR_W + i] = 15;

    draw_text(fb, 8, 16, "FLOPOS cannot start.", 15);
    draw_text(fb, 8, 32, what, 15);
    if (detail) draw_text(fb, 8, 42, detail, 7);

    draw_text(fb, 8, 64, "This machine reports:", 7);
    {
        char line[64];
        fmt_str(line, sizeof line, "  %u kb of memory above 1mb", BOOTINFO_EXTKB);
        draw_text(fb, 8, 76, line, 15);
        fmt_str(line, sizeof line, "  a20 gate %s, bios drive %x",
                BOOTINFO_A20 ? "open" : "unverified", BOOTINFO_DRIVE);
        draw_text(fb, 8, 86, line, 15);
    }

    draw_text(fb, 8, 112, "Please tell whoever built this what it says,", 7);
    draw_text(fb, 8, 122, "including the two numbers above.", 7);
    draw_text(fb, 8, 144, "Ctrl+Alt+Del or the power switch.", 15);

    /* Interrupts are off, so watch for the reboot key by hand. */
    for (;;) {
        if (inb(0x64) & 1) {
            u8 sc = inb(0x60);
            if (sc == 0x53) reboot();
        }
    }
}
