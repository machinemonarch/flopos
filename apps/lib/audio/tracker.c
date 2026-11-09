/* ===========================================================================
 * tracker.c -- playing .mod and .xm songs.
 *
 * A tracker song is its own instruments (short recordings, "samples") and
 * a score that says, row by row and channel by channel, which sample to
 * play at which pitch and loudness, with effects -- slides, vibrato,
 * volume fades -- worked out a "tick" at a time, several ticks to a row.
 * Playing one means doing that, and mixing every channel's sample at its
 * pitch into one stream for the Sound Blaster.  It is light work: a few
 * thousand multiply-adds per tick.
 *
 * Two formats:
 *   MOD  ProTracker's, from the Amiga.  4 channels (or more, in the PC
 *        variants), 31 samples, pitch as Amiga periods.
 *   XM   FastTracker 2's.  Up to 32 channels, instruments that choose a
 *        sample by note and shape its loudness with an envelope, a
 *        volume column beside the effect, pitch on a linear scale.
 *
 * Mono, because the Sound Blaster here plays mono; panning is read and
 * ignored.  The song plays once through and stops.
 *
 * Part of the audio library (apps/lib/audio), which any app can build in:
 * it only turns a song into samples, and leaves playing them to the app
 * (through the app interface's audio_* calls).
 * ===========================================================================
 */
#include "app.h"
#include "tracker.h"

typedef signed char s8;
typedef short s16;

#define CHANS     32
#define SAMPLES   256
#define INSTRS    128

typedef struct {
    s8  *data;
    u32  len, lstart, llen;
    u8   loop;                           /* 0 none, 1 forward, 2 ping-pong  */
    s8   finetune;                       /* XM: -128..127; MOD: -8..7 x 16  */
    u8   vol;
    s8   relnote;
} Sample;

typedef struct {
    u8      keymap[96];
    Sample *smp[16];
    int     nsmp;
    u16     env[12][2];                  /* tick, value 0..64               */
    int     envn, envsus, envls, envle;
    u8      envtype;                     /* 1 on, 2 sustain, 4 loop         */
    u16     fadeout;
} Instr;

typedef struct { u8 note, ins, vol, eff, par; } Note;

typedef struct {
    Sample *s;
    Instr  *in;
    u32     pos, frac, step;             /* position, 16-bit fraction       */
    int     dir, on;
    int     period, target, base_vol, vol, note;
    int     portaspd, porta_up, porta_dn, fporta_up, fporta_dn, vslide, xfporta_up, xfporta_dn;
    int     vibpos, vibspd, vibdep, trempos, tremspd, tremdep;
    int     keyoff, fade, envtick, envval;
    int     retrig, cut_at, delay_at, ploop_row, ploop_n, offset;
    int     arp;                         /* period added this tick          */
    int     vib;                         /* period added by vibrato         */
    int     tvol;                        /* volume added by tremolo         */
    Note    delayed;
    int     tremor_on, tremor_off, tremor_n;
    int     mretrig_par, gvslide;
} Chan;

static struct {
    int     xm, linear, nch, nord, restart, npat, ninst;
    u8      order[256];
    Note   *pat[256];
    int     rows[256];
    Sample  smp[SAMPLES];
    Instr   ins[INSTRS];
    int     nsmp;
    Chan    ch[CHANS];
    int     speed, bpm, tick, row, ord, gvol;
    int     next_row, next_ord, jump, pdelay, pdelay_n;
    int     rate, tick_len, tick_left;
    int     playing, ended;
    char    title[24];
    u32     played;                      /* samples out                     */
} T;

static Note s_notes[110000];             /* every row of every pattern     */

/* ---- reading ---------------------------------------------------------------- */
static u32 le16(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8); }
static u32 le32(const u8 *p) { return le16(p) | (le16(p + 2) << 16); }
static u32 be16(const u8 *p) { return ((u32)p[0] << 8) | p[1]; }

/* Amiga periods for finetune 0, C-1 to B-3, which is what ProTracker
 * writes; a note is found by its nearest period here. */
static const u16 s_amiga[36] = {
    856,808,762,720,678,640,604,570,538,508,480,453,
    428,404,381,360,339,320,302,285,269,254,240,226,
    214,202,190,180,170,160,151,143,135,127,120,113,
};

/* 2^(i/768) * 65536, for i 0..767: one octave in the steps XM uses. */
static u32 s_pow2[768];

static void tables(void)
{
    double x = 1.0, k = 1.000902853937;  /* 2^(1/768)                       */
    int i;
    if (s_pow2[0]) return;
    for (i = 0; i < 768; i++) { s_pow2[i] = (u32)(x * 65536.0 + 0.5); x *= k; }
}

static int load_mod(const u8 *d, u32 n)
{
    int i, maxpat = 0, ch = 0;
    u32 at;
    const u8 *sig = d + 1080;
    Note *np = s_notes;

    if (n < 1084) return 0;
    if (!((sig[0] == 'M' && sig[2] == 'K') || (sig[0] == 'F' && sig[1] == 'L' && sig[2] == 'T') ||
          (sig[1] == 'C' && sig[2] == 'H' && sig[3] == 'N') || (sig[2] == 'C' && sig[3] == 'H'))) return 0;
    if (sig[0] == 'M' || sig[0] == 'F') ch = sig[0] == 'F' ? sig[3] - '0' : 4;
    else if (sig[1] == 'C') ch = sig[0] - '0';
    else ch = (sig[0] - '0') * 10 + (sig[1] - '0');
    if (ch < 1 || ch > CHANS) return 0;

    T.xm = 0; T.linear = 0; T.nch = ch;
    for (i = 0; i < 20 && d[i]; i++) T.title[i] = (char)(d[i] >= 32 && d[i] < 127 ? d[i] : ' ');
    T.title[i] = 0;
    T.nord = d[950]; T.restart = d[951] < 128 ? d[951] : 0;
    if (T.nord < 1 || T.nord > 128) return 0;
    for (i = 0; i < 128; i++) { T.order[i] = d[952 + i]; if (d[952 + i] > maxpat) maxpat = d[952 + i]; }
    T.npat = maxpat + 1;
    at = 1084;
    if (at + (u32)T.npat * 64 * ch * 4 > n) return 0;
    if ((u32)T.npat * 64 * ch > sizeof s_notes / sizeof s_notes[0]) return 0;
    for (i = 0; i < T.npat; i++) {
        int r, c;
        T.pat[i] = np; T.rows[i] = 64;
        for (r = 0; r < 64; r++)
            for (c = 0; c < ch; c++, at += 4, np++) {
                const u8 *b = d + at;
                int period = ((b[0] & 0x0F) << 8) | b[1], k, best = 0, bd = 99999;
                np->ins = (u8)((b[0] & 0xF0) | (b[2] >> 4));
                np->eff = (u8)(b[2] & 0x0F);
                np->par = b[3];
                np->vol = 0;
                np->note = 0;
                if (period) {
                    for (k = 0; k < 36; k++) {
                        int dd = period - s_amiga[k];
                        if (dd < 0) dd = -dd;
                        if (dd < bd) { bd = dd; best = k; }
                    }
                    np->note = (u8)(best + 1 + 36);   /* C-1 as XM's C-4  */
                }
            }
    }
    /* Samples follow the patterns, in order. */
    T.nsmp = 31; T.ninst = 31;
    for (i = 0; i < 31; i++) {
        const u8 *h = d + 20 + i * 30;
        Sample *s = &T.smp[i];
        Instr *in = &T.ins[i];
        int k;
        s->len = be16(h + 22) * 2;
        s->finetune = (s8)(((h[24] & 0x0F) ^ 8) - 8) * 16;
        s->vol = h[25] > 64 ? 64 : h[25];
        s->lstart = be16(h + 26) * 2;
        s->llen = be16(h + 28) * 2;
        s->relnote = 0;
        if (at + s->len > n) s->len = at < n ? n - at : 0;
        s->data = (s8 *)(d + at);
        at += s->len;
        if (s->lstart + s->llen > s->len) s->llen = s->lstart < s->len ? s->len - s->lstart : 0;
        s->loop = s->llen > 2 ? 1 : 0;
        for (k = 0; k < 96; k++) in->keymap[k] = 0;
        in->smp[0] = s; in->nsmp = 1; in->envtype = 0; in->fadeout = 0;
    }
    T.speed = 6; T.bpm = 125;
    return 1;
}

static int load_xm(u8 *d, u32 n)
{
    u32 at, hs;
    int i, p, flags;
    Note *np = s_notes;
    u32 room = sizeof s_notes / sizeof s_notes[0], used = 0;

    if (n < 80) return 0;
    for (i = 0; i < 17; i++) if (d[i] != "Extended Module: "[i]) return 0;
    for (i = 0; i < 20 && d[17 + i]; i++) T.title[i] = (char)(d[17 + i] >= 32 && d[17 + i] < 127 ? d[17 + i] : ' ');
    T.title[i] = 0;
    hs = le32(d + 60);
    T.xm = 1;
    T.nord = (int)le16(d + 64);
    T.restart = (int)le16(d + 66);
    T.nch = (int)le16(d + 68);
    T.npat = (int)le16(d + 70);
    T.ninst = (int)le16(d + 72);
    flags = (int)le16(d + 74);
    T.linear = flags & 1;
    T.speed = (int)le16(d + 76);
    T.bpm = (int)le16(d + 78);
    if (T.nch < 1 || T.nch > CHANS || T.npat > 256 || T.ninst > INSTRS || T.nord > 256 || T.nord < 1) return 0;
    for (i = 0; i < 256; i++) T.order[i] = d[80 + i];
    if (T.restart >= T.nord) T.restart = 0;
    at = 60 + hs;

    for (p = 0; p < T.npat; p++) {
        u32 ph, plen, end;
        int rows, r, c;
        if (at + 9 > n) return 0;
        ph = le32(d + at);
        rows = (int)le16(d + at + 5);
        plen = le16(d + at + 7);
        at += ph;
        if (rows < 1 || rows > 256) rows = 64;
        if (used + (u32)rows * (u32)T.nch > room) return 0;
        T.pat[p] = np; T.rows[p] = rows;
        end = at + plen;
        if (end > n) return 0;
        for (r = 0; r < rows; r++)
            for (c = 0; c < T.nch; c++, np++) {
                u8 b = 0;
                np->note = np->ins = np->vol = np->eff = np->par = 0;
                if (!plen) continue;               /* an empty pattern       */
                if (at >= end) continue;
                b = d[at];
                if (b & 0x80) {
                    at++;
                    if (b & 1  && at < end) np->note = d[at++];
                    if (b & 2  && at < end) np->ins  = d[at++];
                    if (b & 4  && at < end) np->vol  = d[at++];
                    if (b & 8  && at < end) np->eff  = d[at++];
                    if (b & 16 && at < end) np->par  = d[at++];
                } else if (at + 5 <= end) {
                    np->note = d[at]; np->ins = d[at + 1]; np->vol = d[at + 2];
                    np->eff = d[at + 3]; np->par = d[at + 4];
                    at += 5;
                }
            }
        used += (u32)rows * (u32)T.nch;
        at = end;
    }
    for (p = T.npat; p < 256; p++) { T.pat[p] = 0; T.rows[p] = 64; }

    T.nsmp = 0;
    for (i = 0; i < T.ninst; i++) {
        Instr *in = &T.ins[i];
        u32 ih, sh = 40;
        int ns, k, first;
        if (at + 29 > n) return 0;
        ih = le32(d + at);
        ns = (int)le16(d + at + 27);
        in->nsmp = 0; in->envtype = 0; in->fadeout = 0; in->envn = 0;
        for (k = 0; k < 96; k++) in->keymap[k] = 0;
        if (ns > 16) ns = 16;
        if (ns > 0) {
            const u8 *h = d + at;
            if (at + 243 > n) return 0;
            sh = le32(h + 29);
            for (k = 0; k < 96; k++) in->keymap[k] = h[33 + k] < 16 ? h[33 + k] : 0;
            for (k = 0; k < 12; k++) { in->env[k][0] = (u16)le16(h + 129 + 4 * k); in->env[k][1] = (u16)le16(h + 131 + 4 * k); }
            in->envn = h[225] > 12 ? 12 : h[225];
            in->envsus = h[227]; in->envls = h[228]; in->envle = h[229];
            in->envtype = h[233];
            in->fadeout = (u16)le16(h + 239);
        }
        at += ih;
        first = T.nsmp;
        for (k = 0; k < ns; k++) {
            Sample *s;
            const u8 *h = d + at + (u32)k * sh;
            if (T.nsmp >= SAMPLES || at + (u32)(k + 1) * sh > n) return 0;
            s = &T.smp[T.nsmp++];
            s->len = le32(h); s->lstart = le32(h + 4); s->llen = le32(h + 8);
            s->vol = h[12] > 64 ? 64 : h[12];
            s->finetune = (s8)h[13];
            s->loop = h[14] & 3; if (s->loop == 3) s->loop = 1;
            s->relnote = (s8)h[16];
            s->data = (s8 *)(h[14] & 16 ? (void *)1 : (void *)0);   /* 16-bit, noted */
            in->smp[in->nsmp++] = s;
        }
        at += (u32)ns * sh;
        /* The sample data: deltas, 8 or 16 bits.  16-bit ones are made
         * 8-bit in place -- the speaker here plays 8 bits anyway. */
        for (k = first; k < T.nsmp; k++) {
            Sample *s = &T.smp[k];
            int bits16 = s->data != 0;
            u32 j, len = s->len;
            if (at + len > n) len = at < n ? n - at : 0;
            if (bits16) {
                s16 v = 0;
                s8 *o = (s8 *)(d + at);
                for (j = 0; j + 1 < len; j += 2) {
                    v = (s16)(v + (s16)le16(d + at + j));
                    o[j / 2] = (s8)(v >> 8);
                }
                s->data = o;
                s->len = len / 2; s->lstart /= 2; s->llen /= 2;
            } else {
                s8 v = 0, *o = (s8 *)(d + at);
                for (j = 0; j < len; j++) { v = (s8)(v + (s8)d[at + j]); o[j] = v; }
                s->data = o;
                s->len = len;
            }
            at += len;
            if (s->lstart > s->len) s->lstart = s->len;
            if (s->lstart + s->llen > s->len) s->llen = s->len - s->lstart;
            if (s->llen < 2) s->loop = 0;
        }
    }
    if (T.speed < 1) T.speed = 6;
    if (T.bpm < 32) T.bpm = 125;
    return 1;
}

/* ---- pitch ------------------------------------------------------------------ */
/* 2^(e/768), for any e: e is in 768ths of an octave. */
static double pow2_768(int e)
{
    int oct = e >= 0 ? e / 768 : -((-e + 767) / 768);
    double v = (double)s_pow2[e - oct * 768] / 65536.0;
    while (oct > 0) { v *= 2.0; oct--; }
    while (oct < 0) { v *= 0.5; oct++; }
    return v;
}

/* For MOD, period is an Amiga period.  For XM, a linear "period":
 * 7680 at C-0, 64 to a semitone. */
static int note_period(int note, int finetune)
{
    if (!T.xm) {
        /* note 37..72 is C-1..B-3; finetune is eighths of a semitone, x16
         * (so 128 to a semitone, as XM has it). */
        int k = note - 37;
        if (k < 0) k = 0;
        if (k > 35) k = 35;
        return (int)((double)s_amiga[k] * pow2_768(-finetune / 2) + 0.5);
    }
    return 7680 - (note - 1) * 64 - finetune / 2;
}

/* How far through a sample to move per output sample, 16.16. */
static u32 period_step(int period)
{
    double hz;
    if (period < 1) period = 1;
    if (!T.xm) hz = 3546895.0 / (double)period;
    else       hz = 8363.0 * pow2_768(4608 - period);
    return (u32)(hz * 65536.0 / (double)T.rate);
}

/* ---- the song moving --------------------------------------------------------- */
static const s16 s_sine[32] = { 0,24,49,74,97,120,141,161,180,197,212,224,235,244,250,253,
                                255,253,250,244,235,224,212,197,180,161,141,120,97,74,49,24 };

static int wave(int pos)
{
    int v = s_sine[pos & 31];
    return (pos & 32) ? -v : v;
}

static void trigger(Chan *c, int note, int ins)
{
    Instr *in;
    Sample *s;
    if (ins) {
        if (ins > T.ninst) { c->on = 0; return; }
        c->in = &T.ins[ins - 1];
    }
    in = c->in;
    if (!in || note < 1 || note > 96) return;
    s = in->smp[in->keymap[note - 1] < in->nsmp ? in->keymap[note - 1] : 0];
    if (!s || !in->nsmp || !s->len || !s->data) { c->on = 0; return; }
    c->s = s;
    c->note = note + (T.xm ? s->relnote : 0);
    if (c->note < 1) c->note = 1;
    if (c->note > 119) c->note = 119;
    c->period = note_period(c->note, s->finetune);
    c->pos = c->offset < 0 ? 0 : (u32)c->offset;
    if (c->pos >= s->len) c->pos = s->len ? s->len - 1 : 0;
    c->offset = 0;
    c->frac = 0; c->dir = 1; c->on = 1;
    c->keyoff = 0; c->fade = 65536; c->envtick = 0;
    c->vibpos = 0; c->trempos = 0;
}

static void vslide(Chan *c, int par)
{
    int up = par >> 4, dn = par & 15;
    if (up) c->vol += up; else c->vol -= dn;
    if (c->vol > 64) c->vol = 64;
    if (c->vol < 0) c->vol = 0;
}

static void porta_to(Chan *c)
{
    if (!c->target) return;
    if (c->period < c->target) { c->period += c->portaspd * (T.xm ? 4 : 1); if (c->period > c->target) c->period = c->target; }
    else if (c->period > c->target) { c->period -= c->portaspd * (T.xm ? 4 : 1); if (c->period < c->target) c->period = c->target; }
}

static int pmul(void) { return T.xm ? 4 : 1; }

static void envelope(Chan *c)
{
    Instr *in = c->in;
    int k, t;
    if (!in || !(in->envtype & 1) || in->envn < 1) { c->envval = 64; return; }
    t = c->envtick;
    for (k = 0; k + 1 < in->envn && t >= in->env[k + 1][0]; k++) { }
    if (k + 1 >= in->envn) c->envval = in->env[in->envn - 1][1];
    else {
        int t0 = in->env[k][0], t1 = in->env[k + 1][0], v0 = in->env[k][1], v1 = in->env[k + 1][1];
        c->envval = t1 > t0 ? v0 + (v1 - v0) * (t - t0) / (t1 - t0) : v0;
    }
    if (c->envval > 64) c->envval = 64;
    /* Move on, unless held at the sustain point until the key is let go. */
    if ((in->envtype & 2) && !c->keyoff && in->envsus < in->envn && t == in->env[in->envsus][0]) return;
    c->envtick++;
    if ((in->envtype & 4) && in->envle < in->envn && in->envls < in->envn &&
        c->envtick > in->env[in->envle][0])
        c->envtick = in->env[in->envls][0];
}

static void row_start(void)
{
    Note *row;
    int i, pat = T.order[T.ord];

    if (pat >= T.npat || !T.pat[pat]) { T.ended = 1; return; }
    row = T.pat[pat] + T.row * T.nch;
    for (i = 0; i < T.nch; i++) {
        Chan *c = &T.ch[i];
        Note *n = &row[i];
        int eff = n->eff, par = n->par, porta = (eff == 3 || eff == 5 || (n->vol >= 0xF0));
        c->arp = 0; c->vib = 0; c->tvol = 0;
        c->cut_at = -1; c->delay_at = -1; c->retrig = 0;

        if (eff == 0x0E && (par >> 4) == 0x0D && (par & 15)) {   /* note delay */
            c->delay_at = par & 15;
            c->delayed = *n;
            continue;
        }
        if (eff == 9) { if (par) c->offset = par * 256; }
        if (n->ins && n->ins <= T.ninst && !(T.xm && n->note == 97)) {
            Instr *in = &T.ins[n->ins - 1];
            Sample *s = in->nsmp ? in->smp[0] : 0;
            if (T.xm && n->note && n->note < 97 && in->nsmp) s = in->smp[in->keymap[n->note - 1] < in->nsmp ? in->keymap[n->note - 1] : 0];
            if (s) c->vol = c->base_vol = s->vol;
            if (!n->note || porta) { c->in = in; c->keyoff = 0; c->fade = 65536; c->envtick = 0; }
        }
        if (n->note == 97) { c->keyoff = 1; if (!c->in || !(c->in->envtype & 1)) c->vol = 0; }
        else if (n->note) {
            if (porta && c->on && c->s) {
                Instr *in = n->ins ? &T.ins[n->ins - 1] : c->in;
                Sample *s = in && in->nsmp ? in->smp[in->keymap[n->note - 1] < in->nsmp ? in->keymap[n->note - 1] : 0] : c->s;
                c->target = note_period(n->note + (T.xm && s ? s->relnote : 0), s ? s->finetune : 0);
            } else {
                trigger(c, n->note, n->ins);
                c->target = 0;
            }
        }

        /* The volume column (XM). */
        if (n->vol >= 0x10 && n->vol <= 0x50) c->vol = n->vol - 0x10;
        else if ((n->vol & 0xF0) == 0x80) { c->vol -= n->vol & 15; if (c->vol < 0) c->vol = 0; }
        else if ((n->vol & 0xF0) == 0x90) { c->vol += n->vol & 15; if (c->vol > 64) c->vol = 64; }
        else if ((n->vol & 0xF0) == 0xA0) c->vibspd = n->vol & 15;
        else if ((n->vol & 0xF0) == 0xB0) { if (n->vol & 15) c->vibdep = n->vol & 15; }
        else if ((n->vol & 0xF0) == 0xF0) { if (n->vol & 15) c->portaspd = (n->vol & 15) * 16; }

        switch (eff) {
        case 0x1: if (par || !T.xm) c->porta_up = par; break;
        case 0x2: if (par || !T.xm) c->porta_dn = par; break;
        case 0x3: if (par) c->portaspd = par; break;
        case 0x4: if (par & 15) c->vibdep = par & 15; if (par >> 4) c->vibspd = par >> 4; break;
        case 0x5: case 0x6: case 0xA: if (par || !T.xm) c->vslide = par; break;
        case 0x7: if (par & 15) c->tremdep = par & 15; if (par >> 4) c->tremspd = par >> 4; break;
        case 0xB: T.next_ord = par; T.next_row = 0; T.jump = 1; break;
        case 0xC: c->vol = par > 64 ? 64 : par; break;
        case 0xD: if (!T.jump) T.next_ord = T.ord + 1;
                  T.next_row = (par >> 4) * 10 + (par & 15); T.jump = 1; break;
        case 0xE:
            switch (par >> 4) {
            case 0x1: if (par & 15) c->fporta_up = par & 15; c->period -= c->fporta_up * pmul(); break;
            case 0x2: if (par & 15) c->fporta_dn = par & 15; c->period += c->fporta_dn * pmul(); break;
            case 0x6:
                if (!(par & 15)) c->ploop_row = T.row;
                else {
                    if (!c->ploop_n) c->ploop_n = par & 15;
                    else c->ploop_n--;
                    if (c->ploop_n) { T.next_row = c->ploop_row; T.next_ord = T.ord; T.jump = 2; }
                }
                break;
            case 0x9: c->retrig = par & 15; break;
            case 0xA: c->vol += par & 15; if (c->vol > 64) c->vol = 64; break;
            case 0xB: c->vol -= par & 15; if (c->vol < 0) c->vol = 0; break;
            case 0xC: c->cut_at = par & 15; break;
            case 0xE: if (!T.pdelay_n) T.pdelay = par & 15; break;
            }
            break;
        case 0xF:
            if (par == 0) break;
            if (par < 0x20) T.speed = par; else T.bpm = par;
            T.tick_len = T.rate * 5 / (T.bpm * 2);
            break;
        case 0x10: T.gvol = par > 64 ? 64 : par; break;          /* G */
        case 0x11: if (par) c->gvslide = par; break;             /* H */
        case 0x15: c->envtick = par; break;                      /* L */
        case 0x1B: if (par) c->mretrig_par = par; break;         /* R */
        case 0x21:                                               /* X */
            if ((par >> 4) == 1) c->period -= (par & 15);
            else if ((par >> 4) == 2) c->period += (par & 15);
            break;
        }
    }
}

static void tick_effects(void)
{
    int i;
    for (i = 0; i < T.nch; i++) {
        Chan *c = &T.ch[i];
        Note *n = T.pat[T.order[T.ord]] ? &T.pat[T.order[T.ord]][T.row * T.nch + i] : 0;
        int eff, par, vc;
        if (!n) continue;
        eff = n->eff; par = n->par; vc = n->vol;
        c->arp = 0; c->vib = 0;

        if (c->delay_at == T.tick) {
            Note *d = &c->delayed;
            if (d->note && d->note < 97) {
                if (d->ins && d->ins <= T.ninst && T.ins[d->ins - 1].nsmp) c->vol = T.ins[d->ins - 1].smp[0]->vol;
                trigger(c, d->note, d->ins);
            }
            if (d->vol >= 0x10 && d->vol <= 0x50) c->vol = d->vol - 0x10;
        }
        if (c->cut_at == T.tick) c->vol = 0;
        if (c->retrig && T.tick % c->retrig == 0 && c->s) { c->pos = 0; c->frac = 0; c->dir = 1; }
        if (eff == 0x14 && T.tick == par) { c->keyoff = 1; if (!c->in || !(c->in->envtype & 1)) c->vol = 0; }

        if (T.tick == 0) continue;
        if ((vc & 0xF0) == 0x60) { c->vol -= vc & 15; if (c->vol < 0) c->vol = 0; }
        if ((vc & 0xF0) == 0x70) { c->vol += vc & 15; if (c->vol > 64) c->vol = 64; }
        if ((vc & 0xF0) == 0xB0 || (vc & 0xF0) == 0xA0) { c->vibpos += c->vibspd; c->vib = wave(c->vibpos) * c->vibdep / 64; }
        if ((vc & 0xF0) == 0xF0) porta_to(c);

        switch (eff) {
        case 0x0:
            if (par) {
                int k = T.tick % 3, semis = k == 1 ? par >> 4 : k == 2 ? par & 15 : 0;
                if (semis) {
                    if (T.xm) c->arp = -semis * 64;
                    else c->arp = (int)((double)c->period * pow2_768(-semis * 64)) - c->period;
                }
            }
            break;
        case 0x1: c->period -= c->porta_up * pmul(); break;
        case 0x2: c->period += c->porta_dn * pmul(); break;
        case 0x3: porta_to(c); break;
        case 0x4: c->vibpos += c->vibspd; c->vib = wave(c->vibpos) * c->vibdep / (T.xm ? 64 : 128); break;
        case 0x5: porta_to(c); vslide(c, c->vslide); break;
        case 0x6: c->vibpos += c->vibspd; c->vib = wave(c->vibpos) * c->vibdep / (T.xm ? 64 : 128); vslide(c, c->vslide); break;
        case 0x7: c->trempos += c->tremspd; c->tvol = wave(c->trempos) * c->tremdep / 64; break;
        case 0xA: vslide(c, c->vslide); break;
        case 0x11: {
            int up = c->gvslide >> 4, dn = c->gvslide & 15;
            T.gvol += up ? up : -dn;
            if (T.gvol < 0) T.gvol = 0;
            if (T.gvol > 64) T.gvol = 64;
            break;
        }
        case 0x1B:
            if ((c->mretrig_par & 15) && T.tick % (c->mretrig_par & 15) == 0 && c->s) { c->pos = 0; c->frac = 0; }
            break;
        }
        if (c->period < (T.xm ? 1 : 28)) c->period = T.xm ? 1 : 28;
        if (c->period > (T.xm ? 7680 + 768 : 6848)) c->period = T.xm ? 7680 + 768 : 6848;
    }
}

static void tick(void)
{
    int i;
    if (T.tick == 0 && T.pdelay_n == 0) {
        T.jump = 0;
        row_start();
        if (T.ended) return;
    }
    tick_effects();
    for (i = 0; i < T.nch; i++) {
        Chan *c = &T.ch[i];
        envelope(c);
        if (c->keyoff && c->in && (c->in->envtype & 1)) {
            c->fade -= c->in->fadeout;
            if (c->fade < 0) c->fade = 0;
        }
        if (c->on && c->s) c->step = period_step(c->period + c->arp + c->vib);
    }
    if (++T.tick >= T.speed) {
        T.tick = 0;
        /* A pattern delay plays the row again, its notes not struck. */
        if (T.pdelay) { T.pdelay_n = T.pdelay; T.pdelay = 0; return; }
        if (T.pdelay_n) { T.pdelay_n--; if (T.pdelay_n) return; }
        if (T.jump) {
            if (T.next_ord <= T.ord && T.jump == 1) {      /* a jump back: the song has looped */
                T.ended = 1;
                return;
            }
            T.ord = T.next_ord;
            T.row = T.next_row;
        } else if (++T.row >= T.rows[T.order[T.ord]]) {
            T.row = 0;
            T.ord++;
        }
        if (T.ord >= T.nord) { T.ended = 1; return; }
        if (T.row >= T.rows[T.order[T.ord]]) T.row = 0;
    }
}

/* ---- mixing ------------------------------------------------------------------ */
static void mix(u8 *out, int n)
{
    static s32 acc[2048];
    /* One channel at full volume is half the speaker's range for a
     * 4-channel song, less for more channels, so they can add up. */
    int i, k, div = 4096 * (T.nch <= 4 ? 2 : T.nch <= 16 ? 3 : 4);

    for (i = 0; i < n; i++) acc[i] = 0;
    for (k = 0; k < T.nch; k++) {
        Chan *c = &T.ch[k];
        Sample *s = c->s;
        int v;
        if (!c->on || !s || !s->len) continue;
        v = c->vol + c->tvol;
        if (v < 0) v = 0;
        if (v > 64) v = 64;
        v = v * c->envval * T.gvol / 64;              /* 0..4096             */
        v = (int)(((s32)v * (c->fade >> 4)) >> 12);   /* and fading out      */
        if (!v) {                                     /* silent, but moving  */
            for (i = 0; i < n && c->on; i++) {
                c->frac += c->step; c->pos += c->frac >> 16; c->frac &= 0xFFFF;
                if (c->pos >= s->len) {
                    if (s->loop && s->llen) c->pos = s->lstart + (c->pos - s->lstart) % s->llen;
                    else c->on = 0;
                }
            }
            continue;
        }
        for (i = 0; i < n; i++) {
            s32 a, b;
            u32 p = c->pos, q = p + 1;
            if (q >= s->len) q = s->loop ? s->lstart : p;
            a = s->data[p]; b = s->data[q];
            acc[i] += (a + (((b - a) * (s32)(c->frac >> 1)) >> 15)) * v;
            if (c->dir > 0) {
                c->frac += c->step; c->pos += c->frac >> 16; c->frac &= 0xFFFF;
                if (s->loop && c->pos >= s->lstart + s->llen) {
                    if (s->loop == 2) {                /* ping-pong: turn back */
                        c->dir = -1;
                        c->pos = s->lstart + s->llen - 1 - (c->pos - (s->lstart + s->llen)) % s->llen;
                    } else
                        c->pos = s->lstart + (c->pos - s->lstart) % s->llen;
                } else if (!s->loop && c->pos >= s->len) { c->on = 0; break; }
            } else {
                u32 back;
                c->frac += c->step; back = c->frac >> 16; c->frac &= 0xFFFF;
                if (c->pos < s->lstart + back) { c->dir = 1; c->pos = s->lstart; }
                else c->pos -= back;
            }
        }
    }
    for (i = 0; i < n; i++) {
        s32 v = acc[i] / div;
        if (v > 127) v = 127;
        if (v < -128) v = -128;
        out[i] = (u8)(v + 128);
    }
}

/* ---- the outside ------------------------------------------------------------- */
int tracker_load(u8 *data, u32 len, int rate)
{
    int i;
    tables();
    for (i = 0; i < (int)sizeof T; i++) ((u8 *)&T)[i] = 0;
    if (!load_xm(data, len) && !load_mod(data, len)) return 0;
    T.rate = rate;
    T.gvol = 64;
    T.tick_len = T.rate * 5 / (T.bpm * 2);
    T.tick_left = 0;
    for (i = 0; i < CHANS; i++) { T.ch[i].fade = 65536; T.ch[i].envval = 64; }
    T.playing = 1;
    return 1;
}

/* n samples of the song into out.  Returns how many it made: fewer than
 * n once the song has ended. */
int tracker_render(u8 *out, int n)
{
    int done = 0;
    while (done < n && !T.ended) {
        int take;
        if (T.tick_left == 0) {
            tick();
            if (T.ended) break;
            T.tick_left = T.tick_len;
        }
        take = T.tick_left;
        if (take > n - done) take = n - done;
        if (take > 2048) take = 2048;
        mix(out + done, take);
        done += take;
        T.tick_left -= take;
        T.played += (u32)take;
    }
    return done;
}

const char *tracker_title(void) { return T.title; }
const char *tracker_kind(void)  { return T.xm ? "XM" : "MOD"; }
int tracker_channels(void)      { return T.nch; }
int tracker_position(int *ord, int *nord) { *ord = T.ord; *nord = T.nord; return T.row; }
u32 tracker_samples(void)       { return T.played; }
