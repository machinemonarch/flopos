/* ===========================================================================
 * wall.c -- apps kept to their own memory.
 *
 * Until 1.33 an app ran as part of the system: it could write anywhere,
 * talk to any hardware, and one overflow in a decoder owned the machine
 * and the floppy in it.  Now an app runs at the CPU's lowest privilege
 * (ring 3) with paging on:
 *
 *   - it can touch its own 12 MB (APP_BASE up) and nothing else.  The rest
 *     of memory is the system's, and reaching for it is a page fault;
 *   - it cannot use I/O ports or switch interrupts off;
 *   - everything it wants from the system it asks for through one door,
 *     INT 0x80, and the system checks every pointer it is handed before
 *     believing it (apps.c, app_ask);
 *   - when it breaks one of these rules it is closed, with a sentence
 *     saying why, and FLOPOS carries on.
 *
 * The page table is as plain as it can be: 1024 four-megabyte pages, each
 * mapped to itself, so the system sees memory exactly as before.  Two of
 * them (the app's 8 MB) are marked as the app's; the rest are the
 * system's alone.  Four-megabyte pages need a Pentium (PSE); on a CPU
 * without them the wall is left down and apps run as they used to.
 *
 * Apps are not changed for this.  They still get a table of functions;
 * behind the wall the table lives in a corner of the app's own memory and
 * each function in it is eight bytes that say "INT 0x80, number N".
 *
 * The system sometimes calls into an app (the song refill from the timer,
 * a line of progress while connecting, each file of a listing).  That is
 * wall_call: it drops to the app's privilege at the function, on the
 * app's stack, with a return address that is "INT 0x81"; that door brings
 * it back here.  These can nest, and can be made from inside an interrupt.
 * ===========================================================================
 */
#include "flopos.h"
#include "io.h"
#include "../apps/sdk/app.h"

u32  wall_go(u32 eip, u32 usp, u32 *kesp, u32 *esp0);
void wall_back(u32 kesp, u32 value);
extern u8 gdt[];

#define APP_END      (APP_BASE + APP_MAX)
#define CORNER       (APP_END - 0x1000)      /* the last page: the table, the stubs, strings */
#define C_API        (CORNER + 0x000)
#define C_STUBS      (CORNER + 0x200)
#define C_EXIT       (CORNER + 0x500)
#define C_UPRET      (CORNER + 0x510)
#define C_STRINGS    (CORNER + 0x540)        /* 3 of 128 bytes                */
#define C_ARGV       (CORNER + 0x700)
#define C_SCRATCH    (CORNER + 0x900)        /* 512 bytes                     */
#define ASK_EXIT     250
#define NCALLS       ((sizeof(Api) - 4) / 4)

static u32 s_pd[1024] __attribute__((aligned(4096)));
static struct { u32 link, esp0, ss0, rest[23]; } s_tss;
static int s_on;                         /* the wall is up                  */
static int s_running;                    /* an app is behind it now         */
static u32 s_app_kesp;                   /* back to wall_run                */
static u32 s_up_kesp;                    /* back to the innermost wall_call */
static u32 s_user_esp;                   /* where the app's stack was, last time it was seen */
static const char *s_killed;             /* why it was closed, if it was    */

int wall_on(void) { return s_on; }
u32 *wall_pdir(void) { return s_pd; }
u32 *wall_esp0(void) { return &s_tss.esp0; }

/* The script room (room.c) is a second wall beside the app's. */
int  room_current(void);
int  room_trap(Regs *r);
void room_fault(Regs *r, const char *what);
int  room_leave(void);
void room_back(int was);
void room_drop(void);
u32 wall_scratch(void) { return C_SCRATCH; }

static int in_app(u32 at, u32 len)
{
    return at >= APP_BASE && at <= APP_END && len <= APP_END - at;
}

void wall_paging(int on)
{
    u32 cr0;
    if (!s_on) return;
    __asm__ __volatile__("movl %%cr0, %0" : "=r"(cr0));
    if (on) cr0 |= 0x80000000u; else cr0 &= ~0x80000000u;
    __asm__ __volatile__("movl %0, %%cr0" :: "r"(cr0));
}

void wall_init(void)
{
    u32 a, b, c, d, i, cr4;
    u32 base = (u32)&s_tss;

    if (!g_has_cpuid) return;
    __asm__ __volatile__("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1));
    if (!(d & 8)) return;                /* no four-megabyte pages: no wall */

    for (i = 0; i < 1024; i++) s_pd[i] = (i << 22) | 0x83;       /* present, writable, 4 MB */
    for (i = APP_BASE >> 22; i < APP_END >> 22; i++) s_pd[i] |= 4;   /* the app's own      */

    /* The task state: all the CPU wants of it is where the system's stack
     * is when an app is interrupted.  No I/O map, so no ports for apps. */
    s_tss.ss0 = 0x10;
    ((u16 *)&s_tss)[51] = sizeof s_tss;  /* I/O map base: past the end      */
    gdt[0x38 + 0] = (u8)(sizeof s_tss - 1); gdt[0x38 + 1] = 0;
    gdt[0x38 + 2] = (u8)base; gdt[0x38 + 3] = (u8)(base >> 8); gdt[0x38 + 4] = (u8)(base >> 16);
    gdt[0x38 + 5] = 0x89; gdt[0x38 + 6] = 0; gdt[0x38 + 7] = (u8)(base >> 24);
    __asm__ __volatile__("ltr %w0" :: "r"((u16)0x38));

    __asm__ __volatile__("movl %%cr4, %0" : "=r"(cr4));
    cr4 |= 0x10;                         /* PSE                             */
    __asm__ __volatile__("movl %0, %%cr4" :: "r"(cr4));
    __asm__ __volatile__("movl %0, %%cr3" :: "r"((u32)s_pd));
    s_on = 1;
    wall_paging(1);
    serial_print("[wall] up: apps are kept to their own memory\n");
}

/* ---- closing an app ----------------------------------------------------- */
void wall_kill(const char *why)
{
    if (!s_running) return;
    s_killed = why;
    wall_back(s_app_kesp, 0);
}

void wall_fault(Regs *r, const char *what)
{
    static char why[96];
    u32 cr2;
    /* An overrun stack caught by its canary (crt.c, room.c) stops with
     * this word in eax, so it can be told apart from any other stumble. */
    if (r->vector == 6 && r->eax == STACK_SMASHED) what = "its stack was overrun, and it was stopped before that could be used";
    if (room_current()) { room_fault(r, what); return; }
    __asm__ __volatile__("movl %%cr2, %0" : "=r"(cr2));
    serial_printf("[wall] app fault: %s at %x, reaching for %x, err %x\n", what, r->eip, cr2, r->err);
    if (r->vector == 14) fmt_str(why, sizeof why, "it reached outside its own memory");
    else if (r->vector == 13) fmt_str(why, sizeof why, "it tried something only the system may do");
    else if (r->vector == 6 && r->eax == STACK_SMASHED) fmt_str(why, sizeof why, "%s", what);
    else fmt_str(why, sizeof why, "it stumbled (%s)", what);
    wall_kill(why);
}

/* ---- calls into the app ---------------------------------------------------- */
u32 wall_call(u32 fn, u32 a0, u32 a1, u32 a2)
{
    u32 usp = (s_user_esp - 64) & ~3u, r;
    u32 keep_esp0 = s_tss.esp0, keep_usp = s_user_esp, keep_up = s_up_kesp;
    u32 flags;
    u32 *st;
    int room;

    if (!s_running) return 0;
    room = room_leave();                 /* the app's memory is the app's again while it runs */
    usp -= 16;
    if (!in_app(usp - 4096, 4096 + 16) || !in_app(fn, 1)) wall_kill("it named a routine outside its own memory");
    st = (u32 *)usp;
    st[0] = C_UPRET; st[1] = a0; st[2] = a1; st[3] = a2;
    __asm__ __volatile__("pushfl; popl %0" : "=r"(flags));
    r = wall_go(fn, usp, &s_up_kesp, &s_tss.esp0);
    s_tss.esp0 = keep_esp0; s_user_esp = keep_usp; s_up_kesp = keep_up;
    room_back(room);
    if (flags & 0x200) sti(); else cli();
    return r;
}

/* A string of the system's, copied to where the app can read it. */
u32 wall_string(int slot, const char *s)
{
    char *d = (char *)(C_STRINGS + 128 * (u32)slot);
    int i;
    for (i = 0; s && s[i] && i < 127; i++) d[i] = s[i];
    d[i] = 0;
    return (u32)d;
}

/* ---- the doors ------------------------------------------------------------- */
u32 g_wall_asks;                         /* requests from apps, counted (test builds report them) */

int wall_trap(Regs *r)
{
    if (room_current()) return room_trap(r);   /* the room's doors are not the app's */
    s_user_esp = r->useresp;
    if (r->vector == 0x80) {
        g_wall_asks++;
        u32 a[16], i, result = 0;
        if (r->eax == ASK_EXIT) wall_back(s_app_kesp, 0);
        if (!in_app(r->useresp, 4 + sizeof a)) wall_kill("its stack is outside its memory");
        for (i = 0; i < 16; i++) a[i] = ((u32 *)r->useresp)[1 + i];
        sti();                           /* asking can take a while, and the clock must tick */
        if (!app_ask(r->eax, a, &result)) wall_kill("it handed over memory that is not its own");
        cli();
        r->eax = result;
        return 1;
    }
    if (r->vector == 0x81) {
        if (!s_up_kesp) wall_kill("it answered a call nobody made");
        wall_back(s_up_kesp, r->eax);
    }
    return 0;
}

/* ---- running one ------------------------------------------------------------- */
int wall_run(u32 entry, int argc, char **argv)
{
    u32 *api = (u32 *)C_API, *st, *av = (u32 *)C_ARGV;
    u8 *stub = (u8 *)C_STUBS, *p;
    char *text = (char *)(C_ARGV + 64);
    u32 i, usp;
    int a, k;

    /* The corner: the table of functions, each one a few bytes that knock
     * on the door with its number. */
    memset8((void *)CORNER, 0, 0x1000);
    api[0] = APP_API_VERSION;
    for (i = 0; i < NCALLS; i++) {
        api[1 + i] = (u32)(stub + 8 * i);
        stub[8 * i + 0] = 0xB8; stub[8 * i + 1] = (u8)i;         /* mov eax, i */
        stub[8 * i + 5] = 0xCD; stub[8 * i + 6] = 0x80;          /* int 0x80   */
        stub[8 * i + 7] = 0xC3;                                  /* ret        */
    }
    p = (u8 *)C_EXIT;  p[0] = 0xB8; p[1] = ASK_EXIT; p[5] = 0xCD; p[6] = 0x80; p[7] = 0xEB; p[8] = 0xFE;
    p = (u8 *)C_UPRET; p[0] = 0xCD; p[1] = 0x81; p[2] = 0xEB; p[3] = 0xFE;

    /* Its arguments, copied to where it can read them. */
    if (argc > 15) argc = 15;
    for (a = 0; a < argc; a++) {
        av[a] = (u32)text;
        for (k = 0; argv[a][k] && (u32)text < C_SCRATCH - 2; k++) *text++ = argv[a][k];
        *text++ = 0;
    }
    av[argc] = 0;

    usp = CORNER - 16;
    st = (u32 *)usp;
    st[0] = C_EXIT; st[1] = C_API; st[2] = (u32)argc; st[3] = C_ARGV;

    s_killed = 0; s_up_kesp = 0; s_user_esp = usp; s_running = 1;
    wall_go(entry, usp, &s_app_kesp, &s_tss.esp0);
    room_drop();                         /* its script room goes with it   */
    s_running = 0;
    g_irq_depth = 0;                     /* it may have been closed from inside an interrupt */
    sti();
    if (s_killed) {
        term_win_close();
        term_colour(C_ACCENT);
        term_print("The app was closed: ");
        term_print(s_killed);
        term_print(".");
        term_newline();
        store_journal_printf("an app was closed: %s", s_killed);
        return 0;
    }
    return 1;
}
