/* ===========================================================================
 * room.c -- the engine at home in the script room.
 *
 * The kernel starts room_main for each task, with the task in the mail.
 * The engine's memory -- its variables, the scripts' heap -- stays as it
 * was between tasks: a page's scripts set things up when it loads, and
 * a click later finds them still there.  A new page clears it all.
 * ===========================================================================
 */
#include "js.h"
#include "engine.h"
#include "room.h"
#include "door.h"

extern char _room_ro_end[], _room_image_end[], _room_bss_end[];
void room_main(u32 len);

/* The stack's canary: random, written here by the kernel when the room is
 * loaded (room.c there), before any of the room's code runs. */
u32 __stack_chk_guard;

__attribute__((section(".roomhdr"), used))
const RoomHeader room_header = {
    { 'F', 'L', 'O', 'P', 'R', 'O', 'O', 'M' },
    ROOM_VERSION, ROOM_BASE,
    (unsigned)_room_ro_end, (unsigned)_room_image_end, (unsigned)_room_bss_end,
    (unsigned)room_main, (unsigned)&__stack_chk_guard
};

/* An overrun caught: stopped at once, with a word the kernel recognises. */
void __stack_chk_fail(void)
{
    __asm__ __volatile__("ud2" :: "a"(0x4B415453u));   /* STACK_SMASHED */
    for (;;) { }
}

void *memset(void *d, int c, __SIZE_TYPE__ n) { u8 *p = d; while (n--) *p++ = (u8)c; return d; }
void *memcpy(void *d, const void *s, __SIZE_TYPE__ n) { u8 *p = d; const u8 *q = s; while (n--) *p++ = *q++; return d; }
void *memmove(void *d, const void *s, __SIZE_TYPE__ n)
{
    u8 *p = d;
    const u8 *q = s;
    if (p < q) while (n--) *p++ = *q++;
    else { p += n; q += n; while (n--) *--p = *--q; }
    return d;
}

static u32 sys(u32 n, u32 a, u32 b, u32 c, u32 d)
{
    u32 r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c), "S"(d) : "memory");
    return r;
}

u32 host_ms(void) { return sys(RS_MS, 0, 0, 0, 0); }

int host_door(const u8 *in, u32 len, u8 *out, u32 cap)
{
    return (int)sys(RS_DOOR, (u32)in, len, (u32)out, cap);
}

__attribute__((noreturn)) static void done(u32 len)
{
    sys(RS_DONE, len, 0, 0, 0);
    for (;;) { }
}

static Vm s_vm;
static int s_ready;

static u32 rd32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
static void wr32(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }

extern u32 g_door_limit;
extern u32 g_current_script;             /* dom.c: document.currentScript */
extern u8  g_answer_flags;
static char s_src[256 * 1024];           /* a script's text, as it arrives in pieces */
static u32  s_srclen;

/* The answer: a code, the next timer, flags (1: an event's default was
 * prevented), a sentence. */
static __attribute__((noreturn)) void answer(int code, const char *why)
{
    u8 *m = (u8 *)ROOM_MAIL;
    u32 n = 6;
    m[0] = (u8)code;
    wr32(m + 1, s_ready ? engine_next_timer(&s_vm) : 0);
    m[5] = g_answer_flags;
    while (why && *why && n < 400) m[n++] = (u8)*why++;
    done(n);
}

/* An answer with a long text: a module's list of addresses. */
static __attribute__((noreturn)) void answer_list(const char *list)
{
    u8 *m = (u8 *)ROOM_MAIL;
    u32 n = 6;
    m[0] = A_OK;
    wr32(m + 1, s_ready ? engine_next_timer(&s_vm) : 0);
    m[5] = 0;
    while (*list && n < ROOM_MAIL_SIZE - 1) m[n++] = (u8)*list++;
    done(n);
}

int room_dispatch(Vm *vm, const u8 *task, u32 len, char *err, u32 errcap);   /* dom.c */
int room_dyn_done(Vm *vm, u32 ticket, const char *url, const char *why, char *err, u32 errcap);   /* dom.c */

void room_main(u32 len)
{
    static char err[300];
    u8 *m = (u8 *)ROOM_MAIL;
    u32 heap = ((u32)_room_bss_end + 0xFFFu) & ~0xFFFu;
    int r = 0;
    if (len < 1 || len > ROOM_MAIL_SIZE) answer(A_THREW, "a task the room did not understand");
    g_answer_flags = 0;
    g_door_limit = 2000;
    switch (m[0]) {
    case T_INIT: case T_CLEAR: {
        union { u64 u; f64 d; } now;
        u32 i;
        now.u = 0;
        for (i = 0; i < 8 && 1 + i < len; i++) now.u |= (u64)m[1 + i] << (8 * i);
        s_srclen = 0;
        s_ready = 0;
        engine_init(&s_vm, (u8 *)heap, ROOM_GAP1 - heap, now.d - (f64)host_ms());
        s_ready = 1;
        answer(A_OK, 0);
    }
    case T_SRCDROP:                      /* a script that will not come after all */
        s_srclen = 0;
        answer(A_OK, 0);
    case T_SRC:                          /* a piece of a script's text      */
        if (s_srclen + len - 1 > sizeof s_src) answer(A_ROOM, "the script is too long for the room");
        memcpy(s_src + s_srclen, m + 1, len - 1);
        s_srclen += len - 1;
        answer(A_OK, 0);
    case T_RUN: {                        /* [ms][node][name\0][the last piece] */
        u32 ms = rd32(m + 1), n = 9;
        const char *name = (const char *)m + 9;
        if (!s_ready) answer(A_THREW, "the room was not ready");
        while (n < len && m[n]) n++;
        n++;
        if (n > len) answer(A_THREW, "a task the room did not understand");
        if (s_srclen + (len - n) > sizeof s_src) { s_srclen = 0; answer(A_ROOM, "the script is too long for the room"); }
        memcpy(s_src + s_srclen, m + n, len - n);
        s_srclen += len - n;
        g_door_limit = 20000;
        g_current_script = rd32(m + 5);
        r = engine_run(&s_vm, s_src, s_srclen, name, ms, err, sizeof err);
        g_current_script = 0;
        s_srclen = 0;
        break;
    }
    case T_MODSCAN: {                    /* [ms][address\0][the last piece] */
        static char list[8000];
        u32 n = 5;
        const char *url = (const char *)m + 5;
        if (!s_ready) answer(A_THREW, "the room was not ready");
        while (n < len && m[n]) n++;
        n++;
        if (n > len || s_srclen + (len - n) > sizeof s_src) { s_srclen = 0; answer(A_ROOM, "the module is too long for the room"); }
        memcpy(s_src + s_srclen, m + n, len - n);
        s_srclen += len - n;
        r = engine_module_scan(&s_vm, s_src, s_srclen, url, list, sizeof list, err, sizeof err);
        s_srclen = 0;
        if (r == ENGINE_OK) answer_list(list);
        break;
    }
    case T_MODRUN: {                     /* [ms][address\0][n][addresses\0] */
        const char *dep[64];
        u32 n = 5, k, cnt;
        const char *url = (const char *)m + 5;
        if (!s_ready) answer(A_THREW, "the room was not ready");
        while (n < len && m[n]) n++;
        n++;
        if (n + 4 > len) answer(A_THREW, "a task the room did not understand");
        cnt = rd32(m + n); n += 4;
        if (cnt > 64) answer(A_THREW, "a task the room did not understand");
        for (k = 0; k < cnt; k++) {
            dep[k] = (const char *)m + n;
            while (n < len && m[n]) n++;
            if (n >= len) answer(A_THREW, "a task the room did not understand");
            n++;
        }
        g_door_limit = 20000;
        r = engine_module_run(&s_vm, url, dep, (int)cnt, rd32(m + 1), err, sizeof err);
        break;
    }
    case T_DYNDONE: {                    /* [ticket][address\0][why\0] */
        u32 n = 5;
        const char *url = (const char *)m + 5, *why;
        if (!s_ready) answer(A_OK, 0);
        while (n < len && m[n]) n++;
        if (n >= len) answer(A_THREW, "a task the room did not understand");
        why = (const char *)m + ++n;
        while (n < len && m[n]) n++;
        if (n >= len) answer(A_THREW, "a task the room did not understand");
        r = room_dyn_done(&s_vm, rd32(m + 1), url, why, err, sizeof err);
        break;
    }
    case T_TIMERS:
        if (!s_ready) answer(A_OK, 0);
        r = engine_timers(&s_vm, rd32(m + 1), err, sizeof err) < 0 ? ENGINE_TIME : ENGINE_OK;
        break;
    case T_CALL:
        if (!s_ready) answer(A_THREW, "the room was not ready");
        r = room_dispatch(&s_vm, m, len, err, sizeof err);
        break;
    default:
        answer(A_THREW, "a task the room did not understand");
    }
    answer(r == ENGINE_OK ? A_OK : r == ENGINE_SYNTAX ? A_SYNTAX : r == ENGINE_TIME ? A_TIME :
           r == ENGINE_ROOM ? A_ROOM : A_THREW, r ? err : 0);
}

/* ---- what the compiler asks for and there is no library to give -------- */
/* 64-bit division, done the long way: the Pentium divides 64 by 32 only. */
u64 __udivmoddi4(u64 n, u64 d, u64 *rem)
{
    u64 q = 0;
    int i;
    if (!d) { if (rem) *rem = 0; return 0; }
    if (d > n) { if (rem) *rem = n; return 0; }
    if (!(d >> 32) && !(n >> 32)) { if (rem) *rem = (u32)n % (u32)d; return (u32)n / (u32)d; }
    for (i = 63; i >= 0; i--) {
        if ((n >> i) >= d) { n -= d << i; q |= 1ull << i; }
    }
    if (rem) *rem = n;
    return q;
}
u64 __udivdi3(u64 n, u64 d) { return __udivmoddi4(n, d, 0); }
u64 __umoddi3(u64 n, u64 d) { u64 r; __udivmoddi4(n, d, &r); return r; }
s64 __divdi3(s64 n, s64 d)
{
    int neg = (n < 0) != (d < 0);
    u64 q = __udivmoddi4(n < 0 ? (u64)-n : (u64)n, d < 0 ? (u64)-d : (u64)d, 0);
    return neg ? -(s64)q : (s64)q;
}
s64 __moddi3(s64 n, s64 d)
{
    u64 r;
    __udivmoddi4(n < 0 ? (u64)-n : (u64)n, d < 0 ? (u64)-d : (u64)d, &r);
    return n < 0 ? -(s64)r : (s64)r;
}

