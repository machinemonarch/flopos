/* ===========================================================================
 * audio.c -- recorded sound, through the Sound Blaster's sample player.
 *
 * The FM chip makes notes; this plays samples -- a song from a module, an
 * mp3, the radio.  The Sound Blaster has a DSP that reads 8-bit samples
 * from memory by itself (DMA, channel 1) at a steady rate, and here it is
 * set looping around one 16 KB buffer forever.
 *
 * Nothing waits on the card's interrupt.  Instead the system timer, 250
 * times a second, asks the DMA controller where in the buffer the card is
 * reading now, and fills the part it has just finished with what apps
 * have queued, or silence when nothing is.  That way it does not matter
 * which IRQ the laptop's card was given, and a busy app only has to keep
 * the queue topped up: the queue holds about six seconds.
 *
 * Apps get it through the app interface (audio_open, audio_write, and so
 * on).  8-bit unsigned mono, at whatever rate the card manages nearest to
 * the one asked for.  While it plays, the FM music steps aside.
 *
 * The same card listens, too (audio_listen, audio_heard), though never
 * while it plays: the loop runs the other way, the card writing what it
 * hears into the buffer and the timer taking it out into the queue.  That
 * is how a cassette is read back.
 * ===========================================================================
 */
#include "flopos.h"
#include "io.h"

#define DMA_LEN   16384                  /* the card's loop                */
#define RING_LEN  131072                 /* what apps have queued          */
#define GUARD     64                     /* stay this far behind the card  */

static u16 s_base;                       /* where the DSP answers, or 0    */
static int s_probed;
static u8  s_ver_hi, s_ver_lo;
static u8 *s_dma;
static u8 *s_ring;
static volatile u32 s_head, s_tail;      /* apps write at head, the timer reads at tail */
static volatile u32 s_fill;              /* where in s_dma the timer writes next */
static volatile int s_on;                /* 1 playing, 2 listening         */
static u32 s_rate;
static const u8 *s_hit;                  /* gibson's last hit, and where it went */
static u32 s_hit_at, s_hit_n;
static void (*volatile s_feeder)(void);  /* an app's refill routine        */
static volatile int s_feeding;
static int s_source;                     /* listening to: 1 line-in, 0 the microphone */

#ifdef FLOPOS_TEST
/* ---- test build only: a pretend tape ------------------------------------
 * The emulator's card cannot record, so in a test build whatever is played
 * is also kept, and listening plays it back at the speed it was made: the
 * tape app can then be tried whole, saving and reading, with no recorder. */
#define PRETEND_LEN (22050u * 80u)
static u8 *s_pretend;
static u32 s_pretend_n, s_pretend_at, s_pretend_t0;

static void pretend_record(const u8 *p, int n)
{
    int k;
    if (!s_pretend) s_pretend = (u8 *)halloc(PRETEND_LEN);
    if (!s_pretend) return;
    for (k = 0; k < n && s_pretend_n < PRETEND_LEN; k++) s_pretend[s_pretend_n++] = p[k];
}

static void pretend_rewind(void)
{
    s_pretend_at = 0;
    s_pretend_t0 = ms_now();
}

static void pretend_tick(void)
{
    u32 upto = (ms_now() - s_pretend_t0) * (s_rate / 100u) / 10u;
    while (s_pretend_at < upto) {
        u32 next = (s_head + 1) % RING_LEN;
        u8 v = s_pretend_at < s_pretend_n ? s_pretend[s_pretend_at] : 0x80;
        if (next != s_tail) { s_ring[s_head] = v; s_head = next; }
        s_pretend_at++;
    }
}
#endif

/* ---- the DSP ---------------------------------------------------------------- */
static int dsp_reset(u16 b)
{
    int i;
    outb((u16)(b + 6), 1);
    for (i = 0; i < 10; i++) io_wait();          /* at least 3 us          */
    outb((u16)(b + 6), 0);
    for (i = 0; i < 2000; i++) {
        if (inb((u16)(b + 0xE)) & 0x80) return inb((u16)(b + 0xA)) == 0xAA;
        io_wait();
    }
    return 0;
}

static void dsp_w(u8 v)
{
    int t = 20000;
    while ((inb((u16)(s_base + 0xC)) & 0x80) && --t) io_wait();
    outb((u16)(s_base + 0xC), v);
}

static int dsp_r(void)
{
    int t = 20000;
    while (!(inb((u16)(s_base + 0xE)) & 0x80) && --t) io_wait();
    return t ? inb((u16)(s_base + 0xA)) : -1;
}

/* Is there a Sound Blaster, and where?  The usual places, in order. */
int audio_probe(void)
{
    static const u16 bases[] = { 0x220, 0x240, 0x260, 0x280 };
    int i;
    if (s_probed) return s_base != 0;
    s_probed = 1;
    for (i = 0; i < 4; i++) {
        if (dsp_reset(bases[i])) {
            int hi, lo;
            s_base = bases[i];
            dsp_w(0xE1);                         /* its version            */
            hi = dsp_r(); lo = dsp_r();
            s_ver_hi = (u8)(hi < 0 ? 0 : hi);
            s_ver_lo = (u8)(lo < 0 ? 0 : lo);
            return 1;
        }
    }
    return 0;
}

u16 audio_base(void) { return s_base; }
u32 audio_version(void) { return ((u32)s_ver_hi << 8) | s_ver_lo; }

/* Where in s_dma the card is reading now: the DMA controller's current
 * address for channel 1, less where the buffer starts.  Read twice,
 * because the two halves can change between the reads. */
static u32 dma_pos(void)
{
    u32 a, b, start = (u32)s_dma & 0xFFFF;
    int i;
    for (i = 0; i < 3; i++) {
        outb(0x0C, 0);
        a = inb(0x02); a |= (u32)inb(0x02) << 8;
        outb(0x0C, 0);
        b = inb(0x02); b |= (u32)inb(0x02) << 8;
        if (a == b || a + 1 == b) break;
    }
    b -= start;
    return b < DMA_LEN ? b : 0;
}

/* gibson's key hit (sound.c), added in right where the card is about to
 * read, a few milliseconds ahead, rather than at the end of what is already
 * queued, which is most of a second away.  A new key first takes back what
 * the last hit has not yet played, so each key cuts the one before off and
 * starts afresh.  From the timer. */
static void hit_add(u32 from, u32 to, int sign)
{
    for (; from < to; from++) {
        u8 *d = &s_dma[(s_hit_at + from) % DMA_LEN];
        int x = (int)*d + sign * ((int)s_hit[from] - 128);
        *d = (u8)(x < 0 ? 0 : x > 255 ? 255 : x);
    }
}

void audio_hit(const u8 *p, u32 n)
{
    u32 at, done;
    if (s_on != 1 || n > DMA_LEN / 2) return;
    at = (dma_pos() + GUARD) % DMA_LEN;
    if (s_hit) {                                  /* the last hit's unplayed rest */
        done = (at + DMA_LEN - s_hit_at) % DMA_LEN;
        if (done < s_hit_n) hit_add(done, s_hit_n, -1);
    }
    s_hit = p; s_hit_at = at; s_hit_n = n;
    hit_add(0, n, 1);
}

/* From the timer: fill what the card has just played. */
void audio_tick(void)
{
    u32 pos, upto;
    if (!s_on) return;
#ifdef FLOPOS_TEST
    if (s_on == 2) { pretend_tick(); return; }
#endif
    pos = dma_pos();
    if (s_on == 2) {
        /* Listening: take what the card has written since last time.  If
         * nobody has collected for six seconds the newest is dropped. */
        (void)inb((u16)(s_base + 0xE));          /* its interrupt, answered, should it wait on that */
        while (s_fill != pos) {
            u32 next = (s_head + 1) % RING_LEN;
            if (next != s_tail) { s_ring[s_head] = s_dma[s_fill]; s_head = next; }
            s_fill = (s_fill + 1) % DMA_LEN;
        }
        return;
    }
    upto = (pos + DMA_LEN - GUARD) % DMA_LEN;
    while (s_fill != upto) {
        u8 v = 0x80;
        if (s_tail != s_head) { v = s_ring[s_tail]; s_tail = (s_tail + 1) % RING_LEN; }
        s_dma[s_fill] = v;
        s_fill = (s_fill + 1) % DMA_LEN;
    }
}

static void mix(u8 reg, u8 v)
{
    outb((u16)(s_base + 4), reg);
    outb((u16)(s_base + 5), v);
}

/* The buffers, the rate and a fresh DSP, for playing or for listening.
 * Returns the DSP's time constant for the rate, or 0. */
static u32 get_ready(int rate)
{
    u32 tc, i;
    if (!s_dma) {
        u8 *p = (u8 *)halloc(DMA_LEN * 2);
        if (!p) return 0;
        /* Its own 16 KB block, so it never crosses a 64 KB line, which
         * the DMA controller cannot carry an address over. */
        s_dma = (u8 *)(((u32)p + DMA_LEN - 1) & ~(u32)(DMA_LEN - 1));
        s_ring = (u8 *)halloc(RING_LEN);
        if (!s_ring || (u32)s_dma + DMA_LEN > 0x1000000u) { s_dma = 0; return 0; }
    }
    if (rate < 4000) rate = 4000;
    if (rate > 22050) rate = 22050;              /* the ordinary-speed limit */
    tc = 256 - 1000000u / (u32)rate;
    s_rate = 1000000u / (256 - tc);

    for (i = 0; i < DMA_LEN; i++) s_dma[i] = 0x80;
    s_head = s_tail = 0;
    s_hit = 0;                                   /* no hit to take back in a fresh buffer */
    s_fill = 0;

    snd_fx_begin();                              /* the FM music steps aside */
    dsp_reset(s_base);
    return tc;
}

/* Set the DMA controller looping round the buffer on channel 1. */
static void loop_dma(u8 mode)
{
    u32 a = (u32)s_dma;
    outb(0x0A, 0x05);                            /* mask channel 1         */
    outb(0x0C, 0x00);
    outb(0x0B, mode);                            /* single, auto-init, ch 1 */
    outb(0x02, (u8)a); outb(0x02, (u8)(a >> 8));
    outb(0x83, (u8)(a >> 16));                   /* the page               */
    outb(0x0C, 0x00);
    outb(0x03, (u8)(DMA_LEN - 1)); outb(0x03, (u8)((DMA_LEN - 1) >> 8));
    outb(0x0A, 0x01);                            /* and unmask it          */
}

int audio_open(int rate)
{
    u32 tc;

    if (!audio_probe()) return 0;
    if (s_on) audio_close();
    tc = get_ready(rate);
    if (!tc) return 0;
    dsp_w(0xD1);                                 /* speaker on             */

    /* Mixer, for the cards that have one: samples up, and the master a
     * step below full, so the laptop's own amplifier is not driven into
     * clipping. */
    mix(0x22, 0xCC);
    mix(0x04, 0xEE);

    loop_dma(0x59);                              /* memory to card         */
    dsp_w(0x40); dsp_w((u8)tc);                  /* the rate               */
    dsp_w(0x48); dsp_w((u8)((DMA_LEN / 2 - 1) & 0xFF)); dsp_w((u8)((DMA_LEN / 2 - 1) >> 8));
    dsp_w(0x1C);                                 /* 8-bit, auto-init, go   */
    s_on = 1;
    return (int)s_rate;
}

/* ---- listening -----------------------------------------------------------
 * The card records instead of playing; it cannot do both.  source is 1
 * for the line-in socket, 0 for the microphone.  Returns the rate it got,
 * or 0.  What it hears is also passed on to the loudspeaker, so a tape
 * can be heard while it is read.  audio_close ends it. */
int audio_listen(int rate, int source)
{
    u32 tc;

    if (!audio_probe()) return 0;
    if (s_on) audio_close();
    tc = get_ready(rate);
    if (!tc) return 0;
    dsp_w(0xD3);                                 /* its own player: quiet  */

    /* Mixer: which socket, with the card's input filter out of the way
     * (it would dull the tones); that socket up, and the master as for
     * playing. */
    mix(0x0C, source ? 0x26 : 0x20);
    mix(source ? 0x2E : 0x0A, source ? 0xCC : 0x06);
    mix(0x22, 0xCC);

    loop_dma(0x55);                              /* card to memory         */
    dsp_w(0x40); dsp_w((u8)tc);
    dsp_w(0x48); dsp_w((u8)((DMA_LEN / 2 - 1) & 0xFF)); dsp_w((u8)((DMA_LEN / 2 - 1) >> 8));
    dsp_w(0x2C);                                 /* 8-bit, auto-init, listen */
    s_source = source;
#ifdef FLOPOS_TEST
    pretend_rewind();
#endif
    s_on = 2;
    serial_printf("[audio] listening to the %s at %u a second\n", source ? "line-in" : "microphone", s_rate);
    return (int)s_rate;
}

/* What has been heard since the last call, oldest first: up to max. */
int audio_heard(u8 *out, int max)
{
    int n = 0;
    if (s_on != 2) return 0;
    while (n < max && s_tail != s_head) { out[n++] = s_ring[s_tail]; s_tail = (s_tail + 1) % RING_LEN; }
    return n;
}

/* ---- refilling, at interrupt time ---------------------------------------
 * A player cannot rely on being asked for more when it is convenient for
 * it: the app may be drawing, or waiting on the network, for longer than
 * the queue lasts.  So, the way the Mac's Sound Manager did it, the app
 * hands over a refill routine and the timer calls it whenever the queue
 * has room.  It runs after the interrupt is acknowledged, with interrupts
 * back on, so the clock, the keys and the network card carry on around
 * it; never inside itself; and with the floating-point unit's state saved,
 * since decoders use it and so may whatever it interrupted. */
void audio_feeder(void (*fn)(void))
{
    s_feeder = fn;
    if (!fn) s_feeding = 0;              /* an app closed in the middle of a refill */
}

static volatile u32 s_feed_cycles;       /* spent in the refill, so far    */

void audio_deferred(void)
{
    static u8 fpu[128] __attribute__((aligned(16)));
    void (*fn)(void) = s_feeder;
    u32 t0;
    if (!fn || s_feeding || s_on != 1 || audio_space() < 2048) return;
    s_feeding = 1;
    t0 = g_cpu_mhz ? tsc32() : 0;
    __asm__ __volatile__("fnsave %0" : "=m"(fpu));
    __asm__ __volatile__("sti");
    fn();
    __asm__ __volatile__("cli");
    __asm__ __volatile__("frstor %0" : : "m"(fpu));
    if (g_cpu_mhz) s_feed_cycles += tsc32() - t0;
    s_feeding = 0;
}

/* Every ten seconds while sound plays, down the serial wire: what share of
 * the machine the refill took, and how many frames the screen drew a
 * second -- the two numbers that say whether playing costs the screen
 * anything.  From the main loop, not the interrupt. */
void audio_tell(u32 fps)
{
    static u32 at, cyc0;
    u32 now = ms_now(), cyc;
    if (s_on != 1 || !g_cpu_mhz) { at = 0; return; }
    if (!at) { at = now; cyc0 = s_feed_cycles; return; }
    if (now - at < 10000) return;
    cyc = s_feed_cycles - cyc0;
    serial_printf("[audio] decoding took %u.%u%% of the machine; the screen drew %u frames a second\n",
                  cyc / (g_cpu_mhz * (now - at) * 10u), (cyc / (g_cpu_mhz * (now - at))) % 10u, fps);
    at = now;
    cyc0 = s_feed_cycles;
}

int audio_space(void)
{
    u32 used = (s_head + RING_LEN - s_tail) % RING_LEN;
    return (int)(RING_LEN - 1 - used);
}

int audio_queued(void) { return (int)((s_head + RING_LEN - s_tail) % RING_LEN); }

int audio_write(const u8 *p, int n)
{
    int k = 0, room = audio_space();
    if (s_on != 1) return 0;
    if (n > room) n = room;
#ifdef FLOPOS_TEST
    pretend_record(p, n);
#endif
    while (k < n) { s_ring[s_head] = p[k++]; s_head = (s_head + 1) % RING_LEN; }
    return n;
}

void audio_close(void)
{
    int was = s_on;
    s_feeder = 0;
    if (!s_on) return;
    s_on = 0;
    dsp_w(0xD0);                                 /* pause                  */
    dsp_w(0xDA);                                 /* and leave auto-init    */
    dsp_w(0xD3);                                 /* speaker off            */
    if (was == 2) mix(s_source ? 0x2E : 0x0A, 0x00);   /* the socket: out of the loudspeaker again */
    outb(0x0A, 0x05);                            /* mask channel 1         */
    dsp_reset(s_base);
    s_head = s_tail = 0;
    snd_fx_end();
}

/* `dsp`: is it there, and does it take samples at the rate it says?  Plays
 * half a second of two notes and measures how far the card got. */
void audio_report(void)
{
    enum { TONE = 11025 };
    static u8 *tone;                     /* from the heap: low memory is short */
    u32 i, t0, el, start, moved = 0, last;
    int rate;

    if (!audio_probe()) {
        term_print("no sample player answers at 220, 240, 260 or 280");
        term_newline();
        return;
    }
    term_printf("sound blaster dsp at %x, version %u.%02u", (u32)s_base, (u32)s_ver_hi, (u32)s_ver_lo);
    term_newline();
    if (!tone) tone = (u8 *)halloc(TONE);
    rate = tone ? audio_open(11025) : 0;
    if (!rate) { term_print("but it would not start"); term_newline(); return; }
    for (i = 0; i < TONE; i++) {                        /* a fifth, softly    */
        int p = (int)((i < TONE / 2 ? 440u : 660u) * i * SINLEN / (u32)rate);
        tone[i] = (u8)(128 + (SIN(p) >> 7));
    }
    audio_write(tone, TONE);
    start = last = dma_pos();
    t0 = ms_now();
    while ((el = ms_now() - t0) < 1100) {
        u32 p = dma_pos();
        moved += (p + DMA_LEN - last) % DMA_LEN;
        last = p;
        delay_ms(5);
    }
    audio_close();
    (void)start;
    term_printf("asked for %u a second; it took %u bytes in %u ms (%u a second)",
                (u32)rate, moved, el, el ? moved * 1000 / el : 0);
    term_newline();
}

/* The n samples the card has just played: what can be heard this moment.
 * For the HUD's bars.  0 while the card is closed. */
int audio_peek(u8 *out, int n)
{
    u32 pos;
    int i;
    if (!s_dma || !s_rate || n > DMA_LEN) return 0;
    pos = dma_pos();
    for (i = 0; i < n; i++) out[i] = s_dma[(pos + DMA_LEN - (u32)n + (u32)i) % DMA_LEN];
    return 1;
}

int audio_rate_now(void) { return s_dma ? (int)s_rate : 0; }
