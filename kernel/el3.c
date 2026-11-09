/* ===========================================================================
 * el3.c -- the 3Com 3C589, which is an EtherLink III on a PC Card.
 *
 * The card told us what it is itself: 3Com Corporation, 3C589D, TP/BNC LAN
 * Card Ver. 2a, function code 6, configuration index 1, and its switch-on
 * register sixty-four kilobytes into its own attribute memory.  None of
 * that was looked up.  It was read off the card in the machine.
 *
 * What is in here is the EtherLink III programming model, which 3Com
 * published: sixteen I/O ports, and a window number in the top bits of the
 * command register that decides what most of those sixteen ports mean.
 * One set of ports is the EEPROM, another is the packet FIFOs, another is
 * the station address.  You select a window, then you use the ports.
 *
 * It is an unusual arrangement and it exists because the card only has
 * sixteen addresses to work with and needs far more than sixteen
 * registers.  The cost is that nothing can be touched without knowing
 * which window is currently selected, so everything here selects first.
 *
 * Provenance: register numbers, command numbers and bit meanings are the
 * published interface to the chip -- facts about a part, the same way a
 * port number is a fact.  No driver's code is here, 3Com's or anybody
 * else's; this was written from the interface and works how it works.
 *
 * Interrupts are not used.  Everything is polled, because the first thing
 * that has to be true is that the card answers at all, and an interrupt
 * that never arrives and a card that is not there look identical.
 * ===========================================================================
 */
#include "flopos.h"
#include "io.h"

/* The command register, and the status register at the same address. */
#define EL3_CMD     0x0E
#define EL3_STATUS  0x0E

/* Commands live in the top five bits, the argument in the low eleven. */
#define CMD(n)      ((u16)((n) << 11))
#define C_RESET     CMD(0)
#define C_WINDOW    CMD(1)
#define C_RX_DIS    CMD(3)
#define C_RX_ENA    CMD(4)
#define C_RX_RESET  CMD(5)
#define C_RX_DISCARD CMD(8)
#define C_TX_ENA    CMD(9)
#define C_TX_DIS    CMD(10)
#define C_TX_RESET  CMD(11)
#define C_ACK_INTR  CMD(13)
#define C_SET_MASK  CMD(14)
#define C_SET_ZERO  CMD(15)
#define C_RX_FILTER CMD(16)
#define C_STATS_DIS CMD(22)

/* Window 0: how the card was set up at the factory. */
#define W0_EE_CMD   0x0A
#define W0_EE_DATA  0x0C
#define EE_BUSY     0x8000
#define EE_READ     0x0080

/* Window 1: the one it runs in. */
#define W1_TX_FIFO  0x00
#define W1_RX_FIFO  0x00
#define W1_RX_STAT  0x08
#define W1_TX_STAT  0x0B
#define W1_TX_FREE  0x0C

/* Window 2: the station address, which has to be written back into the
 * card even though the card is where it came from. */
#define W2_ADDR     0x00

/* The EEPROM's own layout.  The first three words are the address. */
#define EE_ADDR0    0x00
#define EE_PROD_ID  0x03
#define EE_MANF_ID  0x07

#define MANF_3COM   0x6D50               /* "3Com", as a number            */

/* What to listen to. */
#define RXF_STATION   0x01
#define RXF_MULTICAST 0x02
#define RXF_BROADCAST 0x04
#define RXF_ALL       0x08

/* The receive status word, and the one status bit that means "still
 * chewing on the last command you gave me". */
#define RX_INCOMPLETE 0x8000
#define RX_ERROR      0x4000
#define S_CMD_BUSY    0x1000

static int  s_up;
static const char *s_why = "not tried";
static int  s_sock;
static u16  s_io;
static u8   s_mac[6];
static u16  s_prod;
static u16  s_manf;

static void el3_discard(void);

static void win(int n)
{
    outw((u16)(s_io + EL3_CMD), (u16)(C_WINDOW | n));
}

static u16 st(void)
{
    return inw((u16)(s_io + EL3_STATUS));
}

/* The EEPROM is slow in the way that only a part designed in 1993 is
 * slow: ask for a word, then wait while it spells it out.  162
 * microseconds is the published worst case, so a millisecond of patience
 * per attempt is generous and a tenth of a second in total is a refusal. */
static int ee_read(int addr, u16 *out)
{
    int i;

    win(0);
    outw((u16)(s_io + W0_EE_CMD), (u16)(EE_READ | (addr & 0x3F)));

    for (i = 0; i < 100; i++) {
        delay_ms(1);
        if (!(inw((u16)(s_io + W0_EE_CMD)) & EE_BUSY)) {
            *out = inw((u16)(s_io + W0_EE_DATA));
            return 1;
        }
    }
    return 0;
}

/* Throwing the top frame away is a command, and the card is busy while it
 * does it. */
static void el3_discard(void)
{
    int guard;
    outw((u16)(s_io + EL3_CMD), C_RX_DISCARD);
    for (guard = 0; guard < 10000; guard++)
        if (!(st() & S_CMD_BUSY)) return;
}

int  el3_up(void)        { return s_up; }
u16  el3_io(void)        { return s_io; }
int  el3_socket(void)    { return s_sock; }
u16  el3_product(void)   { return s_prod; }
u16  el3_maker(void)     { return s_manf; }
const u8 *el3_mac(void)  { return s_mac; }

/* Bring the card up far enough to ask it who it is.
 *
 * Switching it on is two separate things that are easy to confuse.  The
 * socket has to be told where in the machine's address space the card's
 * sixteen ports should appear -- that is the adapter's job.  And the card
 * has to be told to start decoding addresses at all, by writing the number
 * of one of its own configurations into a register in its attribute
 * memory -- that is the card's job.  Neither works without the other, and
 * getting only one right gives a card that is there and silent.
 */
/* The two things worth keeping out of a card's own description: where its
 * switch-on register is, and whether it claims to be a network card. */
typedef struct { u32 cor; int func; } Found;

static int el3_tuple(u8 code, const u8 *body, int len, void *ctx)
{
    Found *f = (Found *)ctx;
    int i;

    if (code == CISTPL_FUNCID && len >= 1) f->func = body[0];
    if (code == CISTPL_CONFIG && len >= 3) {
        int nb = (body[0] & 3) + 1;
        u32 base = 0;
        for (i = 0; i < nb && 2 + i < len; i++)
            base |= (u32)body[2 + i] << (8 * i);
        f->cor = base;
    }
    return 0;
}

#define FUNC_NETWORK 6
#define EL3_IO_BASE  0x300               /* Toshiba's own known-good slot  */

/* Look in every socket for something that says it is a network card, and
 * take the first one.  Which socket it happens to be in is the card
 * owner's business, not ours. */
static int el3_find(void)
{
    int n = pcic_probe(), sock;

    if (!n) { s_why = "no PC Card adapter answered"; return 0; }

    for (sock = 0; sock < n; sock++) {
        Found f;
        if (!pcic_present(sock)) continue;
        if (!pcic_power_up(sock, 1)) { pcic_power_down(sock); continue; }

        f.cor = 0; f.func = -1;
        if (pcic_cis_walk(sock, el3_tuple, &f) > 0 &&
            f.func == FUNC_NETWORK && f.cor) {
            return el3_start(sock, f.cor, EL3_IO_BASE);
        }
        pcic_power_down(sock);
    }
    s_why = "no network card in either socket";
    return 0;
}

const char *el3_why(void) { return s_why; }

int el3_start(int sock, u32 cor, u16 io)
{
    u16 w;
    int i;

    s_up = 0;
    s_sock = sock;
    s_io = io;

    if (!pcic_present(sock)) { s_why = "the socket is empty"; return 0; }
    if (!pcic_power_up(sock, 1)) {
        s_why = "the card never came ready after power";
        return 0;
    }

    /* Reset the card through its own configuration register first, so it
     * does not matter what state anything left it in. */
    pcic_attr_write(sock, cor, 0x80);
    delay_ms(20);
    pcic_attr_write(sock, cor, 0x00);
    delay_ms(20);

    if (!pcic_map_io(sock, io, 16)) {
        s_why = "the socket would not take an I/O window";
        return 0;
    }
    pcic_set_irq(sock, 0);               /* polled, for now                */

    /* Configuration one: the entry the card marked as its default. */
    pcic_attr_write(sock, cor, 0x01);
    delay_ms(20);

    /* From here the card should be answering at io.  A global reset takes
     * about a millisecond and the card ignores everything until it is
     * finished. */
    outw((u16)(s_io + EL3_CMD), C_RESET);
    delay_ms(10);

    /* If nothing is there the bus floats and every read is 0xFFFF.  That
     * is the one answer that means "no card", and it has to be checked
     * before anything is believed. */
    if (st() == 0xFFFF) {
        s_why = "nothing answers at the I/O address";
        return 0;
    }

    if (!ee_read(EE_MANF_ID, &s_manf)) { s_why = "its eeprom never answered"; return 0; }
    if (!ee_read(EE_PROD_ID, &s_prod)) { s_why = "its eeprom stopped halfway"; return 0; }
    if (s_manf != MANF_3COM) { s_why = "the eeprom says it is not a 3Com"; return 0; }

    /* The address, three words of it, most significant byte first inside
     * each word -- which is the opposite way round from everything else on
     * this machine, so it is spelled out rather than cast. */
    for (i = 0; i < 3; i++) {
        if (!ee_read(EE_ADDR0 + i, &w)) {
            s_why = "it would not give up its address";
            return 0;
        }
        s_mac[i * 2]     = (u8)(w >> 8);
        s_mac[i * 2 + 1] = (u8)(w & 0xFF);
    }

    /* The card does not put its own address into its receive filter on its
     * own; it has to be written back in through window 2. */
    win(2);
    for (i = 0; i < 6; i++)
        outb((u16)(s_io + W2_ADDR + i), s_mac[i]);

    /* No interrupts and no statistics: everything is polled, so an
     * interrupt would only be something else to get wrong. */
    outw((u16)(s_io + EL3_CMD), C_SET_MASK | 0);
    outw((u16)(s_io + EL3_CMD), C_SET_ZERO | 0);
    outw((u16)(s_io + EL3_CMD), C_ACK_INTR | 0x7FF);
    outw((u16)(s_io + EL3_CMD), C_STATS_DIS);

    /* Listen for things addressed to us and for things addressed to
     * everybody, and nothing else.  Promiscuous would be easier and is
     * exactly the sort of easier that ends up shipped. */
    outw((u16)(s_io + EL3_CMD), C_RX_FILTER | RXF_STATION | RXF_BROADCAST);
    outw((u16)(s_io + EL3_CMD), C_RX_RESET);
    delay_ms(2);
    outw((u16)(s_io + EL3_CMD), C_TX_RESET);
    delay_ms(2);
    outw((u16)(s_io + EL3_CMD), C_RX_ENA);
    outw((u16)(s_io + EL3_CMD), C_TX_ENA);
    win(1);
    s_why = "up";

    s_up = 1;
    return 1;
}

/* ---- sending ----------------------------------------------------------
 * The transmit FIFO wants four bytes of preamble before the frame -- the
 * length, and two bytes of nothing -- and then the frame itself padded out
 * to a multiple of four.  Everything goes through one port.
 */
static int el3_send(const u8 *frame, int len)
{
    int words = (len + 1) / 2;
    int i, guard;

    if (!s_up) return 0;
    win(1);

    /* Room for the frame and its preamble, or wait until there is.  The
     * card frees space as the wire drains, so this is short or never. */
    for (guard = 0; guard < 10000; guard++) {
        if (inw((u16)(s_io + W1_TX_FREE)) >= len + 4) break;
        if (guard > 2000) return 0;
    }

    outw((u16)(s_io + W1_TX_FIFO), (u16)len);
    outw((u16)(s_io + W1_TX_FIFO), 0);
    outsw((u16)(s_io + W1_TX_FIFO), frame, words);
    /* Pad to a whole number of four-byte units, which is what the FIFO
     * counts in whatever the length said. */
    for (i = words * 2; i & 3; i += 2) outw((u16)(s_io + W1_TX_FIFO), 0);

    /* The transmit status is a stack of one byte per attempt; read until
     * it stops giving them, and clear each as it comes. */
    for (guard = 0; guard < 32; guard++) {
        u8 ts = inb((u16)(s_io + W1_TX_STAT));
        if (!(ts & 0x80)) break;
        outb((u16)(s_io + W1_TX_STAT), 0);
        if (ts & 0x38) {                 /* jabber, underrun, max collisions */
            outw((u16)(s_io + EL3_CMD), C_TX_RESET);
            outw((u16)(s_io + EL3_CMD), C_TX_ENA);
            return 0;
        }
    }
    return len;
}

/* ---- receiving --------------------------------------------------------
 * One status word says whether a whole frame is waiting and how long it
 * is.  The frame comes out of the same single port it went in, and then
 * the card has to be told to throw it away before the next one appears.
 */
static int el3_recv(u8 *frame, int max)
{
    u16 rs;
    int len, guard;

    if (!s_up) return 0;
    win(1);

    rs = inw((u16)(s_io + W1_RX_STAT));
    if (rs & RX_INCOMPLETE) return 0;    /* nothing whole yet              */

    if (rs & RX_ERROR) { el3_discard(); return 0; }

    len = rs & 0x7FF;
    if (len <= 0 || len > max) { el3_discard(); return 0; }

    insw((u16)(s_io + W1_RX_FIFO), frame, (len + 1) / 2);
    el3_discard();
    (void)guard;
    return len;
}

void el3_stop(void)
{
    if (!s_up) return;
    outw((u16)(s_io + EL3_CMD), C_RESET);
    s_up = 0;
}

static const u8 *el3_mac_ro(void) { return s_mac; }
static int el3_up_ro(void) { return el3_find(); }

const Nic g_nic_el3 = {
    "3C589", el3_up_ro, el3_stop, el3_mac_ro, el3_send, el3_recv, el3_why
};
