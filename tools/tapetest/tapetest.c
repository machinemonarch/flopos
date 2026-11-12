/* tapetest.c -- the cassette code (apps/lib/tape) against a pretend tape.
 *
 * Makes a file into sound, runs the sound through what a cassette does to
 * it (wrong speed, wavering speed, quiet, noisy, a drifting middle, dull,
 * upside down, a dropout), reads it back and compares.  One line a case
 * on failure; ok or FAILED at the end.
 *
 *   tapetest             the cases
 *   tapetest --wav F     read the blocks in a WAV file (8-bit mono, 22050)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../../apps/lib/tape/tape.h"

#define FILE_N 5000
static u8 s_file[FILE_N], s_back[FILE_N];
static u8 s_have[FILE_N / TAPE_PIECE + 2];
static int s_heads, s_ends, s_wrong;

static void block(void *ctx, int type, int seq, const u8 *d, int len)
{
    (void)ctx;
    if (type == TAPE_HEAD) s_heads++;
    else if (type == TAPE_END) s_ends++;
    else if (seq * TAPE_PIECE + len <= FILE_N) {
        memcpy(s_back + seq * TAPE_PIECE, d, len);
        if (memcmp(d, s_file + seq * TAPE_PIECE, len)) s_wrong++;
        s_have[seq] = 1;
    } else s_wrong++;
}

static u8 *record(int *n)
{
    static u8 buf[TAPE_RATE * 80];
    TapeTx t;
    int seq = 0, off = 0, stage = 0;
    u8 head[32] = "WRITE1.MD";
    *n = 0;
    tape_tx_begin(&t);
    for (;;) {
        if (!tape_tx_busy(&t)) {
            if (stage == 0)      { tape_tx_block(&t, TAPE_HEAD, 0, head, 20, 2000); stage = 1; }
            else if (stage == 1) { tape_tx_block(&t, TAPE_HEAD, 0, head, 20, 100); stage = 2; }
            else if (stage == 2 && off < FILE_N) {
                int len = FILE_N - off > TAPE_PIECE ? TAPE_PIECE : FILE_N - off;
                tape_tx_block(&t, TAPE_DATA, seq++, s_file + off, len, 100);
                off += len;
            }
            else if (stage == 2) { tape_tx_block(&t, TAPE_END, seq, head, 0, 100); stage = 3; }
            else if (stage == 3) { tape_tx_block(&t, TAPE_END, seq, head, 0, 300); stage = 4; }
            else break;
        }
        *n += tape_tx_samples(&t, buf + *n, 4096);
    }
    return buf;
}

typedef struct {
    const char *name;
    double speed, wow, gain, noise, dc, dull;
    int flip, drop_at_ms, drop_ms, missing_ok;
    double lop, thin;                    /* halves made unequal; the low notes thinned */
} Case;

static double rnd(void) { return (rand() / (double)RAND_MAX) * 2 - 1; }

static int run(const Case *c, const u8 *src, int n)
{
    static u8 out[TAPE_RATE * 100];
    TapeRx r;
    double pos = 0, lp = 0, drift = 0, hp = 0;
    int m = 0, i, missing = 0, blocks = (FILE_N + TAPE_PIECE - 1) / TAPE_PIECE;
    while (pos < n - 2 && m < (int)sizeof out) {
        int k = (int)pos;
        double f = pos - k, v = (src[k] * (1 - f) + src[k + 1] * f) - 128;
        double ms = m * 1000.0 / TAPE_RATE;
        if (c->flip) v = -v;
        v *= c->gain;
        if (c->drop_ms && ms >= c->drop_at_ms && ms < c->drop_at_ms + c->drop_ms) v *= 0.02;
        if (c->dull > 0) { lp += (v - lp) * c->dull; v = lp; }
        if (c->lop > 0) {                /* a recorder's hot, lopsided output: shifted, then flattened */
            v = v * 6 + c->lop * 600;
            if (v > 100) v = 100;
            if (v < -100) v = -100;
        }
        if (c->thin > 0) { hp += (v - hp) * c->thin; v -= hp; }   /* and no low notes, as through a small capacitor */
        drift = c->dc * sin(ms / 1000.0 * 2 * M_PI * 0.7);
        v += drift + rnd() * c->noise;
        if (v > 127) v = 127;
        if (v < -128) v = -128;
        out[m++] = (u8)(int)(v + 128.5);
        pos += c->speed * (1 + c->wow * sin(ms / 1000.0 * 2 * M_PI * 4));
    }
    memset(s_have, 0, sizeof s_have); memset(s_back, 0, sizeof s_back);
    s_heads = s_ends = s_wrong = 0;
    tape_rx_begin(&r, block, 0);
    for (i = 0; i < m; i += 777) tape_rx_samples(&r, out + i, m - i < 777 ? m - i : 777);
    for (i = 0; i < blocks; i++) missing += !s_have[i];
    if (s_wrong || missing > c->missing_ok || (!c->missing_ok && (!s_heads || !s_ends)) ||
        (c->drop_ms && !missing)) {
        printf("%s: %d of %d pieces missing, %d wrong, %d heads, %d ends, %u failed checks\n",
               c->name, missing, blocks, s_wrong, s_heads, s_ends, r.bad);
        return 0;
    }
    return 1;
}

static int wav(const char *path)
{
    FILE *f = fopen(path, "rb");
    static u8 buf[4096];
    TapeRx r;
    int n;
    if (!f) return 1;
    fseek(f, 44, SEEK_SET);
    memset(s_have, 0, sizeof s_have);
    tape_rx_begin(&r, 0, 0);
    while ((n = (int)fread(buf, 1, sizeof buf, f)) > 0) tape_rx_samples(&r, buf, n);
    printf("%u blocks read, %u failed\n", r.good, r.bad);
    return r.good ? 0 : 1;
}

int main(int argc, char **argv)
{
    static const Case cases[] = {
        /* name                speed  wow    gain  noise dc  dull flip drop        */
        { "clean",             1.00,  0,     1.0,  0,    0,  0,   0,   0, 0, 0 },
        { "6% fast",           1.06,  0,     1.0,  0,    0,  0,   0,   0, 0, 0 },
        { "6% slow",           0.94,  0,     1.0,  0,    0,  0,   0,   0, 0, 0 },
        { "wavering speed",    1.02,  0.01,  1.0,  0,    0,  0,   0,   0, 0, 0 },
        { "quiet",             1.00,  0,     0.08, 0,    0,  0,   0,   0, 0, 0 },
        { "loud, clipped",     1.00,  0,     3.0,  0,    0,  0,   0,   0, 0, 0 },
        { "noisy",             1.00,  0,     0.5,  8,    0,  0,   0,   0, 0, 0 },
        { "drifting middle",   1.00,  0,     0.5,  0,    25, 0,   0,   0, 0, 0 },
        { "dull",              1.00,  0,     1.0,  0,    0,  0.3, 0,   0, 0, 0 },
        { "upside down",       1.00,  0,     1.0,  0,    0,  0,   1,   0, 0, 0 },
        { "a worn tape",       0.97,  0.006, 0.3,  4,    10, 0.4, 1,   0, 0, 0 },
        { "lopsided",          1.00,  0,     1.0,  0,    0,  0,   0,   0, 0, 0, 0.35, 0 },
        { "more lopsided",     1.00,  0,     1.0,  0,    0,  0,   1,   0, 0, 0, 0.55, 0 },
        { "lopsided and thin", 0.97,  0.006, 1.0,  2,    0,  0.5, 0,   0, 0, 0, 0.45, 0.2 },
        { "a dropout",         1.00,  0,     1.0,  1,    0,  0,   0,   9000, 300, 2 },
    };
    int n, i, ok = 1;
    u8 *snd;
    if (argc > 2 && !strcmp(argv[1], "--wav")) return wav(argv[2]);
    srand(430);
    for (i = 0; i < FILE_N; i++) s_file[i] = (u8)rand();
    snd = record(&n);
    for (i = 0; i < (int)(sizeof cases / sizeof cases[0]); i++) ok &= run(&cases[i], snd, n);
    {   /* Hiss alone must never pass for a block. */
        static u8 hiss[TAPE_RATE * 30];
        TapeRx r;
        for (i = 0; i < (int)sizeof hiss; i++) hiss[i] = (u8)(128 + rnd() * 60);
        tape_rx_begin(&r, 0, 0);
        tape_rx_samples(&r, hiss, sizeof hiss);
        if (r.good) { printf("hiss: %u blocks out of nothing\n", r.good); ok = 0; }
    }
    printf("%s (%d s of tape for %d bytes)\n", ok ? "ok" : "FAILED", n / TAPE_RATE, FILE_N);
    return !ok;
}
