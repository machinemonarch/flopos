/* rogue.c -- an app that breaks the rules on purpose, one rule per number,
 * to see the wall (src/kernel/wall.c) hold.  Never put on a floppy: run.py
 * builds it and copies it onto a scratch image.
 *
 *   rogue 1   write into the system's memory
 *   rogue 2   read the system's memory
 *   rogue 3   talk to hardware (an I/O port)
 *   rogue 4   switch interrupts off
 *   rogue 5   hand the system a string that is not the app's
 *   rogue 6   hand the system a buffer that is not the app's, to fill
 *   rogue 7   have the system format a string that is not the app's
 *   rogue 8   jump into the system
 *   rogue 9   answer a call nobody made
 *   rogue 10  overwrite the system's own file (must be refused, not closed)
 *   rogue 11  fall down its stack for ever
 *   rogue 12  name a routine outside itself for the system to call
 *   rogue 0   nothing wrong: says so
 */
#include "app.h"

static const Api *A;
static volatile u32 sink;

static int deeper(int n) { volatile char pad[512]; pad[0] = (char)n; return deeper(n + 1) + pad[0]; }

int app_main(const Api *api, int argc, char **argv)
{
    int n = 0, i;
    static u8 buf[512];
    char line[64];
    AppFile f;
    A = api;
    for (i = 0; argc > 1 && argv[1][i]; i++) n = n * 10 + (argv[1][i] - '0');

    switch (n) {
    case 1:  *(volatile u32 *)0x00020000 = 0xDEADBEEF; break;
    case 2:  sink = *(volatile u32 *)0x00100000; break;
    case 3:  __asm__ __volatile__("outb %%al, $0x80" :: "a"(0)); break;
    case 4:  __asm__ __volatile__("cli"); break;
    case 5:  A->print((const char *)0x00020000); break;
    case 6:  A->inv_read("NOTHING.MD", (u8 *)0x00100000, 4096); break;
    case 7:  A->format(line, sizeof line, "%s", (const char *)0x00020000); break;
    case 8:  ((void (*)(void))0x00010000)(); break;
    case 9:  __asm__ __volatile__("int $0x81"); break;
    case 10:
        if (!A->file_find("KERNEL.BIN", &f)) { A->print("no kernel?"); A->newline(); return 0; }
        A->print(A->file_write(&f, buf) ? "[rogue] the system's file was written" : "[rogue] refused");
        A->newline();
        return 0;
    case 11: sink = (u32)deeper(0); break;
    case 12: A->audio_open(22050); A->audio_feeder((void (*)(void))0x00010000); break;
    default:
        A->print("[rogue] behaving");
        A->newline();
        return 0;
    }
    A->print("[rogue] still here: the wall did not hold");
    A->newline();
    return 0;
}
