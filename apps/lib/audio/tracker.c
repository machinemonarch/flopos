/* ===========================================================================
 * tracker.c -- playing .mod, .xm, .s3m and .it songs.
 *
 * A tracker song is its own instruments (short recordings, "samples") and
 * a score that says, row by row and channel by channel, which sample to
 * play at which pitch and loudness, with effects -- slides, vibrato,
 * volume fades -- worked out a "tick" at a time, several ticks to a row.
 * Playing one means doing that, and mixing every channel's sample at its
 * pitch into one stream for the Sound Blaster.  It is light work: a few
 * thousand multiply-adds per tick.
 *
 * Four formats:
 *   MOD  ProTracker's, from the Amiga.  4 channels (or more, in the PC
 *        variants), 31 samples, pitch as Amiga periods.
 *   XM   FastTracker 2's.  Up to 32 channels, instruments that choose a
 *        sample by note and shape its loudness with an envelope, a
 *        volume column beside the effect, pitch on a linear scale.
 *   S3M  Scream Tracker 3's.  Up to 32 channels, samples that each give
 *        their own playing rate, a volume column, effects named by letter.
 *   IT   Impulse Tracker's.  Up to 64 channels, instruments much like XM's,
 *        and samples that may be packed (Impulse Tracker 2.14's own
 *        compression).
 *
 * The score is never unpacked into memory.  Each row is read from the
 * file when it is reached, so a long song costs nothing beyond the file
 * itself.  S3M and IT are put into XM's terms as they are read: their
 * effects become XM's effects, and each sample's rate becomes XM's
 * relative note and finetune.  So one player plays all four.
 *
 * Left out: panning (the Sound Blaster here plays mono), IT's new-note
 * actions (a new note cuts the old one), filters, pitch and panning
 * envelopes, and samples' own vibrato.  The song plays once and stops.
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

#define CHANS     64
#define SAMPLES   256
#define INSTRS    128
#define ENVPTS    25

enum { F_MOD, F_XM, F_S3M, F_IT };

/* Effects of S3M and IT that XM has no number for. */
#define E_SPEED   0x24                   /* Axx: ticks to a row             */
#define E_TEMPO   0x25                   /* Txx: tempo; IT's T0x/T1x slide  */
#define E_BREAK   0x26                   /* IT's Cxx: the row, not in BCD   */
#define E_CHVOL   0x27                   /* IT's Mxx: the channel's volume  */
#define E_HIOFS   0x28                   /* IT's SAx: offsets past 64K      */
#define E_VSL     0x29                   /* Dxy, with its fine slides and memory */
#define E_PUP     0x2A                   /* Fxx, and Exx below: one memory  */
#define E_PDN     0x2B                   /*   for both, fine ones included  */

#define NOTE_OFF  97                     /* the key let go                  */
#define NOTE_CUT  98                     /* S3M's and IT's ^^: silence      */

typedef struct {
    s8  *data;
    u32  len, lstart, llen;
    u8   loop;                           /* 0 none, 1 forward, 2 ping-pong  */
    s8   finetune;                       /* XM: -128..127; MOD: -8..7 x 16  */
    u8   vol;
    s8   relnote;
    u8   gv;                             /* IT's own volume for it, 0..64   */
} Sample;

typedef struct {
    u8      keymap[96];                  /* note -> smp[]; 255 for none     */
    s8      shift[96];                   /* IT: the note it plays instead, as a change */
    Sample *smp[16];
    int     nsmp;
    u16     env[ENVPTS][2];              /* tick, value 0..64               */
    int     envn, envsus, envls, envle;
    u8      envtype;                     /* 1 on, 2 sustain, 4 loop         */
    u8      fades;                       /* IT: a key let go fades, envelope or not */
    u8      gv;                          /* IT's volume for the instrument, 0..64 */
    u8      nna;                         /* IT: a new note 0 cuts this one, 1 lets it
                                            carry on, 2 lets its key go, 3 fades it */
    u8      dct, dca;                    /* IT: unless it is the same 1 note, 2 sample,
                                            3 instrument -- then 0 cut, 1 let go, 2 fade */
    int     fadeout;
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
    int     retrig, cut_at, delay_at, ploop_row, ploop_n, offset, ofs_par, hiofs;
    int     arp;                         /* period added this tick          */
    int     vib;                         /* period added by vibrato         */
    int     tvol;                        /* volume added by tremolo         */
    int     cvol;                        /* IT's channel volume, 0..64      */
    int     key, owner;                  /* the note as played; IT: its channel */
    int     dmem, dnorm, pmem, pslide;   /* S3M, IT: D's and E/F's memories */
    Note    delayed;
    int     mretrig_par, gvslide;
} Chan;

static struct {
    int     fmt, xm, nch, nord, npat, ninst;  /* xm: XM's pitch and effects (XM, S3M, IT) */
    u8      order[256];
    int     rows[256];
    Sample  smp[SAMPLES];
    Instr   ins[INSTRS];
    int     nsmp;
    Chan    ch[CHANS];
    int     speed, bpm, tick, row, ord, gvol;
    int     next_row, next_ord, jump, pdelay, pdelay_n;
    int     rate, tick_len, tick_left;
    int     playing, ended;
    char    title[28];
    u32     played;                      /* samples out                     */
} T;

/* Where the score is in the file, and where reading it stopped. */
static struct {
    const u8 *d;
    u32  at[256], end[256];              /* each pattern's packed rows      */
    int  pat, row;                       /* the next row to read ...        */
    u32  pos;                            /* ... starts here                 */
    u8   on[CHANS];                      /* channels the song plays on      */
    u8   ordmap[256];                    /* S3M, IT: order as stored -> as kept */
    u8   mask[CHANS], last[CHANS][5];    /* IT: what each channel repeats   */
    int  maxch;
} R;

static Note s_row[CHANS];                /* the row being played            */

/* IT: notes still sounding under newer ones (see to_background). */
#define VOICES 16
static Chan s_bg[VOICES];

/* ---- reading ---------------------------------------------------------------- */
static u32 le16(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8); }
static u32 le32(const u8 *p) { return le16(p) | (le16(p + 2) << 16); }
static u32 be16(const u8 *p) { return ((u32)p[0] << 8) | p[1]; }

static int same(const u8 *p, const char *s)
{
    while (*s) if (*p++ != (u8)*s++) return 0;
    return 1;
}

static void set_title(const u8 *p, int max)
{
    int i, k = 0;
    if (max > (int)sizeof T.title - 1) max = (int)sizeof T.title - 1;
    for (i = 0; i < max && p[i]; i++) T.title[k++] = (char)(p[i] >= 32 && p[i] < 127 ? p[i] : ' ');
    while (k > 0 && T.title[k - 1] == ' ') k--;
    T.title[k] = 0;
}

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

/* 2^(e/768), for any e: e is in 768ths of an octave. */
static double pow2_768(int e)
{
    int oct = e >= 0 ? e / 768 : -((-e + 767) / 768);
    double v = (double)s_pow2[e - oct * 768] / 65536.0;
    while (oct > 0) { v *= 2.0; oct--; }
    while (oct < 0) { v *= 0.5; oct++; }
    return v;
}

/* A sample that plays at hz for its middle note, as XM says it: a
 * relative note and a finetune (128ths of a semitone, here in twos). */
static void rate_to_pitch(Sample *s, u32 hz)
{
    int lo = -768 * 6, hi = 768 * 6, e;
    if (hz < 100) hz = 8363;
    while (hi - lo > 1) {
        int mid = (lo + hi) / 2;
        if (8363.0 * pow2_768(mid) <= (double)hz) lo = mid; else hi = mid;
    }
    e = 8363.0 * pow2_768(hi) - hz < hz - 8363.0 * pow2_768(lo) ? hi : lo;
    s->relnote = (s8)(e >= 0 ? e / 64 : -((-e + 63) / 64));
    s->finetune = (s8)((e - s->relnote * 64) * 2);
}

/* An instrument that is just one sample, for every note: MOD's and S3M's,
 * and IT's when the song has no instruments. */
static void one_sample(Instr *in, Sample *s)
{
    int k;
    for (k = 0; k < 96; k++) in->keymap[k] = 0;
    in->smp[0] = s; in->nsmp = 1;
    in->envtype = 0; in->envn = 0; in->fadeout = 0; in->fades = 0;
}

static void clamp_loop(Sample *s)
{
    if (s->lstart > s->len) s->lstart = s->len;
    if (s->lstart + s->llen > s->len) s->llen = s->len - s->lstart;
    if (s->llen < 2) s->loop = 0;
}

/* S3M and IT mark orders to skip (254) and the end (255).  The list is
 * kept without them, and jumps are mapped to where they now are. */
static void orders(const u8 *o, int n)
{
    int i, k = 0, end = 0;
    for (i = 0; i < 256; i++) {
        R.ordmap[i] = (u8)k;
        if (end || i >= n) continue;
        if (o[i] == 255) { end = 1; continue; }
        if (o[i] == 254) continue;
        if (k < 255) T.order[k++] = o[i];
    }
    T.nord = k;
}

/* ---- MOD ---------------------------------------------------------------------- */
static int load_mod(const u8 *d, u32 n)
{
    int i, maxpat = 0, ch = 0;
    u32 at;
    const u8 *sig = d + 1080;

    if (n < 1084) return 0;
    if (!((sig[0] == 'M' && sig[2] == 'K') || (sig[0] == 'F' && sig[1] == 'L' && sig[2] == 'T') ||
          (sig[1] == 'C' && sig[2] == 'H' && sig[3] == 'N') || (sig[2] == 'C' && sig[3] == 'H'))) return 0;
    if (sig[0] == 'M' || sig[0] == 'F') ch = sig[0] == 'F' ? sig[3] - '0' : 4;
    else if (sig[1] == 'C') ch = sig[0] - '0';
    else ch = (sig[0] - '0') * 10 + (sig[1] - '0');
    if (ch < 1 || ch > 32) return 0;

    T.fmt = F_MOD; T.xm = 0; T.nch = ch;
    set_title(d, 20);
    T.nord = d[950];
    if (T.nord < 1 || T.nord > 128) return 0;
    for (i = 0; i < 128; i++) { T.order[i] = d[952 + i]; if (d[952 + i] > maxpat) maxpat = d[952 + i]; }
    T.npat = maxpat + 1;
    at = 1084 + (u32)T.npat * 64 * (u32)ch * 4;
    if (at > n) return 0;
    for (i = 0; i < 256; i++) T.rows[i] = 64;
    for (i = 0; i < ch; i++) R.on[i] = 1;

    /* Samples follow the patterns, in order. */
    T.nsmp = 31; T.ninst = 31;
    for (i = 0; i < 31; i++) {
        const u8 *h = d + 20 + i * 30;
        Sample *s = &T.smp[i];
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
        one_sample(&T.ins[i], s);
    }
    T.speed = 6; T.bpm = 125;
    return 1;
}

/* A MOD row is ch cells of 4 bytes, at a place that can be worked out. */
static void mod_row(int pat, int row)
{
    const u8 *b = R.d + 1084 + ((u32)pat * 64 + (u32)row) * (u32)T.nch * 4;
    int c;
    for (c = 0; c < T.nch; c++, b += 4) {
        Note *np = &s_row[c];
        int period = ((b[0] & 0x0F) << 8) | b[1], k, best = 0, bd = 99999;
        np->ins = (u8)((b[0] & 0xF0) | (b[2] >> 4));
        np->eff = (u8)(b[2] & 0x0F);
        np->par = b[3];
        if (period) {
            for (k = 0; k < 36; k++) {
                int dd = period - s_amiga[k];
                if (dd < 0) dd = -dd;
                if (dd < bd) { bd = dd; best = k; }
            }
            np->note = (u8)(best + 1 + 36);       /* C-1 as XM's C-4  */
        }
    }
}

/* ---- XM ----------------------------------------------------------------------- */
/* One packed cell: a byte saying which of the five follow, or all five. */
static u32 xm_cell(u32 at, u32 end, Note *np)
{
    const u8 *d = R.d;
    Note n;
    n.note = n.ins = n.vol = n.eff = n.par = 0;
    if (at < end) {
        u8 b = d[at];
        if (b & 0x80) {
            at++;
            if (b & 1  && at < end) n.note = d[at++];
            if (b & 2  && at < end) n.ins  = d[at++];
            if (b & 4  && at < end) n.vol  = d[at++];
            if (b & 8  && at < end) n.eff  = d[at++];
            if (b & 16 && at < end) n.par  = d[at++];
        } else if (at + 5 <= end) {
            n.note = d[at]; n.ins = d[at + 1]; n.vol = d[at + 2];
            n.eff = d[at + 3]; n.par = d[at + 4];
            at += 5;
        } else
            at = end;
    }
    if (n.note > NOTE_OFF) n.note = 0;
    if (np) *np = n;
    return at;
}

static u32 xm_line(u32 at, u32 end, int keep)
{
    int c;
    for (c = 0; c < T.nch; c++) at = xm_cell(at, end, keep ? &s_row[c] : 0);
    return at;
}

static int load_xm(u8 *d, u32 n)
{
    u32 at, hs;
    int i, p;

    if (n < 80 || !same(d, "Extended Module: ")) return 0;
    set_title(d + 17, 20);
    hs = le32(d + 60);
    T.fmt = F_XM; T.xm = 1;
    T.nord = (int)le16(d + 64);
    T.nch = (int)le16(d + 68);
    T.npat = (int)le16(d + 70);
    T.ninst = (int)le16(d + 72);
    T.speed = (int)le16(d + 76);
    T.bpm = (int)le16(d + 78);
    if (T.nch < 1 || T.nch > 32 || T.npat > 256 || T.ninst > INSTRS || T.nord > 256 || T.nord < 1) return 0;
    if (n < 80 + 256) return 0;
    for (i = 0; i < 256; i++) T.order[i] = d[80 + i];
    for (i = 0; i < T.nch; i++) R.on[i] = 1;
    at = 60 + hs;

    for (p = 0; p < 256; p++) T.rows[p] = 64;
    for (p = 0; p < T.npat; p++) {
        u32 ph, plen;
        int rows;
        if (at + 9 > n) return 0;
        ph = le32(d + at);
        rows = (int)le16(d + at + 5);
        plen = le16(d + at + 7);
        at += ph;
        if (rows < 1 || rows > 256) rows = 64;
        T.rows[p] = rows;
        if (at > n || plen > n - at) return 0;
        R.at[p] = at; R.end[p] = at + plen;     /* none: an empty pattern */
        at += plen;
    }

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
            in->fadeout = (int)le16(h + 239);
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
         * 8-bit in place -- the speaker here plays 8 bits anyway.  So the
         * file is changed, and must be loaded only once. */
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
            clamp_loop(s);
        }
    }
    if (T.speed < 1) T.speed = 6;
    if (T.bpm < 32) T.bpm = 125;
    return 1;
}

/* ---- S3M's and IT's effects, as XM's ---------------------------------------- */
static void slide_letter(Note *n, int p, int eff)
{
    int hi = p >> 4, lo = p & 15;
    if (eff == 0x0A && hi == 15 && lo) { n->eff = 0x0E; n->par = (u8)(0xB0 | lo); return; }  /* fine down */
    if (eff == 0x0A && lo == 15 && hi) { n->eff = 0x0E; n->par = (u8)(0xA0 | hi); return; }  /* fine up   */
    n->eff = (u8)eff; n->par = (u8)p;
}

/* The effects, named A to Z (1 to 26). */
static void letter(Note *n, int cmd, int p)
{
    int hi = p >> 4, lo = p & 15;
    n->eff = 0; n->par = 0;
    if (cmd < 1 || cmd > 26) return;
    switch ('A' + cmd - 1) {
    case 'A': if (p) { n->eff = E_SPEED; n->par = (u8)p; } break;
    case 'B': n->eff = 0x0B; n->par = R.ordmap[p & 255]; break;
    case 'C': n->eff = T.fmt == F_IT ? E_BREAK : 0x0D; n->par = (u8)p; break;
    case 'D': n->eff = E_VSL; n->par = (u8)p; break;
    case 'E': n->eff = E_PDN; n->par = (u8)p; break;
    case 'F': n->eff = E_PUP; n->par = (u8)p; break;
    case 'G': n->eff = 3; n->par = (u8)p; break;
    case 'H': n->eff = 4; n->par = (u8)p; break;
    case 'J': n->eff = 0; n->par = (u8)p; break;              /* arpeggio    */
    case 'K': slide_letter(n, p, 6); break;
    case 'L': slide_letter(n, p, 5); break;
    case 'M': if (T.fmt == F_IT) { n->eff = E_CHVOL; n->par = (u8)p; } break;
    case 'O': n->eff = 9; n->par = (u8)p; break;
    case 'Q': n->eff = 0x1B; n->par = (u8)p; break;           /* retrigger   */
    case 'R': n->eff = 7; n->par = (u8)p; break;              /* tremolo     */
    case 'S':
        if (hi == 0xA && T.fmt == F_IT) { n->eff = E_HIOFS; n->par = (u8)lo; }
        else if (hi == 0xB) { n->eff = 0x0E; n->par = (u8)(0x60 | lo); }   /* loop  */
        else if (hi >= 0xC && hi <= 0xE) { n->eff = 0x0E; n->par = (u8)(hi << 4 | lo); }
        break;
    case 'T': n->eff = E_TEMPO; n->par = (u8)p; break;
    case 'U': n->eff = 4; n->par = (u8)((p & 0xF0) | ((lo + 3) >> 2)); break;   /* fine vibrato */
    case 'V': n->eff = 0x10; n->par = (u8)(T.fmt == F_IT ? p / 2 : p); break;
    case 'W': n->eff = 0x11; n->par = (u8)p; break;
    }
}

/* ---- S3M ---------------------------------------------------------------------- */
static void s3m_cell(Note *n, int note, int ins, int vol, int cmd, int par)
{
    if (note == 254) n->note = NOTE_CUT;
    else if (note < 254 && (note & 15) < 12) {
        int k = (note >> 4) * 12 + (note & 15) + 1;   /* C-4 is XM's C-4 */
        n->note = (u8)(k <= 96 ? k : 0);
    }
    n->ins = (u8)ins;
    if (vol >= 0 && vol <= 64) n->vol = (u8)(0x10 + vol);
    letter(n, cmd, par);
}

/* A row: entries, each a byte naming the channel and what follows, then 0. */
static u32 s3m_line(u32 at, u32 end, int keep)
{
    const u8 *d = R.d;
    while (at < end) {
        int w = d[at++], ch, note = 255, ins = 0, vol = -1, cmd = 0, par = 0;
        if (!w) break;
        ch = w & 31;
        if (w & 32)  { if (at + 2 > end) return end; note = d[at]; ins = d[at + 1]; at += 2; }
        if (w & 64)  { if (at + 1 > end) return end; vol = d[at++]; }
        if (w & 128) { if (at + 2 > end) return end; cmd = d[at]; par = d[at + 1]; at += 2; }
        if (keep && ch < T.nch && R.on[ch]) s3m_cell(&s_row[ch], note, ins, vol, cmd, par);
    }
    return at;
}

static int load_s3m(u8 *d, u32 n)
{
    u32 base, i, j;
    int nord, nins, npat, ffi, c;

    if (n < 96 || !same(d + 44, "SCRM")) return 0;
    nord = (int)le16(d + 32); nins = (int)le16(d + 34); npat = (int)le16(d + 36);
    ffi = (int)le16(d + 42);                       /* 2: samples unsigned    */
    if (nord > 256 || nins > INSTRS || nins > SAMPLES || npat > 256) return 0;
    base = 96 + (u32)nord;
    if (base + 2u * (u32)(nins + npat) > n) return 0;
    T.fmt = F_S3M; T.xm = 1;
    set_title(d, 27);
    for (c = 0; c < 32; c++) { R.on[c] = d[64 + c] < 16; if (R.on[c]) T.nch = c + 1; }
    if (!T.nch) return 0;
    orders(d + 96, nord);
    T.gvol = d[48] && d[48] <= 64 ? d[48] : 64;
    T.speed = d[49] ? d[49] : 6;
    T.bpm = d[50] >= 32 ? d[50] : 125;

    /* Instruments are samples.  Their data is made signed 8-bit in place. */
    T.ninst = T.nsmp = nins;
    for (i = 0; i < (u32)nins; i++) {
        u32 p = le16(d + base + 2 * i) * 16, at, len, ls, le;
        Sample *s = &T.smp[i];
        const u8 *h = d + p;
        int fl, wide;
        one_sample(&T.ins[i], s);
        if (p > n - 80 || h[0] != 1 || h[30] != 0) continue;   /* empty, AdLib, or packed */
        at = (((u32)h[13] << 16) | le16(h + 14)) * 16;
        len = le32(h + 16); ls = le32(h + 20); le = le32(h + 24);
        fl = h[31]; wide = (fl & 4) != 0;
        s->vol = h[28] > 64 ? 64 : h[28];
        rate_to_pitch(s, le32(h + 32));
        if (at >= n || !len) continue;
        if (len > (n - at) >> wide) len = (n - at) >> wide;
        s->data = (s8 *)(d + at);
        for (j = 0; j < len; j++) {
            if (wide) { u32 v = le16(d + at + 2 * j); if (ffi == 2) v ^= 0x8000; s->data[j] = (s8)(v >> 8); }
            else s->data[j] = (s8)(d[at + j] ^ (ffi == 2 ? 0x80 : 0));
        }
        s->len = len;
        s->loop = (fl & 1) && le > ls;
        s->lstart = ls; s->llen = le > ls ? le - ls : 0;
        clamp_loop(s);
    }

    T.npat = npat;
    for (i = 0; i < 256; i++) T.rows[i] = 64;
    for (i = 0; i < (u32)npat; i++) {
        u32 p = le16(d + base + 2u * (u32)nins + 2 * i) * 16;
        if (!p || p > n - 2) continue;                 /* an empty pattern       */
        R.at[i] = p + 2;
        R.end[i] = le16(d + p) > n - (p + 2) ? n : p + 2 + le16(d + p);
    }
    return 1;
}

/* ---- IT ----------------------------------------------------------------------- */
static void it_cell(Note *n, int note, int ins, int vp, int cmd, int par)
{
    static const u8 spd[10] = { 0, 1, 4, 8, 16, 32, 64, 96, 128, 255 };
    if (note >= 0) {
        if (note <= 119) { int k = note - 11; n->note = (u8)(k >= 1 && k <= 96 ? k : 0); }   /* C-5 is XM's C-4 */
        else n->note = note == 254 ? NOTE_CUT : NOTE_OFF;      /* 255 off; the rest fade */
    }
    n->ins = (u8)ins;
    letter(n, cmd, par);
    if (vp < 0) return;
    /* The volume column, as XM's where XM has the same thing. */
    if (vp <= 64)       n->vol = (u8)(0x10 + vp);
    else if (vp <= 74)  n->vol = (u8)(0x90 + vp - 65);          /* fine up     */
    else if (vp <= 84)  n->vol = (u8)(0x80 + vp - 75);          /* fine down   */
    else if (vp <= 94)  n->vol = (u8)(0x70 + vp - 85);          /* slide up    */
    else if (vp <= 104) n->vol = (u8)(0x60 + vp - 95);          /* slide down  */
    else if (vp <= 124) {                                       /* pitch slides, where there's room */
        if (!n->eff && !n->par) { n->eff = (u8)(vp <= 114 ? 2 : 1); n->par = (u8)(((vp - 105) % 10) * 4); }
    } else if (vp >= 193 && vp <= 202) {                        /* slide to the note */
        if (!n->eff && !n->par) { n->eff = 3; n->par = spd[vp - 193]; }
        else { int k = (spd[vp - 193] + 15) / 16; n->vol = (u8)(0xF0 + (k > 15 ? 15 : k)); }
    } else if (vp >= 203 && vp <= 212) n->vol = (u8)(0xB0 + vp - 203);   /* vibrato */
}

static void it_reset(void)
{
    int c, k;
    for (c = 0; c < CHANS; c++) { R.mask[c] = 0; for (k = 0; k < 5; k++) R.last[c][k] = 0; }
}

/* A row: for each channel with something, a byte naming it, then what it
 * has -- or a repeat of what it had last.  A 0 ends the row. */
static u32 it_line(u32 at, u32 end, int keep)
{
    const u8 *d = R.d;
    while (at < end) {
        int cv = d[at++], ch, m;
        u8 *l;
        if (!cv) break;
        ch = (cv - 1) & 63;
        if (cv & 128) { if (at >= end) return end; R.mask[ch] = d[at++]; }
        m = R.mask[ch]; l = R.last[ch];
        if (m & 1) { if (at >= end) return end; l[0] = d[at++]; }
        if (m & 2) { if (at >= end) return end; l[1] = d[at++]; }
        if (m & 4) { if (at >= end) return end; l[2] = d[at++]; }
        if (m & 8) { if (at + 2 > end) return end; l[3] = d[at]; l[4] = d[at + 1]; at += 2; }
        if (R.on[ch] && ch >= R.maxch) R.maxch = ch + 1;
        if (keep && ch < T.nch && R.on[ch])
            it_cell(&s_row[ch], m & 17 ? l[0] : -1, m & 34 ? l[1] : 0, m & 68 ? l[2] : -1,
                    m & 136 ? l[3] : 0, m & 136 ? l[4] : 0);
    }
    return at;
}

/* Impulse Tracker 2.14's packed samples, and 2.15's, which add up twice.
 * The data comes in blocks: a 16-bit length, then bits read low first.
 * Each value is a change from the last sample, at a width that the stream
 * itself changes by values set aside to say so.  A block holds up to 32 KB
 * of samples. */
typedef struct { const u8 *p, *end; u32 bits; int have; } Lsb;

static u32 lsb(Lsb *b, int w)
{
    u32 v;
    while (b->have < w) { b->bits |= (u32)(b->p < b->end ? *b->p++ : 0) << b->have; b->have += 8; }
    v = b->bits & ((1u << w) - 1);
    b->bits >>= w; b->have -= w;
    return v;
}

static void it_unpack(const u8 *p, const u8 *end, s8 *out, u32 len, int wide, int twice)
{
    int top = wide ? 17 : 9;                       /* the widest width       */
    u32 done = 0, k;
    for (k = 0; k < len; k++) out[k] = 0;
    while (done < len && end - p >= 2) {
        u32 blk = wide ? 0x4000 : 0x8000, bl;
        int w = top;
        s32 d1 = 0, d2 = 0;
        Lsb b;
        if (blk > len - done) blk = len - done;
        bl = le16(p); p += 2;
        if (bl > (u32)(end - p)) bl = (u32)(end - p);
        b.p = p; b.end = p + bl; b.bits = 0; b.have = 0;
        p += bl;
        for (k = 0; k < blk; ) {
            u32 v = lsb(&b, w);
            s32 x;
            if (w < 7) {
                if (v == 1u << (w - 1)) {                         /* a new width follows */
                    int nw = (int)lsb(&b, wide ? 4 : 3) + 1;
                    w = nw < w ? nw : nw + 1;
                    continue;
                }
            } else if (w < top) {
                u32 border = ((wide ? 0xFFFFu : 0xFFu) >> (top - w)) - (wide ? 8 : 4);
                if (v > border && v <= border + (wide ? 16 : 8)) {   /* values near the top */
                    v -= border;
                    w = (int)v < w ? (int)v : (int)v + 1;
                    continue;
                }
            } else if (v & (1u << (top - 1))) {                    /* the widest: top bit */
                w = (int)((v + 1) & 0xFF);
                if (w < 1 || w > top) break;
                continue;
            }
            if (w < top - 1) { int sh = 32 - w; x = (s32)(v << sh) >> sh; }
            else x = wide ? (s32)(s16)(u16)v : (s32)(s8)(u8)v;
            if (wide) { d1 = (s16)(d1 + x); d2 = (s16)(d2 + d1); out[done + k] = (s8)((twice ? d2 : d1) >> 8); }
            else      { d1 = (s8)(d1 + x);  d2 = (s8)(d2 + d1);  out[done + k] = (s8)(twice ? d2 : d1); }
            k++;
        }
        done += blk;
    }
}

static int load_it(u8 *d, u32 n, u32 room)
{
    u32 nord, nins, nsmp, npat, base, i, k, spare;
    int flags, cmwt, c;

    if (n < 192 || !same(d, "IMPM")) return 0;
    nord = le16(d + 32); nins = le16(d + 34); nsmp = le16(d + 36); npat = le16(d + 38);
    cmwt = (int)le16(d + 42); flags = (int)le16(d + 44);
    if (nord > 256 || nins > INSTRS || nsmp > SAMPLES || npat > 256) return 0;
    base = 192 + nord;
    if (base + 4 * (nins + nsmp + npat) > n) return 0;
    T.fmt = F_IT; T.xm = 1;
    set_title(d + 4, 26);
    orders(d + 192, (int)nord);
    T.gvol = d[48] >= 128 ? 64 : d[48] / 2;
    T.speed = d[50] ? d[50] : 6;
    T.bpm = d[51] >= 32 ? d[51] : 125;
    for (c = 0; c < 64; c++) { R.on[c] = !(d[64 + c] & 128); T.ch[c].cvol = d[128 + c] > 64 ? 64 : d[128 + c]; }

    /* Samples: signed 8-bit in place, or unpacked into the room after the
     * file.  A packed one there's no room for stays silent. */
    spare = (n + 15) & ~15u;
    T.nsmp = (int)nsmp;
    for (i = 0; i < nsmp; i++) {
        u32 p = le32(d + base + 4 * (nins + i)), at, len, ls, le;
        Sample *s = &T.smp[i];
        const u8 *h = d + p;
        int fl, cvt, wide;
        if (p > n - 80 || !same(h, "IMPS")) continue;
        fl = h[18]; cvt = h[46]; wide = (fl & 2) != 0;
        s->gv = h[17] > 64 ? 64 : h[17];
        s->vol = h[19] > 64 ? 64 : h[19];
        rate_to_pitch(s, le32(h + 60));
        len = le32(h + 48); at = le32(h + 72);
        if (!(fl & 1) || !len || at >= n) continue;
        if (fl & 16)      { ls = le32(h + 52); le = le32(h + 56); s->loop = fl & 64 ? 2 : 1; }
        else if (fl & 32) { ls = le32(h + 64); le = le32(h + 68); s->loop = fl & 128 ? 2 : 1; }  /* held: loops */
        else              { ls = le = 0; s->loop = 0; }
        if (fl & 8) {
            if (spare > room || len > room - spare) continue;
            s->data = (s8 *)(d + spare);
            it_unpack(d + at, d + n, s->data, len, wide, (cvt & 4) != 0);
            spare += (len + 15) & ~15u;
        } else {
            if (len > (n - at) >> wide) len = (n - at) >> wide;
            s->data = (s8 *)(d + at);
            for (k = 0; k < len; k++) {
                if (wide) { u32 v = le16(d + at + 2 * k); if (!(cvt & 1)) v ^= 0x8000; s->data[k] = (s8)(v >> 8); }
                else s->data[k] = (s8)(d[at + k] ^ (cvt & 1 ? 0 : 0x80));
            }
        }
        s->len = len; s->lstart = ls; s->llen = le > ls ? le - ls : 0;
        clamp_loop(s);
    }

    if (flags & 4) {                               /* instruments            */
        T.ninst = (int)nins;
        for (i = 0; i < nins; i++) {
            u32 p = le32(d + base + 4 * i);
            Instr *in = &T.ins[i];
            const u8 *h = d + p;
            int j;
            for (k = 0; k < 96; k++) in->keymap[k] = 255;
            in->nsmp = 0; in->envtype = 0; in->envn = 0; in->fades = 1;
            for (k = 0; k < 96; k++) in->shift[k] = 0;
            if (n < 554 || p > n - 554 || !same(h, "IMPI")) continue;
            if (cmwt >= 0x200) {
                in->nna = h[17] <= 3 ? h[17] : 0;
                in->dct = h[18] <= 3 ? h[18] : 0;
                in->dca = h[19] <= 2 ? h[19] : 0;
                const u8 *e = h + 304;                 /* the volume envelope */
                int num = e[1] > ENVPTS ? ENVPTS : e[1];
                in->fadeout = (int)le16(h + 20) * 64;
                in->gv = h[24] >= 128 ? 64 : h[24] / 2;
                in->envtype = (u8)((e[0] & 1 ? 1 : 0) | (e[0] & 4 ? 2 : 0) | (e[0] & 2 ? 4 : 0));
                in->envn = num; in->envls = e[2]; in->envle = e[3]; in->envsus = e[4];
                for (j = 0; j < num; j++) {
                    in->env[j][1] = e[6 + 3 * j] > 64 ? 64 : e[6 + 3 * j];
                    in->env[j][0] = (u16)le16(e + 7 + 3 * j);
                }
            } else
                in->fadeout = (int)le16(h + 20) * 128;  /* Impulse Tracker 1's  */
            /* The keyboard: a sample for each note.  XM's note 1 is IT's 12. */
            for (j = 12; j < 108; j++) {
                int sn = h[64 + 2 * j + 1], slot;
                if (!sn || sn > (int)nsmp) continue;
                for (slot = 0; slot < in->nsmp && in->smp[slot] != &T.smp[sn - 1]; slot++) { }
                if (slot == in->nsmp) { if (in->nsmp == 16) continue; in->smp[in->nsmp++] = &T.smp[sn - 1]; }
                in->keymap[j - 12] = (u8)slot;
                if (h[64 + 2 * j] <= 119) {
                    int sh = (int)h[64 + 2 * j] - j;
                    in->shift[j - 12] = (s8)(sh < -96 ? -96 : sh > 96 ? 96 : sh);
                }
            }
        }
    } else {                                       /* samples played directly */
        T.ninst = (int)nsmp;
        for (i = 0; i < nsmp; i++) one_sample(&T.ins[i], &T.smp[i]);
    }

    T.npat = (int)npat;
    for (i = 0; i < 256; i++) T.rows[i] = 64;
    for (i = 0; i < npat; i++) {
        u32 p = le32(d + base + 4 * (nins + nsmp + i)), at;
        int r;
        if (!p || p > n - 8) continue;             /* 64 empty rows          */
        T.rows[i] = (int)le16(d + p + 2);
        if (T.rows[i] < 1 || T.rows[i] > 256) T.rows[i] = 64;
        R.at[i] = p + 8;
        R.end[i] = le16(d + p) > n - (p + 8) ? n : p + 8 + le16(d + p);
        it_reset();                                /* read through once, to see the channels used */
        for (at = R.at[i], r = 0; r < T.rows[i]; r++) at = it_line(at, R.end[i], 0);
    }
    T.nch = R.maxch ? R.maxch : 1;
    return 1;
}

/* ---- rows --------------------------------------------------------------------- */
static u32 line(u32 at, u32 end, int keep)
{
    if (T.fmt == F_XM) return xm_line(at, end, keep);
    if (T.fmt == F_S3M) return s3m_line(at, end, keep);
    return it_line(at, end, keep);
}

/* The row into s_row.  Rows are mostly read in order, carrying on from
 * where the last one ended; after a jump, the pattern is read through
 * from its start. */
static void read_row(int pat, int row)
{
    u32 at, end;
    int c;
    for (c = 0; c < T.nch; c++) s_row[c].note = s_row[c].ins = s_row[c].vol = s_row[c].eff = s_row[c].par = 0;
    if (pat >= T.npat) return;
    if (T.fmt == F_MOD) { mod_row(pat, row); return; }
    end = R.end[pat];
    if (R.pat == pat && R.row == row) at = R.pos;
    else {
        if (T.fmt == F_IT) it_reset();
        for (at = R.at[pat], c = 0; c < row; c++) at = line(at, end, 0);
    }
    at = line(at, end, 1);
    R.pat = pat; R.row = row + 1; R.pos = at;
}

/* ---- pitch ------------------------------------------------------------------ */
/* For MOD, period is an Amiga period.  For the others, a linear "period":
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

/* The note a sample sounds at, for a note played on an instrument. */
static int sounding(Instr *in, Sample *s, int note)
{
    int k = note + (in ? in->shift[note - 1] : 0) + (T.xm ? s->relnote : 0);
    return k < 1 ? 1 : k > 119 ? 119 : k;
}

/* The sample an instrument plays for a note, or none. */
static Sample *pick(Instr *in, int note)
{
    int k;
    if (!in || !in->nsmp || note < 1 || note > 96) return 0;
    k = in->keymap[note - 1];
    if (k == 255) return 0;
    return in->smp[k < in->nsmp ? k : 0];
}

static void trigger(Chan *c, int note, int ins)
{
    Sample *s;
    if (ins) {
        if (ins > T.ninst) { c->on = 0; return; }
        c->in = &T.ins[ins - 1];
    }
    if (!c->in || note < 1 || note > 96) return;
    s = pick(c->in, note);
    if (!s || !s->len || !s->data) { c->on = 0; return; }
    c->s = s;
    c->key = note;
    c->note = sounding(c->in, s, note);
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

static void key_off(Chan *c)
{
    c->keyoff = 1;
    if (!c->in || (!(c->in->envtype & 1) && !c->in->fades)) c->vol = 0;
}

static void set_tempo(int bpm)
{
    T.bpm = bpm < 32 ? 32 : bpm > 255 ? 255 : bpm;
    T.tick_len = T.rate * 5 / (T.bpm * 2);
}

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

/* IT: an instrument can say that its note goes on sounding when a new
 * note comes on its channel -- carried on, let go, or fading out -- as
 * Impulse Tracker's "new note actions" have it.  Such a note is moved
 * here, keeps its pitch and its envelope, and is mixed until it falls
 * silent.  With all places taken, the quietest gives way.  mode is the
 * action: 1 carry on, 2 let go, 3 fade. */
static void to_background(int ch, Chan *c, int mode)
{
    int k, slot = 0, quiet = 0x7FFFFFFF;
    Chan *v;
    if (!c->on || !c->s || !c->in || !mode || !c->vol) return;
    for (k = 0; k < VOICES; k++) {
        int loud;
        if (!s_bg[k].on) { slot = k; break; }
        loud = s_bg[k].vol * (s_bg[k].fade >> 10) * s_bg[k].envval;
        if (loud < quiet) { quiet = loud; slot = k; }
    }
    v = &s_bg[slot];
    *v = *c;
    v->owner = ch;
    v->arp = v->vib = v->tvol = 0;
    v->cut_at = v->delay_at = -1; v->retrig = 0;
    if (mode >= 2) v->keyoff = 1;        /* let go, or faded: both fade here */
    c->on = 0;
}

/* Is v the same as a new note (by the new instrument's duplicate check)? */
static int duplicate(const Chan *v, const Instr *in, const Sample *s, int note)
{
    if (!v->on || !in || v->in != in || !in->dct) return 0;
    return in->dct == 3 || (in->dct == 1 && v->key == note) || (in->dct == 2 && v->s == s);
}

/* A new note on channel ch: what becomes of the notes already sounding there. */
static void new_note_on(int ch, Chan *c, const Note *n)
{
    Instr *in = n->ins && n->ins <= T.ninst ? &T.ins[n->ins - 1] : c->in;
    Sample *s = pick(in, n->note);
    int k;
    for (k = 0; k < VOICES; k++)
        if (s_bg[k].owner == ch && duplicate(&s_bg[k], in, s, n->note)) {
            if (in->dca == 0) s_bg[k].on = 0;
            else s_bg[k].keyoff = 1;
        }
    if (duplicate(c, in, s, n->note)) to_background(ch, c, in->dca == 0 ? 0 : in->dca + 1);
    else if (c->in) to_background(ch, c, c->in->nna);
}

static void background_tick(void)
{
    int k;
    for (k = 0; k < VOICES; k++) {
        Chan *v = &s_bg[k];
        if (!v->on) continue;
        envelope(v);
        if (v->keyoff && ((v->in->envtype & 1) || v->in->fades)) {
            v->fade -= v->in->fadeout;
            if (v->fade <= 0) { v->fade = 0; v->on = 0; }
        }
        /* An envelope that has ended at nothing is the end of the note. */
        if ((v->in->envtype & 1) && v->envval == 0 && !(v->in->envtype & 4) &&
            v->envtick > v->in->env[v->in->envn - 1][0]) v->on = 0;
    }
}

static void row_start(void)
{
    int i, pat = T.order[T.ord];

    if (pat >= T.npat) { T.ended = 1; return; }
    read_row(pat, T.row);
    for (i = 0; i < T.nch; i++) {
        Chan *c = &T.ch[i];
        Note *n = &s_row[i];
        int eff = n->eff, par = n->par, porta = (eff == 3 || eff == 5 || (n->vol >= 0xF0));
        c->arp = 0; c->vib = 0; c->tvol = 0;
        c->cut_at = -1; c->delay_at = -1; c->retrig = 0;

        if (eff == 0x0E && (par >> 4) == 0x0D && (par & 15)) {   /* note delay */
            c->delay_at = par & 15;
            c->delayed = *n;
            continue;
        }
        if (eff == 9) {                                           /* from an offset */
            if (par) c->ofs_par = par;
            c->offset = (T.fmt == F_IT ? c->hiofs * 65536 : 0) + c->ofs_par * 256;
        }
        if (n->ins && n->ins <= T.ninst && !(T.xm && n->note == NOTE_OFF)) {
            Instr *in = &T.ins[n->ins - 1];
            Sample *s = in->nsmp ? in->smp[0] : 0;
            if (T.xm && n->note && n->note <= 96) s = pick(in, n->note);
            else if (T.fmt >= F_S3M && c->s) s = c->s;
            if (s) c->vol = c->base_vol = s->vol;
            if (!n->note || porta) { c->in = in; c->keyoff = 0; c->fade = 65536; c->envtick = 0; }
        }
        if (n->note == NOTE_CUT) { c->vol = 0; c->on = 0; }
        else if (n->note == NOTE_OFF) key_off(c);
        else if (n->note) {
            if (porta && c->on && c->s) {
                Instr *in = n->ins && n->ins <= T.ninst ? &T.ins[n->ins - 1] : c->in;
                Sample *s = pick(in, n->note);
                if (!s) s = c->s;
                c->target = note_period(sounding(in, s, n->note), s->finetune);
            } else {
                if (T.fmt == F_IT) new_note_on(i, c, n);
                trigger(c, n->note, n->ins);
                c->target = 0;
            }
        }
        c->offset = 0;                     /* taken by trigger(), or not wanted */

        /* The volume column (XM's, and S3M's and IT's put as XM's). */
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
            if (par < 0x20) T.speed = par; else set_tempo(par);
            break;
        case 0x10: T.gvol = par > 64 ? 64 : par; break;          /* G */
        case 0x11: if (par) c->gvslide = par; break;             /* H */
        case 0x15: c->envtick = par; break;                      /* L */
        case 0x1B: if (par) c->mretrig_par = par; break;         /* R */
        case 0x21:                                               /* X */
            if ((par >> 4) == 1) c->period -= (par & 15);
            else if ((par >> 4) == 2) c->period += (par & 15);
            break;
        case E_SPEED: T.speed = par; break;
        case E_TEMPO: if (par >= 0x20) set_tempo(par); break;
        case E_BREAK: if (!T.jump) T.next_ord = T.ord + 1;
                      T.next_row = par; T.jump = 1; break;
        case E_CHVOL: c->cvol = par > 64 ? 64 : par; break;
        case E_HIOFS: c->hiofs = par; break;
        case E_VSL: {                                            /* D */
            int hi, lo;
            if (par) c->dmem = par;
            hi = c->dmem >> 4; lo = c->dmem & 15;
            c->dnorm = 0;
            if (hi == 15 && lo)      { c->vol -= lo; if (c->vol < 0) c->vol = 0; }    /* fine, now */
            else if (lo == 15 && hi) { c->vol += hi; if (c->vol > 64) c->vol = 64; }
            else { c->vslide = c->dmem; c->dnorm = 1; }          /* every tick but the first */
            break;
        }
        case E_PUP: case E_PDN: {                                /* F, E */
            int dir = eff == E_PUP ? -1 : 1;
            if (par) c->pmem = par;
            c->pslide = 0;
            if (c->pmem >= 0xF0)      c->period += dir * (c->pmem & 15) * 4;   /* fine, now */
            else if (c->pmem >= 0xE0) c->period += dir * (c->pmem & 15);       /* extra fine */
            else c->pslide = dir * c->pmem * 4;
            break;
        }
        }
    }
}

static void tick_effects(void)
{
    int i;
    for (i = 0; i < T.nch; i++) {
        Chan *c = &T.ch[i];
        Note *n = &s_row[i];
        int eff = n->eff, par = n->par, vc = n->vol;
        c->arp = 0; c->vib = 0;

        if (c->delay_at == T.tick) {
            Note *d = &c->delayed;
            if (d->note && d->note <= 96) {
                if (d->ins && d->ins <= T.ninst && T.ins[d->ins - 1].nsmp) c->vol = T.ins[d->ins - 1].smp[0]->vol;
                trigger(c, d->note, d->ins);
            } else if (d->note == NOTE_CUT) { c->vol = 0; c->on = 0; }
            else if (d->note == NOTE_OFF) key_off(c);
            if (d->vol >= 0x10 && d->vol <= 0x50) c->vol = d->vol - 0x10;
        }
        if (c->cut_at == T.tick) c->vol = 0;
        if (c->retrig && T.tick % c->retrig == 0 && c->s) { c->pos = 0; c->frac = 0; c->dir = 1; }
        if (eff == 0x14 && T.tick == par) key_off(c);

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
        case E_VSL: if (c->dnorm) vslide(c, c->vslide); break;
        case E_PUP: case E_PDN: c->period += c->pslide; break;
        case E_TEMPO:                                           /* IT: T0x down, T1x up */
            if (par < 0x20 && T.fmt == F_IT) set_tempo(T.bpm + ((par >> 4) ? (par & 15) : -(par & 15)));
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
    background_tick();
    for (i = 0; i < T.nch; i++) {
        Chan *c = &T.ch[i];
        envelope(c);
        if (c->keyoff && c->in && ((c->in->envtype & 1) || c->in->fades)) {
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
static s32 s_acc[2048];

/* One voice's n samples, added into s_acc, at volume v (0..4096).
 *
 * Most of the work is the stretch between a voice's loop points, done in
 * a tight loop with the position in registers and nothing to check.  The
 * samples where it meets a loop's end or the sample's end, or plays
 * backwards (a ping-pong loop), are done one at a time, carefully. */
static void mix_voice(Chan *c, int n, int v)
{
    Sample *s = c->s;
    const s8 *data = s->data;
    int i = 0;
    while (i < n && c->on) {
        if (c->dir > 0 && c->step < (1u << 20)) {
            u32 end = s->loop ? s->lstart + s->llen : s->len, cnt;
            if (c->pos + 2 <= end) {
                /* As many as keep pos + 1 inside, so no checks are needed. */
                u32 r = end - 2 - c->pos, pos = c->pos, frac = c->frac, step = c->step, k;
                s32 *acc = s_acc + i;
                cnt = r >= 0x8000 || !step ? (u32)(n - i) : (((r + 1) << 16) - 1 - frac) / step + 1;
                if (cnt > (u32)(n - i)) cnt = (u32)(n - i);
                for (k = 0; k < cnt; k++) {
                    s32 a = data[pos];
                    acc[k] += (a + (((data[pos + 1] - a) * (s32)(frac >> 1)) >> 15)) * v;
                    frac += step; pos += frac >> 16; frac &= 0xFFFF;
                }
                c->pos = pos; c->frac = frac;
                i += (int)cnt;
                if (pos >= end) {                          /* as one step would have */
                    if (s->loop == 2) { c->dir = -1; c->pos = end - 1 - (pos - end) % s->llen; }
                    else if (s->loop) c->pos = s->lstart + (pos - s->lstart) % s->llen;
                    else c->on = 0;
                }
                continue;
            }
        }
        {
            s32 a, b;
            u32 p = c->pos, q = p + 1;
            if (q >= s->len) q = s->loop ? s->lstart : p;
            a = data[p]; b = data[q];
            s_acc[i] += (a + (((b - a) * (s32)(c->frac >> 1)) >> 15)) * v;
            i++;
            if (c->dir > 0) {
                c->frac += c->step; c->pos += c->frac >> 16; c->frac &= 0xFFFF;
                if (s->loop && c->pos >= s->lstart + s->llen) {
                    if (s->loop == 2) {                /* ping-pong: turn back */
                        c->dir = -1;
                        c->pos = s->lstart + s->llen - 1 - (c->pos - (s->lstart + s->llen)) % s->llen;
                    } else
                        c->pos = s->lstart + (c->pos - s->lstart) % s->llen;
                } else if (!s->loop && c->pos >= s->len) c->on = 0;
            } else {
                u32 back;
                c->frac += c->step; back = c->frac >> 16; c->frac &= 0xFFFF;
                if (c->pos < s->lstart + back) { c->dir = 1; c->pos = s->lstart; }
                else c->pos -= back;
            }
        }
    }
}

/* A voice too quiet to move an 8-bit sample: it only moves on. */
static void skip_voice(Chan *c, int n)
{
    Sample *s = c->s;
    u32 t = c->frac + (u32)n * (c->step < (1u << 20) ? c->step : 1u << 20);
    c->pos += t >> 16; c->frac = t & 0xFFFF;
    if (c->dir < 0) { c->dir = 1; c->pos = s->lstart; }  /* near enough, for silence */
    if (s->loop && s->llen && c->pos >= s->lstart + s->llen) c->pos = s->lstart + (c->pos - s->lstart) % s->llen;
    else if (!s->loop && c->pos >= s->len) c->on = 0;
}

/* A voice's volume, 0..4096, with everything that makes it. */
static int loudness(const Chan *c)
{
    int v = c->vol + c->tvol;
    if (v < 0) v = 0;
    if (v > 64) v = 64;
    v = v * c->envval * T.gvol / 64;              /* 0..4096             */
    v = (int)(((s32)v * (c->fade >> 4)) >> 12);   /* and fading out      */
    v = v * c->s->gv * c->cvol / 4096;            /* IT's sample and channel volumes */
    if (c->in) v = v * c->in->gv / 64;            /* and its instrument's */
    return v;
}

/* Voices mixed at once, at most: the loudest.  A song of 64 channels,
 * with notes ringing on under new ones, could otherwise take a quarter of
 * the machine; the rest keep their place, silently, until they are among
 * the loudest again. */
#define AUDIBLE 20

static void mix(u8 *out, int n)
{
    /* One channel at full volume is half the speaker's range for a
     * 4-channel song, less for more channels, so they can add up. */
    int i, k, m = 0, total = 0, cut = 0, hist[65];
    int shift = T.nch <= 4 ? 13 : T.nch <= 16 ? 12 : 14, third = T.nch > 4 && T.nch <= 16;
    int div = 4096 * (T.nch <= 4 ? 2 : T.nch <= 16 ? 3 : 4);
    Chan *all[CHANS + VOICES];
    int loud[CHANS + VOICES];

    for (k = 0; k < T.nch; k++) if (T.ch[k].on && T.ch[k].s && T.ch[k].s->len) all[m++] = &T.ch[k];
    if (T.fmt == F_IT) for (k = 0; k < VOICES; k++) if (s_bg[k].on && s_bg[k].s && s_bg[k].s->len) all[m++] = &s_bg[k];
    for (k = 0; k < m; k++) loud[k] = loudness(all[k]);
    if (m > AUDIBLE) {
        for (k = 0; k <= 64; k++) hist[k] = 0;
        for (k = 0; k < m; k++) hist[loud[k] >> 6]++;
        for (k = 64; k >= 0; k--) {
            if (total + hist[k] > AUDIBLE && total) break;
            total += hist[k];
        }
        cut = (k + 1) << 6;
    }
    for (i = 0; i < n; i++) s_acc[i] = 0;
    for (k = 0; k < m; k++) {
        if (loud[k] * 256 < div || loud[k] < cut) skip_voice(all[k], n);   /* under half a step, or crowded out */
        else mix_voice(all[k], n, loud[k]);
    }
    for (i = 0; i < n; i++) {
        s32 v = s_acc[i] >> shift;                     /* / div, without dividing */
        if (third) v = (v * 21845) >> 16;
        if (v > 127) v = 127;
        if (v < -128) v = -128;
        out[i] = (u8)(v + 128);
    }
}

/* ---- the outside ------------------------------------------------------------- */
static void fresh(u8 *data)
{
    int i;
    for (i = 0; i < (int)sizeof T; i++) ((u8 *)&T)[i] = 0;
    for (i = 0; i < (int)sizeof R; i++) ((u8 *)&R)[i] = 0;
    for (i = 0; i < (int)sizeof s_bg; i++) ((u8 *)s_bg)[i] = 0;
    for (i = 0; i < SAMPLES; i++) T.smp[i].gv = 64;
    for (i = 0; i < INSTRS; i++) T.ins[i].gv = 64;
    for (i = 0; i < CHANS; i++) T.ch[i].cvol = 64;
    R.d = data; R.pat = -1;
}

int tracker_load(u8 *data, u32 len, u32 room, int rate)
{
    int i, ok;
    tables();
    if (room < len) room = len;
    fresh(data); ok = load_xm(data, len);
    if (!ok) { fresh(data); ok = load_s3m(data, len); }
    if (!ok) { fresh(data); ok = load_it(data, len, room); }
    if (!ok) { fresh(data); ok = load_mod(data, len); }
    if (!ok || T.nord < 1) return 0;
    T.rate = rate;
    if (!T.gvol && T.fmt < F_S3M) T.gvol = 64;
    T.tick_len = T.rate * 5 / (T.bpm * 2);
    T.tick_left = 0;
    for (i = 0; i < CHANS; i++) { T.ch[i].fade = 65536; T.ch[i].envval = 64; }
    T.playing = 1;
    return 1;
}

void tracker_rate(int rate)
{
    if (rate < 4000) return;
    T.rate = rate;
    T.tick_len = T.rate * 5 / (T.bpm * 2);
    if (T.tick_left > T.tick_len) T.tick_left = T.tick_len;
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
const char *tracker_kind(void)  { return T.fmt == F_IT ? "IT" : T.fmt == F_S3M ? "S3M" : T.fmt == F_XM ? "XM" : "MOD"; }
int tracker_channels(void)      { return T.nch; }
int tracker_position(int *ord, int *nord) { *ord = T.ord; *nord = T.nord; return T.row; }
u32 tracker_samples(void)       { return T.played; }
