/* ===========================================================================
 * room.c -- the script room: a second wall, beside the app's.
 *
 * browzer runs JavaScript, but not itself: the scripts run in a room of
 * their own (src/room/), 4 MB that browzer's own memory is hidden from, and
 * that cannot reach the network, the floppy, the hardware or anything
 * else.  While the room works, browzer's memory is marked the system's,
 * so the room cannot see it; while browzer works, the room's is.  They
 * take turns: the Pentium has one core.
 *
 * The room's memory is mapped in 4 KB pages, so each part is locked as
 * it should be: the engine's code read-only, a page that is not there on
 * either side of its stack, the rest its own to write.
 *
 * The only way out of the room is the door: the room asks, the kernel
 * copies the request into browzer's memory and calls browzer's door
 * routine, then copies the answer back.  No pointer ever crosses.
 *
 * The room is stopped -- not asked, stopped -- if it faults, if it asks
 * for anything but its three requests, or if it runs past its time.
 * Then it is unloaded, and must be loaded afresh before it runs again.
 * ===========================================================================
 */
#include "flopos.h"
#include "inflate.h"
#include "io.h"
#include "../apps/sdk/app.h"
#include "../room/room.h"

u32  wall_go(u32 eip, u32 usp, u32 *kesp, u32 *esp0);
void wall_back(u32 kesp, u32 value);
u32 *wall_pdir(void);
u32 *wall_esp0(void);
u32  wall_call(u32 fn, u32 a0, u32 a1, u32 a2);
extern int g_irq_depth;

static u32 s_pt[1024] __attribute__((aligned(4096)));
static int s_loaded;                     /* ROOM.BIN is in, and well       */
static int s_current;                    /* the room is the one running now */
static int s_in_task;
static u32 s_kesp, s_entry, s_ro_end, s_deadline, s_left_at;
static int s_irq_depth;
static const char *s_why = "";

static u32 s_door_fn, s_door_buf, s_door_cap;   /* browzer's, for this task */

const char *room_why(void) { return s_why; }
int room_current(void) { return s_current; }

static void flush(void)
{
    u32 cr3;
    __asm__ __volatile__("movl %%cr3, %0; movl %0, %%cr3" : "=r"(cr3) :: "memory");
}

/* Who may touch what: the room, or the app. */
static void map(int room)
{
    u32 *pd = wall_pdir(), i;
    for (i = APP_BASE >> 22; i < (APP_BASE + APP_MAX) >> 22; i++) {
        if (room) pd[i] &= ~4u; else pd[i] |= 4u;
    }
    pd[ROOM_BASE >> 22] = room ? ((u32)s_pt | 7u) : (ROOM_BASE | 0x83u);
    flush();
}

/* Called around every call into the app (wall_call): the app's memory
 * back while it runs, the room's again after.  The room's clock stops
 * meanwhile. */
int room_leave(void)
{
    if (!s_current) return 0;
    s_current = 0;
    s_left_at = ms_now();
    map(0);
    return 1;
}

void room_back(int was)
{
    if (!was) return;
    s_deadline += ms_now() - s_left_at;
    map(1);
    s_current = 1;
}

/* The app has gone (left or was closed): the room goes with it. */
void room_drop(void)
{
    if (s_current || s_in_task) map(0);
    s_current = 0;
    s_in_task = 0;
    s_loaded = 0;
}

static int memory_for_room(void)
{
    u32 top = 0x00100000u + BOOTINFO_EXTKB * 1024u;
    return !heap_is_low() && top >= ROOM_END;
}

int room_open(void)
{
    FsFile f;
    RoomHeader *h = (RoomHeader *)ROOM_BASE;
    u32 i;
    if (s_loaded) return 1;
    if (!wall_on()) { s_why = "this machine cannot keep scripts apart from the rest (no 4 MB pages)"; return 0; }
    if (!memory_for_room()) { s_why = "scripts need 24 MB of memory; this machine has less"; return 0; }
    if (!fs_mount() || !fs_find("ROOM.BIN", &f)) { s_why = "ROOM.BIN is not on the floppy"; return 0; }
    map(0);
    {
        const char *why = "";
        int got = load_packed(&f, (u8 *)ROOM_BASE, ROOM_GAP1 - ROOM_BASE, &why);   /* packed or not */
        if (got < 0) { s_why = "ROOM.BIN did not load"; serial_printf("[room] ROOM.BIN: %s\n", why); return 0; }
        if (got < (int)sizeof(RoomHeader)) { s_why = "ROOM.BIN is not the script room"; return 0; }
        f.size = (u32)got;
    }
    if (h->magic[0] != 'F' || h->magic[4] != 'R' || h->magic[7] != 'M' || h->version != ROOM_VERSION ||
        h->load_addr != ROOM_BASE || (h->ro_end & 0xFFF) || h->ro_end < ROOM_BASE || h->ro_end > h->image_end ||
        h->image_end > h->bss_end || h->bss_end > ROOM_GAP1 || h->entry < ROOM_BASE || h->entry >= h->ro_end) {
        s_why = "ROOM.BIN is not the script room";
        return 0;
    }
    memset8((void *)h->image_end, 0, h->bss_end - h->image_end);
    s_entry = h->entry;
    s_ro_end = h->ro_end;
    /* The pages: code read-only; the gaps not there; the rest the room's. */
    for (i = 0; i < 1024; i++) {
        u32 a = ROOM_BASE + i * 4096u;
        if (a == ROOM_GAP1 || a == ROOM_GAP2) s_pt[i] = 0;
        else if (a < s_ro_end) s_pt[i] = a | 5u;          /* present, user, read-only */
        else s_pt[i] = a | 7u;                            /* present, user, writable  */
    }
    s_loaded = 1;
    serial_printf("[room] loaded: %u bytes, code read-only to %x\n", f.size, s_ro_end);
    return 1;
}

/* Is [at, at+len) the room's own writable memory? */
static int room_writable(u32 at, u32 len)
{
    if (at < s_ro_end || at > ROOM_END || len > ROOM_END - at) return 0;
    if (at < ROOM_GAP1 + 4096u && at + len > ROOM_GAP1) return 0;
    if (at < ROOM_GAP2 + 4096u && at + len > ROOM_GAP2) return 0;
    return 1;
}
static int room_readable(u32 at, u32 len)
{
    if (at < ROOM_BASE || at > ROOM_END || len > ROOM_END - at) return 0;
    if (at < ROOM_GAP1 + 4096u && at + len > ROOM_GAP1) return 0;
    if (at < ROOM_GAP2 + 4096u && at + len > ROOM_GAP2) return 0;
    return 1;
}

static void stop(const char *why)
{
    s_why = why;
    serial_printf("[room] stopped: %s\n", why);
    wall_back(s_kesp, 0xFFFFFFFFu);
}

/* One task: in goes the task, out comes the answer's length, or -1 if the
 * room had to be stopped (room_why says why; it is unloaded). */
int room_task(const u8 *task, u32 len, u8 *answer, u32 cap, u32 ms, u32 door_fn, u8 *door_buf, u32 door_cap)
{
    u32 *st, r, keep_esp0, i;
    if (!s_loaded) { s_why = "the script room is not loaded"; return -1; }
    if (s_in_task) { s_why = "the script room is busy"; return -1; }
    if (!len || len > ROOM_MAIL_SIZE) { s_why = "a task too big for the room"; return -1; }
    for (i = 0; i < len; i++) ((u8 *)ROOM_MAIL)[i] = task[i];
    s_door_fn = door_fn; s_door_buf = (u32)door_buf; s_door_cap = door_cap;
    s_in_task = 1;
    s_irq_depth = g_irq_depth;
    map(1);
    s_current = 1;
    s_deadline = ms_now() + ms + 1500;   /* the engine stops itself at ms; this is the backstop */
    st = (u32 *)(ROOM_GAP2 - 16u);
    st[0] = 0;                           /* nowhere to return to: room_main never returns */
    st[1] = len;
    keep_esp0 = *wall_esp0();
    r = wall_go(s_entry, (u32)st, &s_kesp, wall_esp0());
    *wall_esp0() = keep_esp0;
    s_current = 0;
    map(0);
    s_in_task = 0;
    g_irq_depth = s_irq_depth;
    sti();
    if (r == 0xFFFFFFFFu) { s_loaded = 0; return -1; }
    if (r > cap) r = cap;
    for (i = 0; i < r; i++) answer[i] = ((u8 *)ROOM_MAIL)[i];
    return (int)r;
}

/* The room knocked: INT 0x80 from inside it. */
int room_trap(Regs *r)
{
    if (r->vector == 0x81) stop("it answered a call nobody made");
    if (r->vector != 0x80) return 0;
    switch (r->eax) {
    case RS_DONE:
        wall_back(s_kesp, r->ebx <= ROOM_MAIL_SIZE ? r->ebx : 0);
        return 1;
    case RS_MS:
        r->eax = ms_now();
        return 1;
    case RS_DOOR: {
        u32 in = r->ebx, len = r->ecx, out = r->edx, cap = r->esi, got, i;
        if (!room_readable(in, len) || !room_writable(out, cap)) stop("it pointed the door outside its own memory");
        if (len > s_door_cap || !s_door_fn) { r->eax = 0xFFFFFFFFu; return 1; }
        for (i = 0; i < len; i++) ((u8 *)s_door_buf)[i] = ((const u8 *)in)[i];
        sti();
        got = wall_call(s_door_fn, s_door_buf, len, s_door_cap);   /* browzer answers */
        cli();
        if ((s32)got < 0 || got > cap || got > s_door_cap) { r->eax = 0xFFFFFFFFu; return 1; }
        for (i = 0; i < got; i++) ((u8 *)out)[i] = ((const u8 *)s_door_buf)[i];
        r->eax = got;
        return 1;
    }
    }
    stop("it asked for something that does not exist");
    return 1;
}

void room_fault(Regs *r, const char *what)
{
    u32 cr2;
    __asm__ __volatile__("movl %%cr2, %0" : "=r"(cr2));
    serial_printf("[room] fault: %s at %x, reaching for %x\n", what, r->eip, cr2);
    stop(r->vector == 14 ? "it reached outside its own memory" : "it tried something only the system may do");
}

/* From the timer, after the interrupt is acknowledged: a room that has run
 * past its time while it had the processor is stopped. */
void room_tick(Regs *r)
{
    if (s_current && (r->cs & 3) == 3 && (s32)(ms_now() - s_deadline) > 0) stop("it ran past its time, and the system stopped it");
}
