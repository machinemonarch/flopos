/* fuzz.c -- feed FLOPOS's own readers garbage, on this PC.
 *
 * The code that reads what comes from outside (songs, certificates, web
 * pages, network packets) is built here for the PC with the compiler's
 * memory checker switched on, and given real inputs that have been
 * damaged at random: bits flipped, sizes changed, pieces cut out, moved
 * and repeated.  A reader that steps outside its memory, or never
 * finishes, is caught, and the input that did it is kept.
 *
 *   fuzz SEEDS RUNS [SEED] [OUT]     many damaged inputs
 *   fuzz --one FILE [MODE]           one input exactly, to look at a find
 *
 * Each target (audio.c, tracker.c, ...) is linked with this file and
 * gives fuzz_one().  test/fuzz/run.py builds and runs them all.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <dirent.h>

typedef unsigned char u8;
typedef unsigned int u32;

/* The target: one input, tried in the way MODE picks (a target has a few
 * ways in; any number is fine, it takes it modulo what it has). */
extern void fuzz_one(const u8 *data, u32 len, u32 mode);
extern const char *fuzz_name;
extern const u32 fuzz_max;               /* longest input worth giving      */

#define SEEDS 256
static u8 *s_seed[SEEDS];
static u32 s_seed_len[SEEDS];
static int s_nseed;

static u8 *s_cur;                        /* the input being tried           */
static u32 s_cur_len, s_cur_mode, s_run;
static const char *s_out = ".";

static u32 s_x = 2463534242u;
static u32 rnd(void) { s_x ^= s_x << 13; s_x ^= s_x >> 17; s_x ^= s_x << 5; return s_x; }
static u32 upto(u32 n) { return n ? rnd() % n : 0; }

/* Keep the input that broke something, so it can be looked at again. */
static void keep(const char *why)
{
    char name[512];
    FILE *f;
    snprintf(name, sizeof name, "%s/%s-%s-%u-mode%u.bin", s_out, fuzz_name, why, s_run, s_cur_mode);
    if ((f = fopen(name, "wb"))) { fwrite(s_cur, 1, s_cur_len, f); fclose(f); }
    fprintf(stderr, "kept %s\n", name);
}
void __asan_on_error(void) { keep("crash"); }
static void on_alarm(int sig) { (void)sig; keep("hang"); _exit(3); }

static void load_seeds(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    if (!d) { fprintf(stderr, "no seeds in %s\n", dir); exit(2); }
    while ((e = readdir(d)) && s_nseed < SEEDS) {
        char path[1024];
        FILE *f;
        u8 *b;
        u32 n;
        if (e->d_name[0] == '.') continue;
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        if (!(f = fopen(path, "rb"))) continue;
        b = malloc(fuzz_max);
        n = (u32)fread(b, 1, fuzz_max, f);
        fclose(f);
        if (n < 4) { free(b); continue; }
        s_seed[s_nseed] = b; s_seed_len[s_nseed++] = n;
    }
    closedir(d);
    if (!s_nseed) { fprintf(stderr, "no seeds in %s\n", dir); exit(2); }
}

/* Where to damage: half the time near the start, where the headers are. */
static u32 spot(u32 len) { return (rnd() & 1) ? upto(len < 2048 ? len : 2048) : upto(len); }

static u32 damage(u8 *b, u32 len)
{
    static const u32 odd[] = { 0, 1, 0x7F, 0x80, 0xFF, 0x100, 0x7FFF, 0x8000, 0xFFFF,
                               0x10000, 0x7FFFFFFF, 0x80000000u, 0xFFFFFFFFu, 0xFFFFFFFEu };
    int k, n = 1 + (int)upto(8);
    for (k = 0; k < n && len >= 8; k++) {
        u32 at = spot(len), v, w, i;
        switch (upto(10)) {
        case 0: b[at] ^= (u8)(1u << upto(8)); break;
        case 1: b[at] = (u8)rnd(); break;
        case 2: b[at] = (u8)odd[upto(5)]; break;
        case 3:                                     /* a number, either way round */
            v = odd[upto(sizeof odd / sizeof odd[0])]; w = 1u << upto(3);
            for (i = 0; i < w && at + i < len; i++)
                b[at + i] = (u8)(v >> 8 * ((rnd() & 1) ? i : w - 1 - i));
            break;
        case 4: b[at] = (u8)(b[at] + 1 - 2 * (rnd() & 1)); break;
        case 5:                                     /* cut a piece out            */
            w = 1 + upto(len / 4);
            if (at + w < len) { memmove(b + at, b + at + w, len - at - w); len -= w; }
            break;
        case 6:                                     /* repeat a piece             */
            w = 1 + upto(256);
            if (at + 2 * w < len) memmove(b + at + w, b + at, w);
            break;
        case 7: if (upto(4) == 0) len = at + 1; break;            /* stop short   */
        case 8: {                                   /* a piece of another input   */
            int s = (int)upto((u32)s_nseed);
            u32 from = spot(s_seed_len[s]);
            w = 1 + upto(512);
            if (from + w <= s_seed_len[s] && at + w <= len) memcpy(b + at, s_seed[s] + from, w);
            break;
        }
        default: for (i = 0, w = 1 + upto(16); i < w && at + i < len; i++) b[at + i] = (u8)rnd();
        }
    }
    return len;
}

/* A target may have a job of its own (tls: record a real conversation). */
extern int fuzz_extra(int argc, char **argv) __attribute__((weak));

int main(int argc, char **argv)
{
    u32 runs, r;
    signal(SIGALRM, on_alarm);
    if (fuzz_extra && argc >= 2 && !strcmp(argv[1], "--record")) return fuzz_extra(argc, argv);

    if (argc >= 3 && !strcmp(argv[1], "--one")) {
        FILE *f = fopen(argv[2], "rb");
        if (!f) return 2;
        s_cur = malloc(fuzz_max);
        s_cur_len = (u32)fread(s_cur, 1, fuzz_max, f);
        fclose(f);
        s_cur_mode = argc > 3 ? (u32)atoi(argv[3]) : 0;
        s_out = "/dev";                              /* nothing to keep: it is the find */
        alarm(20);
        fuzz_one(s_cur, s_cur_len, s_cur_mode);
        printf("ok\n");
        return 0;
    }
    if (argc < 3) { fprintf(stderr, "fuzz SEEDS RUNS [SEED] [OUT]   or   fuzz --one FILE [MODE]\n"); return 2; }
    load_seeds(argv[1]);
    runs = (u32)atoi(argv[2]);
    if (argc > 3 && atoi(argv[3])) s_x = (u32)atoi(argv[3]) * 2654435761u + 1;
    if (argc > 4) s_out = argv[4];
    s_cur = malloc(fuzz_max);

    for (r = 0; r < runs; r++) {
        int s = (int)upto((u32)s_nseed);
        s_run = r;
        memcpy(s_cur, s_seed[s], s_seed_len[s]);
        s_cur_len = r < (u32)s_nseed * 8 && (r & 7) == 0 ? s_seed_len[s]      /* some as they are */
                                                         : damage(s_cur, s_seed_len[s]);
        s_cur_mode = rnd();
        alarm(20);
        fuzz_one(s_cur, s_cur_len, s_cur_mode);
        alarm(0);
    }
    printf("ok (%s: %u inputs from %d seeds)\n", fuzz_name, runs, s_nseed);
    return 0;
}
