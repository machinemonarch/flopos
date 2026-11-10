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
static volatile int s_on;
static u32 s_rate;

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

/* From the timer: fill what the card has just played. */
void audio_tick(void)
{
    u32 pos, upto;
    if (!s_on) return;
    pos = dma_pos();
    upto = (pos + DMA_LEN - GUARD) % DMA_LEN;
    while (s_fill != upto) {
        u8 v = 0x80;
        if (s_tail != s_head) { v = s_ring[s_tail]; s_tail = (s_tail + 1) % RING_LEN; }
        s_dma[s_fill] = v;
        s_fill = (s_fill + 1) % DMA_LEN;
    }
}

int audio_open(int rate)
{
    u32 tc, a, i;

    if (!audio_probe()) return 0;
    if (s_on) audio_close();
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
    s_fill = 0;

    snd_fx_begin();                              /* the FM music steps aside */
    dsp_reset(s_base);
    dsp_w(0xD1);                                 /* speaker on             */

    /* Mixer, for the cards that have one: samples up, and the master a
     * step below full, so the laptop's own amplifier is not driven into
     * clipping. */
    outb((u16)(s_base + 4), 0x22); outb((u16)(s_base + 5), 0xCC);
    outb((u16)(s_base + 4), 0x04); outb((u16)(s_base + 5), 0xEE);

    a = (u32)s_dma;
    outb(0x0A, 0x05);                            /* mask channel 1         */
    outb(0x0C, 0x00);
    outb(0x0B, 0x59);                            /* single, auto-init, memory to card, ch 1 */
    outb(0x02, (u8)a); outb(0x02, (u8)(a >> 8));
    outb(0x83, (u8)(a >> 16));                   /* the page               */
    outb(0x0C, 0x00);
    outb(0x03, (u8)(DMA_LEN - 1)); outb(0x03, (u8)((DMA_LEN - 1) >> 8));
    outb(0x0A, 0x01);                            /* and unmask it          */

    dsp_w(0x40); dsp_w((u8)tc);                  /* the rate               */
    dsp_w(0x48); dsp_w((u8)((DMA_LEN / 2 - 1) & 0xFF)); dsp_w((u8)((DMA_LEN / 2 - 1) >> 8));
    dsp_w(0x1C);                                 /* 8-bit, auto-init, go   */
    s_on = 1;
    return (int)s_rate;
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
    if (!s_on) return 0;
    if (n > room) n = room;
    while (k < n) { s_ring[s_head] = p[k++]; s_head = (s_head + 1) % RING_LEN; }
    return n;
}

void audio_close(void)
{
    if (!s_on) return;
    s_on = 0;
    dsp_w(0xD0);                                 /* pause                  */
    dsp_w(0xDA);                                 /* and leave auto-init    */
    dsp_w(0xD3);                                 /* speaker off            */
    outb(0x0A, 0x05);                            /* mask channel 1         */
    dsp_reset(s_base);
    s_head = s_tail = 0;
    snd_fx_end();
}

/* `dsp`: is it there, and does it take samples at the rate it says?  Plays
 * half a second of two notes and measures how far the card got. */
void audio_report(void)
{
    static u8 tone[11025];
    u32 i, t0, el, start, moved = 0, last;
    int rate;

    if (!audio_probe()) {
        term_print("no sample player answers at 220, 240, 260 or 280");
        term_newline();
        return;
    }
    term_printf("sound blaster dsp at %x, version %u.%02u", (u32)s_base, (u32)s_ver_hi, (u32)s_ver_lo);
    term_newline();
    rate = audio_open(11025);
    if (!rate) { term_print("but it would not start"); term_newline(); return; }
    for (i = 0; i < sizeof tone; i++) {                 /* a fifth, softly    */
        int p = (int)((i < sizeof tone / 2 ? 440u : 660u) * i * SINLEN / (u32)rate);
        tone[i] = (u8)(128 + (SIN(p) >> 7));
    }
    audio_write(tone, (int)sizeof tone);
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
