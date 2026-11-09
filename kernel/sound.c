/* ===========================================================================
 * sound.c -- a step sequencer driving the PC speaker, and the OPL2 as well
 * if the machine has one.
 *
 * The Satellite 430CDT has a Sound Blaster Pro compatible chip, so the
 * Adlib detection below usually succeeds and you get three-voice FM.  On a
 * machine with nothing but a beeper it degrades to the beeper, which for
 * an acid bassline is honestly not a downgrade.
 *
 * Everything here is driven from the 250Hz timer interrupt, and the beat
 * level it publishes in g_beat / g_bar is what makes the visuals pulse in
 * time with what you are hearing.
 * ===========================================================================
 */
#include "flopos.h"
#include "io.h"

#define PIT_CH2  0x42
#define PIT_CMD  0x43
#define PORT_61  0x61

#define OPL_ADDR 0x388
#define OPL_DATA 0x389

volatile u8  g_beat;
volatile u8  g_bar;
volatile u32 g_step;

static int s_on = 1;
static int s_fm;
static int s_bpm = 124;
static int s_pat;
static int s_tick_in_step;
static int s_ticks_per_step;
static int s_gate_left;
static int s_slide_to, s_slide_cur;
static int s_sweeping;

/* --- note tables, built once ------------------------------------------- */
static u16 s_div[128];                   /* PIT divisor per MIDI note      */
static u16 s_fnum[128];                  /* OPL F-number                   */
static u8  s_block[128];                 /* OPL block (octave)             */

/* One octave of equal temperament, frequency in Hz * 16, starting at C5. */
static const u16 s_oct[12] = {
    8372, 8870, 9397, 9956, 10548, 11175,
    11840, 12544, 13290, 14080, 14917, 15804
};

/* --- patterns ----------------------------------------------------------
 * Three lanes per pattern, all sixteen-step: a bass line, a lead line, and
 * a drum mask.  Notes are MIDI numbers with 0 for a rest.  The OPL2 plays
 * all three at once; the PC speaker, being one square wave, plays the lead
 * where there is one and the bass otherwise.
 */
#define DR_BD  0x10                      /* the same bits the chip wants   */
#define DR_SD  0x08
#define DR_TOM 0x04
#define DR_CYM 0x02
#define DR_HH  0x01

typedef struct {
    const char *name;
    u8  div;                             /* step multiplier                */
    u8  gate;                            /* how much of a step sounds, /16 */
    u8  bpm;
    u8  bass[64];
    u8  lead[64];
    u8  drum[64];
    /* These two are last on purpose: every pattern written before they
     * existed still compiles, and reads as the old defaults. */
    u8  len;                             /* 0 means 32 steps               */
    u8  subdiv;                          /* 0 means 4, sixteenths; 8 is
                                          * thirty-seconds, which is what
                                          * a rolling break needs          */
} Pattern;

static int pat_len(const Pattern *p)    { return p->len ? p->len : 32; }
static int pat_subdiv(const Pattern *p) { return p->subdiv ? p->subdiv : 4; }

#define R 0
#define _ 0
static const Pattern s_pats[] = {

 /* Original jungle.  Two bars at thirty-second resolution, 170 to the
  * minute: the kick never lands where the ear expects twice running, the
  * snare is answered by ghosts half a step late, and the hats carry the
  * roll between them.  The bass is a sub that holds under all of it and
  * moves four times in sixty-four steps, because the drums are already
  * doing the work and a busy bassline on top turns it to mud. */
 { "JUNGLE", 1, 16, 170,
   /* bass -- long, low, left to ring */ {
    28, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _,
    _,  _, _, _, _, _, _, _, 26, _, _, _, _, _, _, _,
    31, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _,
    _,  _, _, _, _, _, _, _, 28, _, _, _, _, _, 33, _ },
   /* lead -- stabs, placed where the drums are not */ {
    _,  _, _, _, _, _, _, 76, _, _, _, _, _, _, _, _,
    _,  _, _, _, _, 74, _, _, _, _, _, _, _, _, _, 72,
    _,  _, _, _, _, _, _, 76, _, _, _, _, _, _, _, _,
    _,  _, _, 79, _, _, _, _, _, _, 76, _, _, _, _, 74 },
   /* drum */ {
    DR_BD|DR_HH, _, DR_HH, _, _, _, DR_BD, _,
    DR_SD|DR_HH, _, DR_HH, DR_SD, _, _, DR_BD, DR_HH,
    _, _, DR_HH, _, DR_BD|DR_HH, _, _, DR_TOM,
    DR_SD|DR_HH, _, DR_HH, _, DR_BD, DR_BD, DR_HH, _,

    DR_BD|DR_HH, _, DR_HH, _, _, DR_SD, DR_BD, _,
    DR_SD|DR_HH, _, DR_HH, _, DR_BD, _, DR_HH, DR_TOM,
    _, _, DR_BD|DR_HH, _, _, _, DR_SD|DR_HH, DR_SD,
    _, DR_HH, DR_BD, _, DR_SD|DR_HH, DR_TOM, DR_HH, DR_CYM },
   64, 8 },

 /* An original big-beat tune: breakbeat kit, a syncopated minor bass
  * figure that keeps falling off the downbeat, and stabs a sixteenth
  * late so the whole thing leans forward. Nothing here is transcribed
  * from anything; it is written to sit in that lane. */
 { "BREAKS", 1, 6, 134,
   /* bass */ {
    45, _, 45, _, 43, _, 45, _, 48, _, 45, _, 40, _, 43, _,
    45, _, 45, _, 43, _, 45, _, 50, _, 48, _, 47, _, 45, 43 },
   /* lead */ {
     _, _, _, 69, _, _, _, _, _, 67, _, _, _, _, 69, _,
     _, _, _, 69, _, _, _, 72, _, 71, _, _, _, 69, _, 67 },
   /* drum */ {
    DR_BD|DR_HH, _, DR_HH, DR_BD, DR_SD|DR_HH, _, DR_HH, DR_BD,
    DR_HH, _, DR_BD|DR_HH, _, DR_SD|DR_HH, _, DR_HH, DR_HH,
    DR_BD|DR_HH, _, DR_HH, DR_BD, DR_SD|DR_HH, _, DR_HH, DR_BD,
    DR_HH, DR_TOM, DR_BD|DR_HH, _, DR_SD|DR_HH, DR_TOM, DR_HH, DR_CYM }, 32, 4 },

 { "DRIFT", 2, 14, 96,
   { 45, _, 52, _, 57, _, 52, _, 50, _, 57, _, 60, _, 57, _,
     43, _, 50, _, 55, _, 50, _, 48, _, 55, _, 59, _, 55, _ },
   { 69, _, _, _, 72, _, _, _, 76, _, _, _, 72, _, _, _,
     67, _, _, _, 71, _, _, _, 74, _, _, _, 71, _, _, _ },
   { DR_BD, _, _, _, _, _, _, _, DR_CYM, _, _, _, _, _, _, _,
     DR_BD, _, _, _, _, _, _, _, DR_CYM, _, _, _, _, _, _, _ }, 32, 4 },

 { "ACID", 1, 7, 128,
   { 33, 33, 45, 33, 36, 33, 45, 40, 33, 33, 48, 33, 36, 45, 33, 40,
     31, 31, 43, 31, 34, 31, 43, 38, 31, 43, 46, 31, 34, 43, 31, 38 },
   { _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _,
     _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _ },
   { DR_BD|DR_HH, _, DR_HH, _, DR_SD|DR_HH, _, DR_HH, _,
     DR_BD|DR_HH, _, DR_HH, _, DR_SD|DR_HH, _, DR_HH, _,
     DR_BD|DR_HH, _, DR_HH, _, DR_SD|DR_HH, _, DR_HH, _,
     DR_BD|DR_HH, _, DR_HH, _, DR_SD|DR_HH, _, DR_HH, DR_HH }, 32, 4 },

 { "RAGA", 1, 11, 112,
   { 48, _, _, _, 48, _, _, _, 46, _, _, _, 46, _, _, _,
     44, _, _, _, 44, _, _, _, 46, _, _, _, 48, _, _, _ },
   { 60, 63, 65, 67, 70, 67, 65, 63, 60, 63, 65, 67, 72, 70, 67, 65,
     58, 61, 63, 65, 68, 65, 63, 61, 58, 61, 63, 65, 70, 68, 65, 63 },
   { DR_BD, _, _, _, _, _, DR_TOM, _, _, _, DR_BD, _, _, _, DR_TOM, _,
     DR_BD, _, _, _, _, _, DR_TOM, _, _, _, DR_BD, _, _, _, DR_TOM, DR_CYM }, 32, 4 },

 { "TEMPLE", 2, 15, 80,
   { 36, _, _, _, 36, _, _, _, 34, _, _, _, 34, _, _, _,
     36, _, _, _, 36, _, _, _, 31, _, _, _, 31, _, _, _ },
   { 60, _, _, _, _, _, _, _, 67, _, _, _, _, _, _, _,
     58, _, _, _, _, _, _, _, 65, _, _, _, _, _, _, _ },
   { DR_BD, _, _, _, _, _, _, _, _, _, _, _, DR_CYM, _, _, _,
     DR_BD, _, _, _, _, _, _, _, _, _, _, _, DR_CYM, _, _, _ }, 32, 4 },

 { "SPIRAL", 1, 6, 150,
   { 36, _, 36, _, 43, _, 36, _, 41, _, 36, _, 38, _, 31, _,
     38, _, 38, _, 45, _, 38, _, 43, _, 38, _, 40, _, 33, _ },
   { 72, 67, 64, 60, 67, 64, 60, 55, 64, 60, 55, 52, 60, 55, 52, 48,
     74, 69, 65, 62, 69, 65, 62, 57, 65, 62, 57, 53, 62, 57, 53, 50 },
   { DR_BD|DR_HH, DR_HH, DR_HH, DR_HH, DR_SD|DR_HH, DR_HH, DR_HH, DR_HH,
     DR_BD|DR_HH, DR_HH, DR_HH, DR_HH, DR_SD|DR_HH, DR_HH, DR_HH, DR_HH,
     DR_BD|DR_HH, DR_HH, DR_HH, DR_HH, DR_SD|DR_HH, DR_HH, DR_HH, DR_HH,
     DR_BD|DR_HH, DR_HH, DR_HH, DR_HH, DR_SD|DR_HH, DR_HH, DR_CYM, DR_HH }, 32, 4 },
};
#undef R
#undef _
#define PAT_COUNT ((int)(sizeof s_pats / sizeof s_pats[0]))

/* --- OPL2 register access ---------------------------------------------
 * The chip needs settling time after the address and after the data.  A
 * read of its own status port is the traditional way to burn it, and takes
 * about a microsecond on any ISA bus.
 */
static void opl_delay(int n)
{
    while (n--) (void)inb(OPL_ADDR);
}

static void opl_write(u8 reg, u8 val)
{
    outb(OPL_ADDR, reg);
    opl_delay(6);
    outb(OPL_DATA, val);
    opl_delay(35);
}

static int opl_detect(void)
{
    u8 s1, s2;

    opl_write(0x04, 0x60);               /* mask both timers               */
    opl_write(0x04, 0x80);               /* reset the IRQ flags            */
    s1 = inb(OPL_ADDR);
    opl_write(0x02, 0xFF);               /* timer 1 to its shortest        */
    opl_write(0x04, 0x21);               /* and start it                   */
    opl_delay(200);                      /* ~200us, it expires in 80       */
    s2 = inb(OPL_ADDR);
    opl_write(0x04, 0x60);
    opl_write(0x04, 0x80);

    return ((s1 & 0xE0) == 0x00) && ((s2 & 0xE0) == 0xC0);
}

/* Operator register offsets for channels 0..8. */
static const u8 s_op1[9] = { 0x00, 0x01, 0x02, 0x08, 0x09, 0x0A, 0x10, 0x11, 0x12 };

static void opl_voice(int ch, u8 mult1, u8 lvl1, u8 ad1, u8 sr1, u8 wav1,
                              u8 mult2, u8 lvl2, u8 ad2, u8 sr2, u8 wav2,
                              u8 fb)
{
    u8 o1 = s_op1[ch], o2 = (u8)(s_op1[ch] + 3);
    opl_write((u8)(0x20 + o1), mult1);
    opl_write((u8)(0x40 + o1), lvl1);
    opl_write((u8)(0x60 + o1), ad1);
    opl_write((u8)(0x80 + o1), sr1);
    opl_write((u8)(0xE0 + o1), wav1);
    opl_write((u8)(0x20 + o2), mult2);
    opl_write((u8)(0x40 + o2), lvl2);
    opl_write((u8)(0x60 + o2), ad2);
    opl_write((u8)(0x80 + o2), sr2);
    opl_write((u8)(0xE0 + o2), wav2);
    opl_write((u8)(0xC0 + ch), fb);
}

static void opl_note_on(int ch, int note)
{
    if (note < 0) note = 0;
    if (note > 127) note = 127;
    opl_write((u8)(0xA0 + ch), (u8)(s_fnum[note] & 0xFF));
    opl_write((u8)(0xB0 + ch),
              (u8)(0x20 | (s_block[note] << 2) | (s_fnum[note] >> 8)));
}

static void opl_note_off(int ch)
{
    opl_write((u8)(0xB0 + ch), 0x00);
}

/* ---- the kit ----------------------------------------------------------
 * In rhythm mode channels 6, 7 and 8 stop being melodic voices.  Channel
 * 6's operator pair becomes the bass drum, and the four operators of 7 and
 * 8 become hi-hat, snare, tom and cymbal -- one operator each, which is
 * why they are tuned by setting the channel frequency rather than a note.
 *
 * Percussion is keyed from register 0xBD, not from the usual key-on bit,
 * and a drum only retriggers on a rising edge -- so every step clears the
 * bits before setting them.
 */
#define RHY_ON   0x20

#define OP_BD1   0x10
#define OP_BD2   0x13
#define OP_HH    0x11
#define OP_SD    0x14
#define OP_TOM   0x12
#define OP_CYM   0x15

static void opl_op(u8 op, u8 mult, u8 level, u8 ad, u8 sr)
{
    opl_write((u8)(0x20 + op), mult);
    opl_write((u8)(0x40 + op), level);
    opl_write((u8)(0x60 + op), ad);
    opl_write((u8)(0x80 + op), sr);
    opl_write((u8)(0xE0 + op), 0x00);
}

static void opl_rhythm_init(void)
{
    opl_write(0xBD, 0x00);               /* quiet while we set up          */

    opl_op(OP_BD1, 0x01, 0x0A, 0xF8, 0xF7);
    opl_op(OP_BD2, 0x01, 0x00, 0xF8, 0xF7);
    opl_write(0xC6, 0x00);

    opl_op(OP_HH,  0x01, 0x1A, 0xD8, 0xF9);   /* well back     */
    opl_op(OP_SD,  0x01, 0x06, 0xD8, 0xF7);
    opl_op(OP_TOM, 0x05, 0x08, 0xF8, 0xF7);
    opl_op(OP_CYM, 0x01, 0x16, 0xD8, 0xF8);

    /* Pitches for the kit.  Key-on bits stay clear: 0xBD does that job. */
    opl_write(0xA6, 0x20); opl_write(0xB6, 0x09);
    opl_write(0xA7, 0x05); opl_write(0xB7, 0x0D);
    opl_write(0xA8, 0x05); opl_write(0xB8, 0x09);

    opl_write(0xBD, RHY_ON);
}

static void opl_drums(u8 mask)
{
    opl_write(0xBD, RHY_ON);                      /* falling edge         */
    if (mask) opl_write(0xBD, (u8)(RHY_ON | (mask & 0x1F)));
}

/* --- PC speaker -------------------------------------------------------- */
static void spk_on(u16 divisor)
{
    if (divisor < 2) divisor = 2;
    outb(PIT_CMD, 0xB6);                 /* ch2, lo/hi, square wave        */
    outb(PIT_CH2, (u8)(divisor & 0xFF));
    outb(PIT_CH2, (u8)(divisor >> 8));
    outb(PORT_61, (u8)(inb(PORT_61) | 0x03));
}

static void spk_off(void)
{
    outb(PORT_61, (u8)(inb(PORT_61) & ~0x03));
}

/* --- setup ------------------------------------------------------------- */
static void build_notes(void)
{
    int n;

    for (n = 0; n < 128; n++) {
        int oct = n / 12;                /* MIDI note 60 is C5 here        */
        int sem = n % 12;
        u32 fx16 = s_oct[sem];           /* Hz * 16 at octave 5            */
        int blk;

        /* Shift into the right octave.  MIDI 60 = C5 in this numbering. */
        if (oct >= 5) fx16 <<= (oct - 5);
        else          fx16 >>= (5 - oct);
        if (fx16 < 16) fx16 = 16;

        s_div[n] = (u16)(19090912u / fx16 > 65535u ? 65535u : 19090912u / fx16);

        blk = oct - 1;
        if (blk < 0) blk = 0;
        if (blk > 7) blk = 7;
        s_block[n] = (u8)blk;
        /* fnum = freq * 2^(20-block) / 49716, with freq held as Hz*16, so
         * the divisor carries the extra 16.  Narrow to 32 bits before
         * dividing: a 64-bit divide would want __udivdi3 and there is no
         * libgcc here.  Anything that saturates is a note far above the
         * chip's range, and gets clamped on the next line regardless. */
        u64 num = (u64)fx16 << (20 - blk);
        if (num > 0xFFFFFFFFull) num = 0xFFFFFFFFull;
        s_fnum[n] = (u16)((u32)num / 795456u);
        if (s_fnum[n] > 1023) s_fnum[n] = 1023;
    }
}

static void recalc_tempo(void)
{
    /* ticks per step = TICK_HZ * 60 / (bpm * steps-per-beat) */
    int t = (TICK_HZ * 60) / (s_bpm * pat_subdiv(&s_pats[s_pat]));
    if (t < 2) t = 2;
    s_ticks_per_step = t;
}

void snd_init(void)
{
    build_notes();
    recalc_tempo();

    s_fm = opl_detect();
    if (s_fm) {
        opl_write(0x01, 0x20);           /* waveform select enabled        */
        opl_write(0x08, 0x00);           /* no note-select split           */


        /* ch0: bass, square-ish, short decay */
        opl_voice(0, 0x01, 0x10, 0xF8, 0x57, 0x01,
                     0x01, 0x00, 0xF6, 0x36, 0x00, 0x08);
        /* ch1: lead, bright with feedback */
        opl_voice(1, 0x21, 0x1A, 0xF4, 0x38, 0x02,
                     0x01, 0x05, 0xE8, 0x28, 0x00, 0x0C);
        /* ch2: a fifth above, soft, long release for the drone */
        opl_voice(2, 0x31, 0x24, 0x93, 0x15, 0x03,
                     0x11, 0x08, 0x85, 0x15, 0x00, 0x04);

        opl_rhythm_init();
    }
    snd_set_tempo(s_pats[s_pat].bpm);
    g_beat = 0;
    g_bar = 0;
}

int snd_has_fm(void) { return s_fm; }
int snd_enabled(void) { return s_on; }
int snd_tempo(void) { return s_bpm; }
int snd_pattern(void) { return s_pat; }
const char *snd_pattern_name(void) { return s_pats[s_pat].name; }

void snd_set_tempo(int bpm)
{
    if (bpm < 50) bpm = 50;
    if (bpm > 220) bpm = 220;
    s_bpm = bpm;
    recalc_tempo();
}

void snd_silence(void)
{
    spk_off();
    if (s_fm) {
        opl_note_off(0); opl_note_off(1); opl_note_off(2);
        opl_write(0xBD, RHY_ON);
    }
}

void snd_toggle(void)
{
    s_on = !s_on;
    if (!s_on) snd_silence();
}

void snd_next_pattern(void)
{
    s_pat = (s_pat + 1) % PAT_COUNT;
    s_tick_in_step = 0;
    g_step = 0;
    snd_silence();
    snd_set_tempo(s_pats[s_pat].bpm);    /* each tune has its own pace     */
}

/* --- an effect borrows the hardware -----------------------------------
 * The sequencer stands down entirely while this is running, because two
 * things writing the OPL's address latch at once -- one of them from an
 * interrupt -- would interleave into nonsense.
 */
void snd_sweep_begin(void)
{
    s_sweeping = 1;
    spk_off();
    if (s_fm) {
        opl_note_off(0); opl_note_off(1); opl_note_off(2);
        opl_write(0xBD, RHY_ON);
        /* A bright, buzzing voice with feedback: it has to carry a slide
         * across four octaves without thinning out. */
        opl_voice(1, 0x21, 0x08, 0xF2, 0x15, 0x02,
                     0x01, 0x00, 0xF2, 0x15, 0x03, 0x0E);
    }
}

void snd_sweep_set(int hz)
{
    u32 div;

    if (!s_sweeping) return;
    if (hz < 40) hz = 40;
    if (hz > 6000) hz = 6000;

    div = 1193182u / (u32)hz;
    if (div > 65535) div = 65535;
    if (s_on) spk_on((u16)div);

    if (s_fm && s_on) {
        /* fnum at block 4 is freq * 2^16 / 49716 */
        u32 fn = ((u32)hz * 65536u) / 49716u;
        if (fn > 1023) fn = 1023;
        opl_write(0xA1, (u8)(fn & 0xFF));
        opl_write(0xB1, (u8)(0x20 | (4 << 2) | (fn >> 8)));
    }
}

void snd_sweep_end(void)
{
    spk_off();
    if (s_fm) {
        opl_note_off(1);
        /* Put the lead voice back the way the sequencer expects it. */
        opl_voice(1, 0x21, 0x1A, 0xF4, 0x38, 0x02,
                     0x01, 0x05, 0xE8, 0x28, 0x00, 0x0C);
    }
    s_sweeping = 0;
}

/* --- effects: chip-tune notes and bells -------------------------------- *
 * For the way into cyberdelia.  The sequencer steps aside, as it does for
 * the sweep, and the effects get three FM channels the music never uses.
 *
 * A square note is the PC speaker, which is a genuine square wave -- the
 * same kind of sound a Game Boy makes -- doubled on FM with a buzzing,
 * sustained patch for body.  A bell is FM only: a fast attack, a long
 * decay and a little vibrato, two channels taking turns so each one rings
 * on under the next.  Without FM a bell is the speaker, and whoever rang
 * it stops it again.  `quiet` silences all of it.
 */
static int s_bell_ch;

static void opl_hz(int ch, int hz, int on)
{
    u32 fn = 1023;
    int block;

    if (hz < 20) hz = 20;
    if (hz > 4000) hz = 4000;
    for (block = 0; block < 8; block++) {
        fn = ((u32)hz << (20 - block)) / 49716u;
        if (fn < 1024) break;
    }
    if (block > 7) { block = 7; fn = 1023; }
    opl_write((u8)(0xA0 + ch), (u8)(fn & 0xFF));
    opl_write((u8)(0xB0 + ch), (u8)((on ? 0x20 : 0) | (block << 2) | (fn >> 8)));
}

void snd_fx_begin(void)
{
    s_sweeping = 1;
    spk_off();
    s_bell_ch = 0;
    if (s_fm) {
        opl_note_off(0); opl_note_off(1); opl_note_off(2);
        /* 3: square-ish, sustained while held. */
        opl_voice(3, 0x21, 0x14, 0xF0, 0x0F, 0x00,
                     0x21, 0x04, 0xF0, 0x0F, 0x00, 0x0E);
        /* 4, 5: bells. */
        opl_voice(4, 0x47, 0x1A, 0xF3, 0x05, 0x00,
                     0x41, 0x02, 0xF2, 0x04, 0x00, 0x04);
        opl_voice(5, 0x47, 0x1A, 0xF3, 0x05, 0x00,
                     0x41, 0x02, 0xF2, 0x04, 0x00, 0x04);
    }
}

void snd_fx_square(int hz)
{
    if (!s_sweeping) return;
    if (hz <= 0 || !s_on) {
        spk_off();
        if (s_fm) opl_hz(3, 440, 0);
        return;
    }
    spk_on((u16)(1193182u / (u32)hz));
    if (s_fm) { opl_hz(3, hz, 0); opl_hz(3, hz, 1); }
}

void snd_fx_bell(int hz)
{
    if (!s_sweeping || !s_on || hz <= 0) return;
    if (s_fm) {
        int ch = 4 + s_bell_ch;
        s_bell_ch ^= 1;
        opl_hz(ch, hz, 0);               /* off first, so it strikes again */
        opl_hz(ch, hz, 1);
    } else
        spk_on((u16)(1193182u / (u32)hz));
}

void snd_fx_end(void)
{
    spk_off();
    if (s_fm) {
        opl_hz(3, 440, 0);
        opl_hz(4, 440, 0);               /* the bells ring out on release   */
        opl_hz(5, 440, 0);
    }
    s_sweeping = 0;
}

/* --- the sequencer, called at TICK_HZ ---------------------------------- */
void snd_tick(void)
{
    if (s_sweeping) return;

    const Pattern *p = &s_pats[s_pat];
    int step_len, idx, bass, lead, voice;
    u8  drum;

    /* Beat envelopes decay whether or not the sound is on, so the visuals
     * keep pulsing for anyone running this silently. */
    g_beat = (u8)(g_beat > 12 ? g_beat - 12 : 0);
    g_bar  = (u8)(g_bar  > 3  ? g_bar  - 3  : 0);

    /* A slide towards the target note, which is what gives a bass line its
     * character on something as blunt as a square wave. */
    if (s_slide_cur != s_slide_to) {
        int d = s_slide_to - s_slide_cur;
        s_slide_cur += (d > 0) ? ((d >> 3) + 1) : ((d >> 3) - 1);
        if (s_on && s_gate_left > 0) spk_on((u16)s_slide_cur);
    }

    if (s_gate_left > 0 && --s_gate_left == 0) {
        spk_off();
        if (s_fm) { opl_note_off(1); }
    }

    step_len = s_ticks_per_step * p->div;
    if (++s_tick_in_step < step_len) return;
    s_tick_in_step = 0;

    g_step++;
    idx  = (int)(g_step % (u32)pat_len(p));
    bass = p->bass[idx];
    lead = p->lead[idx];
    drum = p->drum[idx];

    /* --- variation --------------------------------------------------------
     * A dense figure that repeats exactly every four seconds stops being
     * complex and becomes irritating -- a simple loop survives repetition,
     * an intricate one does not.  So the written pattern is treated as a
     * seed rather than as the music.  What actually plays is that seed put
     * through a mutation keyed to which pass we are on: hats thinned
     * differently each time round, ghosts appearing where none were
     * written, the root moving on a longer cycle than the bar, and the kit
     * falling away entirely for half a pass every so often.
     *
     * It is all deterministic, so it is a composition and not a random
     * number generator with a drum kit.  Nothing repeats exactly for
     * sixteen passes, which is about a minute and a quarter, and the
     * shape of the figure survives the whole way.
     */
    if (pat_subdiv(p) == 8) {
        static const s8 roots[8] = { 0, 0, -2, 0, 3, 0, -4, 5 };
        u32 cycle = g_step / (u32)pat_len(p);
        int sect  = (int)((cycle >> 2) & 3);
        u32 h = (cycle * 2654435761u) ^ ((u32)idx * 40503u);
        h ^= h >> 13; h *= 1274126177u; h ^= h >> 16;

        if ((drum & DR_HH) && (h & 3) == 0) drum &= (u8)~DR_HH;
        if (!drum && (h & 63) == 0) drum |= DR_SD;
        if (!drum && ((h >> 6) & 127) == 0) drum |= DR_BD;

        switch (sect) {
        case 1:                          /* leaner: fewer kicks, no lead  */
            if ((drum & DR_BD) && ((h >> 13) & 3) == 0) drum &= (u8)~DR_BD;
            lead = 0;
            break;
        case 2:                          /* busier: toms creep in         */
            if (!drum && ((h >> 15) & 31) == 0) drum |= DR_TOM;
            break;
        case 3:                          /* the break: kit drops out      */
            if (idx < pat_len(p) / 2) drum &= (u8)DR_HH;
            break;
        default:
            break;
        }

        if (bass) bass += roots[cycle & 7];
        if (bass < 1) bass = 1;
    }

    /* The beat meter follows the kick, not the grid, so what the screen
     * does lines up with what you actually hear. */
    if (drum & DR_BD) g_beat = 255;
    else if (drum) g_beat = 160;
    if ((g_step % (u32)(pat_len(p) / 2)) == 0) g_bar = 255;

    if (!s_on) return;

    if (s_fm) {
        opl_drums((u8)drum);
        if (bass) opl_note_on(0, bass);
        if (lead) opl_note_on(1, lead);
        if (lead && (idx & 15) == 3) opl_note_on(2, lead - 5);
    }

    /* One square wave cannot play three parts, so it takes the lead where
     * there is one and holds down the bass the rest of the time. */
    voice = lead ? lead : bass;
    if (!voice) return;

    s_gate_left = (step_len * p->gate) >> 4;
    if (s_gate_left < 1) s_gate_left = 1;

    s_slide_to = s_div[voice];
    if (s_slide_cur == 0) s_slide_cur = s_slide_to;
    spk_on((u16)s_slide_cur);
}
