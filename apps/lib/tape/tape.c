/* tape.c -- files as sound, and back.  See tape.h for the format. */
#include "tape.h"

#define MAGIC0 0xA5
#define MAGIC1 0x5A

/* Half-waves a second, as a 16.16 share of one sample. */
#define STEP_1 ((u32)(4800u * 65536u / TAPE_RATE))     /* 2400 Hz */
#define STEP_0 ((u32)(2400u * 65536u / TAPE_RATE))     /* 1200 Hz */
#define SHORT0 ((u32)(TAPE_RATE * 16u / 4800u))        /* a 2400 Hz half-wave, 1/16 samples */

u16 tape_crc(const u8 *p, int n)
{
    u16 c = 0xFFFF;
    int i;
    while (n--) {
        c ^= (u16)(*p++ << 8);
        for (i = 0; i < 8; i++) c = (u16)((c & 0x8000) ? (c << 1) ^ 0x1021 : c << 1);
    }
    return c;
}

/* ---- writing ------------------------------------------------------------- */
/* Half a sine wave, 0 to 100 and back, in 32 steps. */
static const u8 s_half[32] = {
      5,  15,  24,  34,  43,  51,  60,  67,  74,  81,  86,  91,  95,  98,  99, 100,
    100,  99,  98,  95,  91,  86,  81,  74,  67,  60,  51,  43,  34,  24,  15,   5
};

void tape_tx_begin(TapeTx *t)
{
    t->len = t->pos = 0; t->bit = 0; t->lead = 0;
    t->half = t->halves = 0; t->phase = 0; t->step = STEP_1; t->level = 1; t->busy = 0;
}

void tape_tx_block(TapeTx *t, int type, int seq, const u8 *data, int len, int lead_ms)
{
    u16 c;
    int i;
    if (len > TAPE_PIECE) len = TAPE_PIECE;
    t->blk[0] = MAGIC0; t->blk[1] = MAGIC1;
    t->blk[2] = (u8)type;
    t->blk[3] = (u8)seq; t->blk[4] = (u8)(seq >> 8);
    t->blk[5] = (u8)len; t->blk[6] = (u8)(len >> 8);
    for (i = 0; i < len; i++) t->blk[7 + i] = data[i];
    c = tape_crc(t->blk + 2, 5 + len);
    t->blk[7 + len] = (u8)c; t->blk[8 + len] = (u8)(c >> 8);
    t->len = 9 + len; t->pos = 0; t->bit = 0;
    t->lead = lead_ms * 1200 / 1000 + 8;
    t->busy = 1;
}

int tape_tx_busy(const TapeTx *t) { return t->busy; }

static int next_bit(TapeTx *t)
{
    int one;
    if (t->lead > 0) { t->lead--; one = 1; }
    else if (t->pos < t->len) {
        if (t->bit == 0)      one = 0;
        else if (t->bit == 9) one = 1;
        else                  one = (t->blk[t->pos] >> (t->bit - 1)) & 1;
        if (++t->bit == 10) { t->bit = 0; t->pos++; }
    } else { t->busy = 0; return 0; }
    t->halves = one ? 4 : 2;
    t->step = one ? STEP_1 : STEP_0;
    t->half = 0;
    return 1;
}

int tape_tx_samples(TapeTx *t, u8 *out, int max)
{
    int n = 0;
    while (n < max) {
        if (t->half >= t->halves && !next_bit(t)) break;
        out[n++] = (u8)(128 + t->level * (int)s_half[(t->phase >> 11) & 31]);
        t->phase += t->step;
        if (t->phase >= 65536u) { t->phase -= 65536u; t->level = -t->level; t->half++; }
    }
    return n;
}

/* ---- reading ------------------------------------------------------------- */
void tape_rx_begin(TapeRx *r, void (*block)(void *, int, int, const u8 *, int), void *ctx)
{
    r->block = block; r->ctx = ctx;
    r->mid = 128 * 256; r->last = 0; r->sign = 1; r->since = 0;
    r->shrt = SHORT0; r->shorts = 0; r->pend = 0;
    r->state = 0; r->nbit = 0; r->byte = 0;
    r->n = 0; r->want = 0; r->sync = 0;
    r->good = r->bad = 0; r->loud = 0; r->tone = 0;
}

static void lost(TapeRx *r)
{
    if (r->sync == 2) r->bad++;
    r->sync = 0; r->state = 0; r->shorts = 0; r->pend = 0; r->tone = 0;
}

static void got_byte(TapeRx *r, u8 b)
{
    if (r->sync == 0) { if (b == MAGIC0) r->sync = 1; return; }
    if (r->sync == 1) {
        if (b == MAGIC1) { r->sync = 2; r->n = 0; r->want = 0; }
        else if (b != MAGIC0) r->sync = 0;
        return;
    }
    r->buf[r->n++] = b;
    if (r->n == 5) {
        int len = r->buf[3] | (r->buf[4] << 8);
        if (len > TAPE_PIECE || r->buf[0] < TAPE_HEAD || r->buf[0] > TAPE_END) { r->bad++; r->sync = 0; return; }
        r->want = 5 + len + 2;
    }
    if (r->want && r->n == r->want) {
        int len = r->want - 7;
        u16 c = (u16)(r->buf[5 + len] | (r->buf[6 + len] << 8));
        if (c == tape_crc(r->buf, 5 + len)) {
            r->good++;
            if (r->block) r->block(r->ctx, r->buf[0], r->buf[1] | (r->buf[2] << 8), r->buf + 5, len);
        } else r->bad++;
        r->sync = 0;
    }
}

static void got_bit(TapeRx *r, int one)
{
    if (r->state == 0) {                 /* between bytes: a 0 starts one  */
        if (!one) { r->state = 1; r->nbit = 0; r->byte = 0; }
        return;
    }
    if (r->nbit < 8) {
        if (one) r->byte |= (u8)(1 << r->nbit);
        r->nbit++;
        return;
    }
    r->state = 0;
    if (one) got_byte(r, r->byte);       /* the 1 that ends it             */
    else { if (r->sync == 2) r->bad++; r->sync = 0; }
}

/* One half-wave, d sixteenths of a sample long. */
static void half_wave(TapeRx *r, u32 d)
{
    u32 s = r->shrt;
    if (d < s / 2 || d > s * 3) { lost(r); return; }
    r->tone = 1;
    if (d < s * 3 / 2) {                 /* short: a quarter of a 1        */
        r->shrt = (s * 15 + d + 8) / 16;
        if (r->pend) { r->pend = 0; r->shorts = 0; }   /* half a 0, and then this: out of step */
        if (++r->shorts == 4) { r->shorts = 0; got_bit(r, 1); }
    } else {                             /* long: half of a 0              */
        r->shrt = (s * 15 + d / 2 + 8) / 16;
        r->shorts = 0;                   /* a 0 always starts on a boundary */
        r->pend ^= 1;
        if (!r->pend) got_bit(r, 0);
    }
    /* The tape's speed is followed, but not out of all reason. */
    if (r->shrt < SHORT0 * 3 / 4) r->shrt = SHORT0 * 3 / 4;
    if (r->shrt > SHORT0 * 4 / 3) r->shrt = SHORT0 * 4 / 3;
}

void tape_rx_samples(TapeRx *r, const u8 *in, int n)
{
    while (n--) {
        int v = (int)*in++ * 256 - r->mid, x, a, h;
        r->mid += v >> 7;                /* the middle drifts; follow it   */
        x = v >> 8;
        a = x < 0 ? -x : x;
        if (a > r->loud) r->loud = a;
        else if ((r->since & 0xFF) == 0 && r->loud > 0) r->loud--;
        h = r->loud / 4 + 2;             /* this far past the middle counts */
        r->since += 16;
        if ((r->sign > 0 && x < -h) || (r->sign < 0 && x > h)) {
            /* Where between the last sample and this one it crossed. */
            int edge = r->sign > 0 ? -h : h, den = x - r->last, f = 0;
            if (den) f = (edge - r->last) * 16 / den;
            if (f < 0) f = 0;
            if (f > 16) f = 16;
            half_wave(r, r->since - 16 + (u32)f);
            r->since = 16 - (u32)f;
            r->sign = -r->sign;
        } else if (r->since > SHORT0 * 8) {
            if (r->tone || r->sync) lost(r);
            if (r->since > 0x40000000u) r->since = SHORT0 * 8 + 16;
        }
        r->last = x;
    }
}
