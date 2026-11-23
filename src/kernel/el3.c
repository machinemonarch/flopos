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
#define C_SET_MASK  CMD(14)                /* which events interrupt     */
#define C_SET_STATUS CMD(15)               /* which events are visible   */
#define C_RX_FILTER CMD(16)
#define C_STATS_DIS CMD(22)
#define C_STOP_COAX CMD(23)

/* Window 0: how the card is set up. */
#define W0_CONFIG   0x04                 /* bit 0 enables the adapter      */
#define W0_ADDRCONF 0x06                 /* top two bits pick the xcvr     */
#define W0_RESOURCE 0x08
#define W0_EE_CMD   0x0A
#define W0_EE_DATA  0x0C

/* Window 4: the wire itself. */
#define W4_MEDIA    0x0A
#define MEDIA_LED      0x0001            /* the link light, on 589E cards  */
#define MEDIA_TP       0x00C0            /* link beat and jabber checking  */
#define MEDIA_LINKBEAT 0x0800            /* somebody is at the other end   */
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
    u32 t0 = tsc32();
    outw((u16)(s_io + EL3_CMD), C_RX_DISCARD);
    for (guard = 0; !spun(t0, (u32)guard, 10000); guard++)   /* 10 ms, from the timer too */
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
        if (pcic_claimed(sock)) continue;
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

/* Issue a command the card has to chew on, and wait for it to finish.
 * Bit 12 of the status word stays up while it is busy; a reset takes the
 * best part of a millisecond and the card ignores anything sent meanwhile. */
static int el3_cmd_wait(u16 cmd)
{
    int guard;
    outw((u16)(s_io + EL3_CMD), cmd);
    for (guard = 0; guard < 2000; guard++) {
        if (!(st() & S_CMD_BUSY)) return 1;
        delay_ms(1);
    }
    return 0;
}

/* Give up cleanly: unclaimed, unmapped, unpowered.  A card left switched
 * on at 0x300 after a failure is a card some other probe will poke. */
static int el3_fail(int sock, const char *why)
{
    s_why = why;
    s_up = 0;
    pcic_claim(sock, 0);
    pcic_power_down(sock);
    return 0;
}

int el3_start(int sock, u32 cor, u16 io)
{
    u16 w;
    int i;

    s_up = 0;
    s_sock = sock;
    s_io = io;

    if (!pcic_present(sock)) { s_why = "the socket is empty"; return 0; }
    pcic_claim(sock, 1);
    if (!pcic_power_up(sock, 1))
        return el3_fail(sock, "the card never came ready after power");

    /* The socket side first: sixteen of the machine's I/O addresses, sent
     * to the card.  Nothing touched afterwards may switch this off again,
     * which an earlier version did every time it wrote the register
     * below. */
    if (!pcic_map_io(sock, io, 16))
        return el3_fail(sock, "the socket would not take an I/O window");
    pcic_set_irq(sock, 0);               /* polled                          */

    /* The card side: the number of the configuration it marked as its
     * default, written into its configuration option register.  Until this
     * happens the card is a memory card and ignores I/O altogether. */
    pcic_attr_write(sock, cor, 0x01);
    delay_ms(20);

    /* Answering at all?  Nothing at an address reads as all ones.  If it
     * is not, the option register's own value goes into the reason: "it
     * did not take the write" and "it took it and still says nothing" are
     * different faults with different fixes. */
    if (st() == 0xFFFF) {
        static char why[80];
        fmt_str(why, sizeof why,
                "nothing answers at %x (option register reads %x)",
                (u32)io, (u32)pcic_attr_read(sock, cor));
        return el3_fail(sock, why);
    }

    if (!ee_read(EE_MANF_ID, &s_manf)) return el3_fail(sock, "its eeprom never answered");
    if (!ee_read(EE_PROD_ID, &s_prod)) return el3_fail(sock, "its eeprom stopped halfway");
    if (s_manf != MANF_3COM) return el3_fail(sock, "the eeprom says it is not a 3Com");

    /* The address, three words of it, most significant byte first inside
     * each word -- the opposite way round from everything else on this
     * machine, so it is spelled out rather than cast. */
    for (i = 0; i < 3; i++) {
        if (!ee_read(EE_ADDR0 + i, &w))
            return el3_fail(sock, "it would not give up its address");
        s_mac[i * 2]     = (u8)(w >> 8);
        s_mac[i * 2 + 1] = (u8)(w & 0xFF);
    }

    /* Window 0 again, for the three registers that make it a working
     * adapter rather than an answering one.  Enable is the one that was
     * missing: without it the card reads its EEPROM quite happily and
     * never puts a single frame on the wire.  The resource register has to
     * hold that particular value on the PC Card version -- the interrupt
     * field is not loaded from anywhere and is left to the driver.  And
     * the transceiver is the twisted pair, which is what the dongle on the
     * end of a 3C589D is for. */
    win(0);
    outw((u16)(s_io + W0_CONFIG), 0x0001);        /* enable the adapter     */
    outw((u16)(s_io + W0_RESOURCE), 0x3F00);
    outw((u16)(s_io + W0_ADDRCONF), 0x0000);      /* 10BASE-T               */

    /* Its own address into its receive filter, through window 2: the card
     * does not do that for itself. */
    win(2);
    for (i = 0; i < 6; i++)
        outb((u16)(s_io + W2_ADDR + i), s_mac[i]);

    /* Twisted pair wants link beat and jabber checking switched on, and
     * the coax converter off. */
    outw((u16)(s_io + EL3_CMD), C_STOP_COAX);
    win(4);
    outw((u16)(s_io + W4_MEDIA), MEDIA_TP | MEDIA_LED);

    /* Statistics off and cleared by reading them, so the counters never
     * fill and ask for attention nobody is listening for. */
    outw((u16)(s_io + EL3_CMD), C_STATS_DIS);
    win(6);
    for (i = 0; i < 10; i++) (void)inb((u16)(s_io + i));
    (void)inw((u16)(s_io + 10));
    (void)inw((u16)(s_io + 12));

    /* Both directions reset and then switched on.  No interrupts --
     * everything is polled -- but every status bit visible, since a status
     * bit masked to zero is one nobody can ever see go up. */
    win(1);
    if (!el3_cmd_wait(C_RX_RESET) || !el3_cmd_wait(C_TX_RESET))
        return el3_fail(sock, "it never finished resetting");
    outw((u16)(s_io + EL3_CMD), C_SET_MASK | 0);
    outw((u16)(s_io + EL3_CMD), C_SET_STATUS | 0xFF);
    outw((u16)(s_io + EL3_CMD), C_ACK_INTR | 0xFF);

    /* Listen for things addressed to us and for things addressed to
     * everybody, and nothing else.  Promiscuous would be easier and is
     * exactly the sort of easier that ends up shipped. */
    outw((u16)(s_io + EL3_CMD), C_RX_FILTER | RXF_STATION | RXF_BROADCAST);
    outw((u16)(s_io + EL3_CMD), C_RX_ENA);
    outw((u16)(s_io + EL3_CMD), C_TX_ENA);

    s_why = "up";
    s_up = 1;
    return 1;
}

/* Whether the twisted pair can hear a hub or switch at the other end.  A
 * card can be perfectly configured and plugged into nothing. */
int el3_link(void)
{
    u16 m;
    if (!s_up) return 0;
    win(4);
    m = inw((u16)(s_io + W4_MEDIA));
    win(1);
    return (m & MEDIA_LINKBEAT) != 0;
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
    u32 t0;

    if (!s_up) return 0;
    win(1);

    /* Room for the frame and its preamble, or wait until there is.  The
     * card frees space as the wire drains, so this is short or never. */
    for (guard = 0, t0 = tsc32(); ; guard++) {   /* a frame drains in 1.2 ms at 10 Mbit */
        if (inw((u16)(s_io + W1_TX_FREE)) >= len + 4) break;
        if (spun(t0, (u32)guard, 3000)) return 0;
    }

    outw((u16)(s_io + W1_TX_FIFO), (u16)len);
    outw((u16)(s_io + W1_TX_FIFO), 0);
    outsw((u16)(s_io + W1_TX_FIFO), frame, len / 2);
    if (len & 1) outw((u16)(s_io + W1_TX_FIFO), frame[len - 1]);      /* never a byte past it */
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
    int len;

    if (!s_up) return 0;
    win(1);

    rs = inw((u16)(s_io + W1_RX_STAT));
    if (rs & RX_INCOMPLETE) return 0;    /* nothing whole yet              */

    if (rs & RX_ERROR) { el3_discard(); return 0; }

    len = rs & 0x7FF;
    /* It comes out in two-byte words, so an odd length needs one more. */
    if (len <= 0 || len + (len & 1) > max) { el3_discard(); return 0; }

    insw((u16)(s_io + W1_RX_FIFO), frame, (len + 1) / 2);
    el3_discard();
    return len;
}

void el3_stop(void)
{
    if (!s_up) return;
    outw((u16)(s_io + EL3_CMD), C_RX_DIS);
    outw((u16)(s_io + EL3_CMD), C_TX_DIS);
    s_up = 0;
    pcic_claim(s_sock, 0);
    pcic_power_down(s_sock);
}

static const u8 *el3_mac_ro(void) { return s_mac; }
static int el3_up_ro(void) { return el3_find(); }

const Nic g_nic_el3 = {
    "3C589", el3_up_ro, el3_stop, el3_mac_ro, el3_send, el3_recv, el3_why,
    el3_link
};
