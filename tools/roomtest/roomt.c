/* roomt.c -- an app that drives the script room, to see it work and see
 * its walls hold.  Never put on a floppy: run.py builds it and copies it
 * onto a scratch image (with the real ROOM.BIN, or rogueroom.bin in its
 * place).
 *
 *   roomt 0      the real room: a script runs and answers
 *   roomt 1      the real room: a script that never ends is stopped
 *   roomt 2      the real room: a script that fills its memory is stopped
 *   roomt N      (N >= 10) the rogue room: task N, see rogueroom.c
 *
 * It writes what happened down the serial wire: "[roomt] answer 0 ..."
 * or "[roomt] stopped: why", then "[roomt] still here" with a check that
 * its own memory was not touched.
 */
#include "app.h"
#include "../../room/room.h"

static const Api *A;
static u8 s_door[4096], s_ans[512];
static u32 s_guard = 0x5AFEC0DE;

static void say(const char *a, const char *b)
{
    char line[600];
    A->format(line, sizeof line, "[roomt] %s%s\n", a, b);
    A->wire(line);
}

static int door(u8 *buf, u32 len, u32 cap)
{
    /* "ping" gets "pong"; anything else gets its own length back */
    if (len == 4 && buf[0] == 'p' && buf[1] == 'i' && buf[2] == 'n' && buf[3] == 'g') {
        buf[0] = 'p'; buf[1] = 'o'; buf[2] = 'n'; buf[3] = 'g';
        return 4;
    }
    (void)cap;
    return 0;
}

static int task(const u8 *t, u32 n, u32 ms)
{
    int r = A->room_task(t, n, s_ans, sizeof s_ans - 1, ms, door, s_door, sizeof s_door);
    if (r < 0) { say("stopped: ", A->room_why()); return -1; }
    s_ans[r] = 0;
    {
        char line[480];
        A->format(line, sizeof line, "answer %u %s", (u32)s_ans[0], r > 6 ? (const char *)s_ans + 6 : "");
        say(line, "");
    }
    return r;
}

static u32 put_run(u8 *t, u32 ms, const char *src)
{
    u32 n = 0, i;
    t[n++] = T_RUN;
    t[n++] = (u8)ms; t[n++] = (u8)(ms >> 8); t[n++] = (u8)(ms >> 16); t[n++] = (u8)(ms >> 24);
    t[n++] = 0; t[n++] = 0; t[n++] = 0; t[n++] = 0;          /* no element */
    t[n++] = 't'; t[n++] = 0;
    for (i = 0; src[i]; i++) t[n++] = (u8)src[i];
    return n;
}

int app_main(const Api *api, int argc, char **argv)
{
    static u8 t[2048];
    int n = 0, i;
    A = api;
    for (i = 0; argc > 1 && argv[1][i]; i++) n = n * 10 + (argv[1][i] - '0');
    if (!A->room_open()) { say("no room: ", A->room_why()); return 0; }
    if (n < 10) {
        t[0] = T_INIT;
        for (i = 1; i < 9; i++) t[i] = 0;
        if (task(t, 9, 1000) < 0) return 0;
        if (n == 0) task(t, put_run(t, 2000, "var a = [1,2,3].map(function (x) { return x * 2; }); if (a.join() !== '2,4,6') throw new Error('wrong');"), 2000);
        if (n == 1) task(t, put_run(t, 500, "while (true) {}"), 500);
        if (n == 2) task(t, put_run(t, 20000, "var k = []; while (true) k.push('x'.repeat(1000) + k.length);"), 20000);
    } else {
        t[0] = (u8)n;
        task(t, 1, 1000);
    }
    say(s_guard == 0x5AFEC0DE ? "still here" : "still here, but its memory was changed", "");
    return 0;
}
