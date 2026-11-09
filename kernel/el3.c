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

static int  s_up;
static int  s_sock;
static u16  s_io;
static u8   s_mac[6];
static u16  s_prod;
static u16  s_manf;

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
int el3_start(int sock, u32 cor, u16 io)
{
    u16 w;
    int i;

    s_up = 0;
    s_sock = sock;
    s_io = io;

    if (!pcic_present(sock)) return 0;
    if (!pcic_power_up(sock, 1)) return 0;

    /* Reset the card through its own configuration register first, so it
     * does not matter what state anything left it in. */
    pcic_attr_write(sock, cor, 0x80);
    delay_ms(20);
    pcic_attr_write(sock, cor, 0x00);
    delay_ms(20);

    if (!pcic_map_io(sock, io, 16)) return 0;
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
    if (st() == 0xFFFF) return 0;

    if (!ee_read(EE_MANF_ID, &s_manf)) return 0;
    if (!ee_read(EE_PROD_ID, &s_prod)) return 0;
    if (s_manf != MANF_3COM) return 0;

    /* The address, three words of it, most significant byte first inside
     * each word -- which is the opposite way round from everything else on
     * this machine, so it is spelled out rather than cast. */
    for (i = 0; i < 3; i++) {
        if (!ee_read(EE_ADDR0 + i, &w)) return 0;
        s_mac[i * 2]     = (u8)(w >> 8);
        s_mac[i * 2 + 1] = (u8)(w & 0xFF);
    }

    /* The card does not put its own address into its receive filter on its
     * own; it has to be written back in through window 2. */
    win(2);
    for (i = 0; i < 6; i++)
        outb((u16)(s_io + W2_ADDR + i), s_mac[i]);

    /* Quiet everything down and leave it in the window it runs in.  No
     * interrupts, no statistics, nothing being received yet -- that is the
     * next piece of work, and a card that is receiving into a FIFO nobody
     * empties is a card that wedges. */
    outw((u16)(s_io + EL3_CMD), C_SET_MASK | 0);
    outw((u16)(s_io + EL3_CMD), C_SET_ZERO | 0);
    outw((u16)(s_io + EL3_CMD), C_ACK_INTR | 0x7FF);
    outw((u16)(s_io + EL3_CMD), C_STATS_DIS);
    outw((u16)(s_io + EL3_CMD), C_RX_DIS);
    outw((u16)(s_io + EL3_CMD), C_TX_DIS);
    win(1);

    s_up = 1;
    return 1;
}

void el3_stop(void)
{
    if (!s_up) return;
    outw((u16)(s_io + EL3_CMD), C_RESET);
    s_up = 0;
}
