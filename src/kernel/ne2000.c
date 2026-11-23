/* ===========================================================================
 * ne2000.c -- the other card, the one that only exists in QEMU.
 *
 * There is no 3C589 in any emulator and no PC Card socket either, so
 * everything that gets built on top of a network card -- finding other
 * machines, addressing them, holding a conversation -- would have to be
 * written blind and posted to Sweden on a floppy to find out whether it
 * worked.  An NE2000 is a card QEMU does have.  Teaching FLOPOS to drive
 * one costs two hundred lines and buys the ability to test every line
 * above the card against a real network.
 *
 * The chip is a National DP8390 with sixteen kilobytes of memory behind
 * it.  Its registers are paged, like the 3Com's are windowed, and for the
 * same reason: not enough addresses.  Its memory is reached through a
 * hatch rather than directly -- set an address and a count, then read or
 * write one port that many times -- which is slower than it sounds and
 * was state of the art in 1990.
 *
 * Register numbers and bit meanings are the published DP8390 interface.
 * ===========================================================================
 */
#include "flopos.h"
#include "io.h"

#define NE_BASE   0x300                  /* where QEMU puts ne2k_isa       */

#define R_CMD     0x00
#define R_DATA    0x10                   /* the hatch into card memory     */
#define R_RESET   0x1F

/* Page 0, writing. */
#define R_PSTART  0x01
#define R_PSTOP   0x02
#define R_BNRY    0x03
#define R_TPSR    0x04
#define R_TBCR0   0x05
#define R_TBCR1   0x06
#define R_ISR     0x07
#define R_RSAR0   0x08
#define R_RSAR1   0x09
#define R_RBCR0   0x0A
#define R_RBCR1   0x0B
#define R_RCR     0x0C
#define R_TCR     0x0D
#define R_DCR     0x0E
#define R_IMR     0x0F
/* Page 0, reading. */
#define R_TSR     0x04
#define R_RSR     0x0C
/* Page 1. */
#define R_PAR0    0x01
#define R_CURR    0x07

#define CMD_STOP  0x01
#define CMD_START 0x02
#define CMD_TXP   0x04
#define CMD_RD_READ  0x08
#define CMD_RD_WRITE 0x10
#define CMD_RD_ABORT 0x20
#define CMD_PAGE1 0x40

#define ISR_PRX   0x01
#define ISR_PTX   0x02
#define ISR_RST   0x80
#define ISR_RDC   0x40
#define ISR_OVW   0x10                     /* the receive ring ran over      */
#define ISR_TXE   0x08

/* How the sixteen kilobytes are split.  Six pages for sending, which is
 * four more than one frame needs, and the rest a ring for receiving. */
#define TX_PAGE   0x40
#define RX_START  0x46
#define RX_STOP   0x80

static int s_up;
static u8  s_mac[6];
static u8  s_next;                       /* where to read the ring next    */
static const char *s_why = "not tried";

static u8   rd(int r)        { return inb((u16)(NE_BASE + r)); }
static void wr(int r, u8 v)  { outb((u16)(NE_BASE + r), v); }

/* Move bytes out of card memory through the hatch. */
static void dma_in(u16 addr, void *buf, int len)
{
    wr(R_ISR, ISR_RDC);
    wr(R_RBCR0, (u8)(len & 0xFF));
    wr(R_RBCR1, (u8)(len >> 8));
    wr(R_RSAR0, (u8)(addr & 0xFF));
    wr(R_RSAR1, (u8)(addr >> 8));
    wr(R_CMD, CMD_RD_READ | CMD_START);
    insw((u16)(NE_BASE + R_DATA), buf, (len + 1) / 2);
}

static void dma_out(u16 addr, const void *buf, int len)
{
    int guard;

    wr(R_ISR, ISR_RDC);
    wr(R_RBCR0, (u8)(len & 0xFF));
    wr(R_RBCR1, (u8)(len >> 8));
    wr(R_RSAR0, (u8)(addr & 0xFF));
    wr(R_RSAR1, (u8)(addr >> 8));
    wr(R_CMD, CMD_RD_WRITE | CMD_START);
    outsw((u16)(NE_BASE + R_DATA), buf, (len + 1) / 2);

    for (guard = 0; guard < 100000; guard++)
        if (rd(R_ISR) & ISR_RDC) break;
}

static const u8 *ne_mac(void) { return s_mac; }
static const char *ne_why(void) { return s_why; }

static int ne_start(void)
{
    u8 prom[32];
    int i, guard;

    s_up = 0;

    /* Reading the reset port and writing it back is how this part is told
     * to start again.  An absent card floats the bus, so the bit that is
     * supposed to come up never does and we leave quickly. */
    outb(NE_BASE + R_RESET, inb(NE_BASE + R_RESET));
    for (guard = 0; guard < 10000; guard++)
        if (rd(R_ISR) & ISR_RST) break;
    if (guard >= 10000) { s_why = "nothing answers at 300"; return 0; }

    /* An empty address floats, and a floating bus reads as all ones --
     * which has the reset bit set, so the check above passes on a machine
     * with nothing there at all.  Writing a value somewhere harmless and
     * reading it back is the only honest test: a bus that is not driven
     * cannot remember anything. */
    wr(R_CMD, CMD_RD_ABORT | CMD_STOP);
    wr(R_RBCR0, 0x5A);
    wr(R_RBCR1, 0xA5);
    if (rd(R_RBCR0) == 0x5A && rd(R_RBCR1) == 0xA5) {
        /* Those two are write-only on a real part, so reading the value
         * back means nothing is decoding the address at all. */
        s_why = "nothing at 300 (the bus is floating)";
        return 0;
    }

    wr(R_CMD, CMD_PAGE1 * 0 | CMD_RD_ABORT | CMD_STOP);
    wr(R_DCR, 0x49);                     /* sixteen bits at a time         */
    wr(R_RBCR0, 0); wr(R_RBCR1, 0);
    wr(R_RCR, 0x20);                     /* hear nothing, for now          */
    wr(R_TCR, 0x02);                     /* talk to itself, for now        */

    /* The address is in a small memory on the card, and in sixteen-bit
     * mode every byte of it arrives twice, so only every other one is
     * real. */
    dma_in(0, prom, 32);
    for (i = 0; i < 6; i++) s_mac[i] = prom[i * 2];

    {
        int ones = 0, zeros = 0;
        for (i = 0; i < 6; i++) {
            if (s_mac[i] == 0xFF) ones++;
            if (s_mac[i] == 0x00) zeros++;
        }
        /* All ones is a bus with nothing on it; all zeros is a card that
         * has not been programmed.  Neither is an address, and neither
         * should ever be reported as one. */
        if (ones == 6 || zeros == 6) {
            s_why = "nothing at 300 (no address to read)";
            return 0;
        }
        /* And the first bit of the first byte means "this is a group, not
         * a machine", which no card's own address ever has set. */
        if (s_mac[0] & 1) {
            s_why = "what is at 300 is not a network card";
            return 0;
        }
    }

    wr(R_TPSR, TX_PAGE);
    wr(R_PSTART, RX_START);
    wr(R_PSTOP, RX_STOP);
    wr(R_BNRY, RX_START);
    s_next = RX_START + 1;

    wr(R_CMD, CMD_PAGE1 | CMD_RD_ABORT | CMD_STOP);
    for (i = 0; i < 6; i++) wr(R_PAR0 + i, s_mac[i]);
    wr(R_CURR, s_next);
    wr(R_CMD, CMD_RD_ABORT | CMD_STOP);

    wr(R_ISR, 0xFF);
    wr(R_IMR, 0x00);                     /* polled, like the other one     */
    wr(R_TCR, 0x00);                     /* out loud now                   */
    wr(R_RCR, 0x04);                     /* us, and everybody              */
    wr(R_CMD, CMD_RD_ABORT | CMD_START);

    s_up = 1;
    s_why = "up";
    return 1;
}

static void ne_stop(void)
{
    if (!s_up) return;
    wr(R_CMD, CMD_RD_ABORT | CMD_STOP);
    s_up = 0;
}

static int ne_send(const u8 *frame, int len)
{
    int guard;

    if (!s_up) return 0;

    dma_out(TX_PAGE << 8, frame, len);
    wr(R_TBCR0, (u8)(len & 0xFF));
    wr(R_TBCR1, (u8)(len >> 8));
    wr(R_CMD, CMD_RD_ABORT | CMD_TXP | CMD_START);

    for (guard = 0; guard < 100000; guard++) {
        if (rd(R_ISR) & ISR_PTX) { wr(R_ISR, ISR_PTX); return len; }
    }
    return 0;
}

/* The ring ran over: frames came faster than they were read -- a radio
 * station still sending after its stream was stopped, say.  The DP8390
 * then stops taking any frame at all until it is put right the way its
 * datasheet gives: stop, wait, clear the byte counts, loop back, empty the
 * ring, clear the flag, and send again whatever was going out.  Without
 * it the card went deaf, and the next name looked up never got its
 * answer. */
static void ring_overflow(void)
{
    u8 cmd = rd(R_CMD);
    int resend = 0;
    wr(R_CMD, CMD_RD_ABORT | CMD_STOP);
    delay_ms(2);                         /* 1.6 ms at least                 */
    wr(R_RBCR0, 0);
    wr(R_RBCR1, 0);
    if (cmd & CMD_TXP) resend = !(rd(R_ISR) & (ISR_PTX | ISR_TXE));
    wr(R_TCR, 0x02);                     /* loop back, while it is emptied  */
    wr(R_CMD, CMD_RD_ABORT | CMD_START);
    s_next = RX_START + 1;               /* everything in it, dropped       */
    wr(R_BNRY, RX_START);
    wr(R_CMD, CMD_PAGE1 | CMD_RD_ABORT | CMD_START);
    wr(R_CURR, s_next);
    wr(R_CMD, CMD_RD_ABORT | CMD_START);
    wr(R_ISR, ISR_OVW);
    wr(R_TCR, 0x00);
    if (resend) wr(R_CMD, CMD_RD_ABORT | CMD_TXP | CMD_START);
    serial_print("[ne2000] the ring ran over: emptied, listening again\n");
}

/* Each frame in the ring has four bytes in front of it: how it went, which
 * page the next one starts on, and how long it is counting those four. */
static int ne_recv(u8 *frame, int max)
{
    u8 hdr[4], curr;
    int len;

    if (!s_up) return 0;
    if (rd(R_ISR) & ISR_OVW) ring_overflow();

    wr(R_CMD, CMD_PAGE1 | CMD_RD_ABORT | CMD_START);
    curr = rd(R_CURR);
    wr(R_CMD, CMD_RD_ABORT | CMD_START);

    if (curr == s_next) return 0;        /* nothing new in the ring        */

    dma_in((u16)(s_next << 8), hdr, 4);
    len = (hdr[2] | (hdr[3] << 8)) - 4;

    /* A header is believed only if its next page is where its length
     * says (give or take the page some cards leave spare).  QEMU's card,
     * when its ring fills exactly, takes it for empty and writes over the
     * oldest frame; what is read there is then not a header at all.  The
     * old answer to that, going back to the start of the ring, landed on
     * more of the same, and the card stayed full and deaf for good.  Now
     * everything in the ring is let go and reading starts again where the
     * card is. */
    {
        int pages = (len + 4 + 4 + 255) >> 8, ring = RX_STOP - RX_START;
        int step = ((int)hdr[1] - (int)s_next + ring) % ring;
        if (!(hdr[0] & 0x01) || len < 14 || len > NET_MTU || hdr[1] < RX_START || hdr[1] >= RX_STOP ||
            step < pages - 1 || step > pages) {
            serial_printf("[ne2000] a damaged frame in the ring at %x (status %x, next %x, %d bytes): the ring let go\n",
                          s_next, hdr[0], hdr[1], len);
            s_next = curr;
            wr(R_BNRY, (u8)(s_next == RX_START ? RX_STOP - 1 : s_next - 1));
            return 0;
        }
    }
    if (len <= max) dma_in((u16)((s_next << 8) + 4), frame, len);
    else len = 0;

    s_next = hdr[1];
    wr(R_BNRY, (u8)(s_next == RX_START ? RX_STOP - 1 : s_next - 1));

    return len;
}

/* Where the ring stands, for the wire (net_state). */
const char *ne2000_state(void)
{
    static char t[80];
    u8 curr, bnry, isr;
    if (!s_up) return "down";
    wr(R_CMD, CMD_PAGE1 | CMD_RD_ABORT | CMD_START);
    curr = rd(R_CURR);
    wr(R_CMD, CMD_RD_ABORT | CMD_START);
    bnry = rd(R_BNRY); isr = rd(R_ISR);
    fmt_str(t, sizeof t, "ring next %x, card at %x, boundary %x, status %x", s_next, curr, bnry, isr);
    return t;
}

/* No link status register on this part worth the name; in QEMU there is
 * always somebody at the other end. */
static int ne_link(void) { return s_up; }

const Nic g_nic_ne2000 = {
    "NE2000", ne_start, ne_stop, ne_mac, ne_send, ne_recv, ne_why, ne_link
};
