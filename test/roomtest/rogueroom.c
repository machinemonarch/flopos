/* rogueroom.c -- a script room that breaks the rules on purpose, one per
 * task, to see the kernel (src/kernel/room.c) hold.  Built in ROOM.BIN's
 * shape and put in its place on a scratch image by run.py.
 *
 *   10  read the app's memory          16  run for ever (no engine to stop it)
 *   11  write its own code              17  point the door at its own code
 *   12  fall into the gap under its stack 18  answer a call nobody made
 *   13  switch interrupts off           19  ask for something that does not exist
 *   14  talk to hardware                20  nothing wrong: through the door and back
 *   15  write the system's memory
 */
#include "../../src/room/room.h"

typedef unsigned char u8;
typedef unsigned int u32;
extern char _room_ro_end[], _room_image_end[], _room_bss_end[];
void room_main(u32 len);

__attribute__((section(".roomhdr"), used))
const RoomHeader room_header = {
    { 'F', 'L', 'O', 'P', 'R', 'O', 'O', 'M' }, ROOM_VERSION, ROOM_BASE,
    (unsigned)_room_ro_end, (unsigned)_room_image_end, (unsigned)_room_bss_end, (unsigned)room_main
};

static volatile u32 sink;
static u8 req[8], got[8];

static u32 sys(u32 n, u32 a, u32 b, u32 c, u32 d)
{
    u32 r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c), "S"(d) : "memory");
    return r;
}

static void answer(const char *s)
{
    u8 *m = (u8 *)ROOM_MAIL;
    u32 n = 6;
    m[0] = 0; m[1] = m[2] = m[3] = m[4] = m[5] = 0;
    while (*s) m[n++] = (u8)*s++;
    sys(RS_DONE, n, 0, 0, 0);
    for (;;) { }
}

void room_main(u32 len)
{
    u8 task = ((u8 *)ROOM_MAIL)[0];
    (void)len;
    switch (task) {
    case 10: sink = *(volatile u32 *)0x00C00100; break;
    case 11: *(volatile u32 *)(ROOM_BASE + 0x40) = 0; break;
    case 12: *(volatile u32 *)(ROOM_GAP1 + 8) = 0; break;
    case 13: __asm__ __volatile__("cli"); break;
    case 14: __asm__ __volatile__("outb %%al, $0x80" :: "a"(0)); break;
    case 15: *(volatile u32 *)0x00020000 = 0xDEADBEEF; break;
    case 16: for (;;) sink++;
    case 17: req[0] = 'p'; req[1] = 'i'; req[2] = 'n'; req[3] = 'g'; sys(RS_DOOR, (u32)req, 4, ROOM_BASE + 0x40, 8); break;
    case 18: __asm__ __volatile__("int $0x81"); break;
    case 19: sys(99, 0, 0, 0, 0); break;
    case 20: {
        u32 n;
        req[0] = 'p'; req[1] = 'i'; req[2] = 'n'; req[3] = 'g';
        n = sys(RS_DOOR, (u32)req, 4, (u32)got, sizeof got);
        got[n < 8 ? n : 7] = 0;
        answer(n == 4 ? (const char *)got : "the door did not answer");
    }
    }
    answer("still running after breaking a rule");
}
