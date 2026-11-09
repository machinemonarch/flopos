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

        if (s_cards[i]->start()) { s_nic = s_cards[i]; return 1; }
        note_why(s_cards[i]->name, s_cards[i]->why());
    }
    return 0;
}

int net_link(void) { return s_nic && s_nic->link ? s_nic->link() : 0; }

void net_down(void)
{
    if (s_nic) s_nic->stop();
    s_nic = 0;
}

int          net_ready(void) { return s_nic != 0; }
const char  *net_name(void)  { return s_nic ? s_nic->name : "none"; }
const char  *net_why(void)   { return s_why; }
const u8    *net_mac(void)   { return s_nic ? s_nic->mac() : (const u8 *)"\0\0\0\0\0\0"; }

/* The wire has a minimum: sixty bytes before the checksum, and anything
 * shorter is a runt that switches are entitled to throw away.  An ARP
 * request is forty-two.  So short frames are copied out and padded with
 * zeros here, once, for every card -- rather than each driver padding by
 * reading on past the end of whatever buffer it was handed. */
int net_send(const u8 *frame, int len)
{
    static u8 pad[60];
    int i;

    if (!s_nic || len < 14 || len > NET_MTU) return 0;
    if (len >= 60) return s_nic->send(frame, len);

    for (i = 0; i < len; i++) pad[i] = frame[i];
    for (; i < 60; i++) pad[i] = 0;
    return s_nic->send(pad, 60) ? len : 0;
}

int net_recv(u8 *frame, int max)
{
    if (!s_nic) return 0;
    return s_nic->recv(frame, max);
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
