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
#define WAVE0 (SHORT0 * 2)               /* a whole 2400 Hz wave, 1/16 samples */

void tape_rx_begin(TapeRx *r, void (*block)(void *, int, int, const u8 *, int), void *ctx)
{
    int k;
    r->block = block; r->ctx = ctx;
    r->mid = 128 * 256; r->last = 0; r->sign = 1; r->since = 0; r->prev = 0; r->n = 0;
    for (k = 0; k < 2; k++) {
        TapeEar *e = &r->ear[k];
        e->shrt = WAVE0; e->shorts = 0; e->state = 0; e->nbit = 0; e->byte = 0;
        e->n = 0; e->want = 0; e->sync = 0; e->tone = 0;
    }
    r->good = r->bad = 0; r->loud = 0; r->tone = 0; r->pitch = 0;
}

static void lost(TapeRx *r, TapeEar *e)
{
    if (e->sync == 2) r->bad++;
    e->sync = 0; e->state = 0; e->shorts = 0; e->tone = 0;
}

static void got_byte(TapeRx *r, TapeEar *e, u8 b)
{
    if (e->sync == 0) { if (b == MAGIC0) e->sync = 1; return; }
    if (e->sync == 1) {
        if (b == MAGIC1) { e->sync = 2; e->n = 0; e->want = 0; }
        else if (b != MAGIC0) e->sync = 0;
        return;
    }
    e->buf[e->n++] = b;
    if (e->n == 5) {
        int len = e->buf[3] | (e->buf[4] << 8);
        if (len > TAPE_PIECE || e->buf[0] < TAPE_HEAD || e->buf[0] > TAPE_END) { r->bad++; e->sync = 0; return; }
        e->want = 5 + len + 2;
    }
    if (e->want && e->n == e->want) {
        int len = e->want - 7;
        u16 c = (u16)(e->buf[5 + len] | (e->buf[6 + len] << 8));
        if (c == tape_crc(e->buf, 5 + len)) {
            r->good++;
            if (r->block) r->block(r->ctx, e->buf[0], e->buf[1] | (e->buf[2] << 8), e->buf + 5, len);
        } else r->bad++;
        e->sync = 0;
    }
}

static void got_bit(TapeRx *r, TapeEar *e, int one)
{
    if (e->state == 0) {                 /* between bytes: a 0 starts one  */
        if (!one) { e->state = 1; e->nbit = 0; e->byte = 0; }
        return;
    }
    if (e->nbit < 8) {
        if (one) e->byte |= (u8)(1 << e->nbit);
        e->nbit++;
        return;
    }
    e->state = 0;
    if (one) got_byte(r, e, e->byte);    /* the 1 that ends it             */
    else { if (e->sync == 2) r->bad++; e->sync = 0; }
}

/* One whole wave, d sixteenths of a sample long. */
static void wave(TapeRx *r, TapeEar *e, u32 d)
{
    u32 s = e->shrt;
    if (d < s / 2 || d > s * 3) { lost(r, e); return; }
    e->tone = 1;
    if (d < s * 3 / 2) {                 /* short: half of a 1             */
        e->shrt = (s * 15 + d + 8) / 16;
        if (++e->shorts == 2) { e->shorts = 0; got_bit(r, e, 1); }
    } else {                             /* long: a 0                      */
        e->shrt = (s * 15 + d / 2 + 8) / 16;
        e->shorts = 0;                   /* a 0 always starts on a boundary */
        got_bit(r, e, 0);
    }
    /* The tape's speed is followed, but not out of all reason. */
    if (e->shrt < WAVE0 * 3 / 4) e->shrt = WAVE0 * 3 / 4;
    if (e->shrt > WAVE0 * 4 / 3) e->shrt = WAVE0 * 4 / 3;
}

void tape_rx_samples(TapeRx *r, const u8 *in, int n)
{
    while (n--) {
        int v = (int)*in++ * 256 - r->mid, x, a, h;
        r->mid += v >> 7;                /* the middle drifts; follow it   */
        x = v >> 8;
        a = x < 0 ? -x : x;
        if (a > r->loud) r->loud = a;
        else if ((++r->n & 63) == 0 && r->loud > 0) r->loud--;   /* falls away in a third of a second */
        h = r->loud / 4 + 2;             /* this far past the middle counts */
        r->since += 16;
        if ((r->sign > 0 && x < -h) || (r->sign < 0 && x > h)) {
            /* Where between the last sample and this one it crossed. */
            int edge = r->sign > 0 ? -h : h, den = x - r->last, f = 0;
            u32 half;
            TapeEar *e = &r->ear[r->sign > 0];
            if (den) f = (edge - r->last) * 16 / den;
            if (f < 0) f = 0;
            if (f > 16) f = 16;
            half = r->since - 16 + (u32)f;
            if (r->prev) {
                wave(r, e, half + r->prev);
                if (half + r->prev) r->pitch = (u32)TAPE_RATE * 16u / (half + r->prev);
            }
            r->prev = half;
            r->since = 16 - (u32)f;
            r->sign = -r->sign;
            r->tone = r->ear[0].tone | r->ear[1].tone;
        } else if (r->since > WAVE0 * 4) {
            if (r->ear[0].tone || r->ear[0].sync) lost(r, &r->ear[0]);
            if (r->ear[1].tone || r->ear[1].sync) lost(r, &r->ear[1]);
            r->prev = 0; r->tone = 0; r->pitch = 0;
            if (r->since > 0x40000000u) r->since = WAVE0 * 4 + 16;
        }
        r->last = x;
    }
}
