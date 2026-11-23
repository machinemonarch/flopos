/* ===========================================================================
 * net.c -- the card, whichever card it is.
 *
 * There are two of them and they have nothing in common.  The 3C589 is
 * what is in the laptop; the NE2000 is what QEMU has, which matters more
 * than it sounds, because everything that gets built on top of this --
 * finding other machines, addressing them, carrying a conversation -- can
 * then be written and tested here rather than guessed at and posted to
 * Sweden on a floppy disk.
 *
 * So a card is four things: come up, tell me your address, send this,
 * and have you got anything.  Nothing above this line knows or cares
 * which chip answered.
 * ===========================================================================
 */
#include "flopos.h"

static const Nic *s_nic;
static char s_why[160] = "nothing has been tried yet";

/* ---- frames waiting ---------------------------------------------------
 * A card keeps only a few frames in its own memory, and one that arrives
 * while that is full is thrown away and has to be sent again, a second or
 * more later.  FLOPOS is often busy for longer than that takes -- drawing
 * the screen, decoding a song -- so the timer, 250 times a second, moves
 * whatever the card holds into a queue here, and everything above reads
 * the queue.  Only one side talks to the card at a time: anything else
 * that touches it marks it busy first, and the timer leaves it alone.
 */
#define RXQ 48                           /* frames; a big TCP window's worth */
static u8 *s_q;
static u16 s_qlen[RXQ];
static volatile u32 s_qin, s_qout;
static volatile int s_busy;

static void drain(int most)
{
    while (most-- > 0 && s_qin - s_qout < RXQ) {
        int n = s_nic->recv(s_q + (s_qin % RXQ) * NET_MTU, NET_MTU);
        if (n <= 0) break;
        s_qlen[s_qin % RXQ] = (u16)n;
        s_qin++;
    }
}

/* From the timer. */
void net_tick(void)
{
    if (!s_nic || !s_q || s_busy) return;
    drain(8);
}

/* In order of preference: the real card first, since on the machine this
 * is for, the real card is the one that is there. */
static const Nic *const s_cards[] = { &g_nic_el3, &g_nic_ne2000 };
#define NCARDS ((int)(sizeof s_cards / sizeof s_cards[0]))

/* Every card's reason, not just the last one's.  When nothing comes up,
 * "no network card" is the least useful sentence available: which card
 * refused and why is the whole of the information. */
static void note_why(const char *card, const char *why)
{
    int n = 0, i;

    while (s_why[n] && n < (int)sizeof s_why - 2) n++;
    if (n) { s_why[n++] = ';'; s_why[n++] = ' '; }
    for (i = 0; card[i] && n < (int)sizeof s_why - 2; i++) s_why[n++] = card[i];
    s_why[n++] = ':'; s_why[n++] = ' ';
    for (i = 0; why[i] && n < (int)sizeof s_why - 1; i++) s_why[n++] = why[i];
    s_why[n] = 0;
}

int net_up(void)
{
    int i;

    if (s_nic) return 1;

    s_why[0] = 0;
    for (i = 0; i < NCARDS; i++) {
        /* The NE2000 lives at 0x300 by convention, which is exactly where
         * the 3Com is put when it comes up.  On a machine with PC Card
         * slots there is no NE2000, and probing for one means writing to
         * the 3Com's registers, so it is not looked for at all. */
        if (s_cards[i] == &g_nic_ne2000 && pcic_probe()) continue;

        if (s_cards[i]->start()) {
            if (!s_q) s_q = (u8 *)halloc(RXQ * NET_MTU);
            s_qin = s_qout = 0;
            s_nic = s_cards[i];
            return 1;
        }
        note_why(s_cards[i]->name, s_cards[i]->why());
    }
    return 0;
}

int net_link(void)
{
    int r;
    if (!s_nic || !s_nic->link) return 0;
    s_busy++; r = s_nic->link(); s_busy--;
    return r;
}

void net_down(void)
{
    s_busy++;
    if (s_nic) s_nic->stop();
    s_nic = 0;
    s_qin = s_qout = 0;
    s_busy--;
}

int          net_ready(void) { return s_nic != 0; }
const char  *net_name(void)  { return s_nic ? s_nic->name : "none"; }
const char  *net_why(void)   { return s_why; }
const u8    *net_mac(void)   { return s_nic ? s_nic->mac() : (const u8 *)"\0\0\0\0\0\0"; }

/* For the wire, when something goes quiet: the queue, and the card's own
 * idea of where it is. */
const char *ne2000_state(void);
const char *net_state(void)
{
    static char t[120];
    const char *c = "";
    if (s_nic == &g_nic_ne2000) { s_busy++; c = ne2000_state(); s_busy--; }
    fmt_str(t, sizeof t, "%u waiting%s%s", s_qin - s_qout, c[0] ? ", " : "", c);
    return t;
}

/* The wire has a minimum: sixty bytes before the checksum, and anything
 * shorter is a runt that switches are entitled to throw away.  An ARP
 * request is forty-two.  So short frames are copied out and padded with
 * zeros here, once, for every card -- rather than each driver padding by
 * reading on past the end of whatever buffer it was handed. */
int net_send(const u8 *frame, int len)
{
    static u8 pad[60];
    int i;

    int r;

    if (!s_nic || len < 14 || len > NET_MTU) return 0;
    s_busy++;
    if (len >= 60) r = s_nic->send(frame, len);
    else {
        for (i = 0; i < len; i++) pad[i] = frame[i];
        for (; i < 60; i++) pad[i] = 0;
        r = s_nic->send(pad, 60) ? len : 0;
    }
    s_busy--;
    return r;
}

/* The next frame that came, from the queue -- asking the card first if
 * the queue is empty, rather than wait for the timer. */
int net_recv(u8 *frame, int max)
{
    const u8 *src;
    int n, i;

    if (!s_nic) return 0;
    if (s_qin == s_qout) { s_busy++; drain(8); s_busy--; }
    if (s_qin == s_qout) return 0;
    src = s_q + (s_qout % RXQ) * NET_MTU;
    n = s_qlen[s_qout % RXQ];
    if (n > max) n = max;
    for (i = 0; i < n; i++) frame[i] = src[i];
    s_qout++;
    return n;
}

/* ---- what a frame looks like ------------------------------------------
 * Six bytes of who it is for, six of who it is from, two saying what the
 * rest of it is.  That has not changed since 1980 and will outlive all of
 * us. */
int net_frame_begin(u8 *f, const u8 *to, u16 type)
{
    int i;
    for (i = 0; i < 6; i++) f[i] = to[i];
    for (i = 0; i < 6; i++) f[6 + i] = net_mac()[i];
    f[12] = (u8)(type >> 8);
    f[13] = (u8)(type & 0xFF);
    return 14;
}

const u8 g_mac_broadcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
