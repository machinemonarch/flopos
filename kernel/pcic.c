/* ===========================================================================
 * pcic.c -- the PC Card sockets, and what is sitting in them.
 *
 * The 430CDT has two stacked 16-bit 5V PC Card slots.  Sixteen-bit only:
 * the CardBus slots on this family are in the optional port replicator,
 * not in the machine, which settles which of the three cards in the drawer
 * can ever work here.  Toshiba shipped SystemSoft Card and Socket Services
 * with it, and Socket Services only ever spoke to host adapters that look
 * like an Intel 82365SL, so that is what this talks to and what it checks
 * for before it believes anything.
 *
 * The adapter is a pair of ports: write a register number to the index
 * port, read or write the value at the data port.  Each socket gets 0x40
 * registers of its own, so socket n's register r is at n*0x40 + r.  That
 * is the whole interface, and it has not changed since 1990.
 *
 * Nothing here is taken from anybody's driver.  The register numbers and
 * the bit meanings are the published ExCA interface -- facts about a chip,
 * the same way a port number is a fact -- and the code around them is ours.
 *
 * What this file does, and deliberately no more: find the adapter, say
 * what is in each socket, power a card up, and read its CIS so the card
 * can identify itself in its own words.  Driving the card is a separate
 * problem and belongs in a separate file.
 * ===========================================================================
 */
#include "flopos.h"
#include "io.h"

/* ---- the ExCA register set ------------------------------------------- */
#define R_IDENT      0x00                /* chip identity and revision     */
#define R_STATUS     0x01                /* what the socket can see        */
#define R_POWER      0x02                /* Vcc, Vpp, and the buffers      */
#define R_INTCTL     0x03                /* reset, card type, IRQ          */
#define R_CSC        0x04                /* what has changed since asked   */
#define R_CSCINT     0x05
#define R_WINDOWS    0x06                /* which windows are live         */
#define R_IOCTL      0x07
#define R_IO(m)      (0x08 + 4 * (m))    /* two I/O windows                */
#define R_MEM(m)     (0x10 + 8 * (m))    /* five memory windows            */

/* R_STATUS */
#define ST_BVD1      0x01
#define ST_BVD2      0x02
#define ST_CD1       0x04                /* both detects, or no card       */
#define ST_CD2       0x08
#define ST_WRPROT    0x10
#define ST_READY     0x20
#define ST_POWERED   0x40

/* R_POWER */
#define PW_VCC_5V    0x10
#define PW_VCC_3V    0x18
#define PW_VPP_5V    0x04
#define PW_AUTO      0x20
#define PW_OUTPUT    0x80                /* let the buffers drive the card */

/* R_INTCTL.  Reset is active low, so the bit is set to let go of it. */
#define IC_IRQ_MASK  0x0F
#define IC_INTR_ENA  0x10
#define IC_IOCARD    0x20
#define IC_NORESET   0x40

/* R_WINDOWS */
#define WIN_MEM(m)   (0x01 << (m))
#define WIN_IO(m)    (0x40 << (m))

/* The high byte of a memory window's card offset.  REG picks attribute
 * memory -- a separate address space from the card's common memory, where
 * the card keeps its own description. */
#define MO_REG       0x40
#define MO_WRPROT    0x80
#define MW_16BIT     0x80                /* in the start register's high   */

/* Where card attribute memory is made to appear.  Upper memory between the
 * video BIOS and the system BIOS, which on this machine is nothing at all,
 * and we only hold the window open for as long as it takes to read. */
#define CIS_HOST     0x000D0000u
#define CIS_PAGES    0x10                /* 64K, one window's worth        */

static u16 s_index;                      /* 0 until something is found     */
static int s_sockets;
static u8  s_ident[4];

/* ---- talking to it ---------------------------------------------------- */
static u8 pc_get(int sock, int reg)
{
    outb(s_index, (u8)(sock * 0x40 + reg));
    return inb((u16)(s_index + 1));
}

static void pc_set(int sock, int reg, u8 v)
{
    outb(s_index, (u8)(sock * 0x40 + reg));
    outb((u16)(s_index + 1), v);
}

/* ---- finding the adapter ----------------------------------------------
 * An absent adapter floats its data port, which reads back as 0xFF on this
 * bus, and a present one answers register 0 with 0b10xxxxxx -- the top two
 * bits are the part's signature and have been since the 82365SL.  Two
 * sockets per chip, and the pair of chips that a four-socket machine would
 * have sit at consecutive index ports.  We look at the two that were ever
 * used and stop at the first that answers.
 */
static int plausible(u8 id)
{
    return (id & 0xC0) == 0x80 && id != 0xFF;
}

/* The ports an ExCA adapter has ever been found at.  Three, because a
 * machine with four sockets puts the second chip next door, and because a
 * handful of parts were strapped one step further along. */
static const u16 s_cand[] = { 0x3E0, 0x3E2, 0x3E4 };
#define NCAND (int)(sizeof s_cand / sizeof s_cand[0])

/* What every candidate answered, whether or not it was believed.  This is
 * here because the probe runs on a machine I cannot put my hands on, and
 * "nothing found" is not a useful thing to be told from the other end of a
 * serial cable: the bytes are. */
int pcic_scan(u16 *ports, u8 *ids, int max)
{
    u16 save = s_index;
    int i;

    for (i = 0; i < NCAND && i < max; i++) {
        s_index = s_cand[i];
        ports[i] = s_cand[i];
        ids[i] = pc_get(0, R_IDENT);
    }
    s_index = save;
    return i < max ? i : max;
}

int pcic_probe(void)
{
    int c;

    s_index = 0;
    s_sockets = 0;

    for (c = 0; c < NCAND; c++) {
        u8 a, b;
        s_index = s_cand[c];
        a = pc_get(0, R_IDENT);
        b = pc_get(1, R_IDENT);
        if (plausible(a)) {
            s_ident[0] = a;
            s_sockets = 1;
            if (plausible(b)) { s_ident[1] = b; s_sockets = 2; }
            return s_sockets;
        }
    }

    s_index = 0;
    return 0;
}

int  pcic_sockets(void)   { return s_sockets; }
u16  pcic_port(void)      { return s_index; }
u8   pcic_ident(int sock) { return (sock >= 0 && sock < 4) ? s_ident[sock] : 0; }

/* The identification byte names the part.  The 82365SL's own datasheet
 * describes bits 5:4 as saying whether the socket does I/O as well as
 * memory, but no real part in this family ever populated them -- a genuine
 * 82365SL answers 0x82 or 0x83, which by that reading would be "memory
 * only" while happily running a network card.  So the bits are ignored and
 * the whole byte is matched instead, which is what the values are actually
 * good for.  Anything unrecognised is still spoken to: the register
 * interface is the same, and the raw byte is printed either way. */
const char *pcic_kind(u8 id)
{
    switch (id) {
    case 0x82: case 0x83: return "82365SL";
    case 0x84: case 0x85: return "82365SL, VLSI part";
    case 0x88: case 0x89: return "VG-468";
    case 0x8A:            return "VG-469";
    default:              return "ExCA compatible";
    }
}

int pcic_present(int sock)
{
    u8 st;
    if (!s_index) return 0;
    st = pc_get(sock, R_STATUS);
    return (st & (ST_CD1 | ST_CD2)) == (ST_CD1 | ST_CD2);
}

u8 pcic_status(int sock)
{
    return s_index ? pc_get(sock, R_STATUS) : 0;
}

/* ---- bringing a card up -----------------------------------------------
 * Vcc first, then wait, then let go of reset, then wait again.  The two
 * waits are not superstition: the standard gives a card 300ms of grace
 * after power and 20ms after reset before it has to answer for itself, and
 * a card read too early answers with rubbish that looks exactly like a
 * card that is not there.
 */
int pcic_power_up(int sock, int io_card)
{
    int tries;

    if (!s_index || !pcic_present(sock)) return 0;

    pc_set(sock, R_WINDOWS, 0);                       /* nothing mapped   */
    pc_set(sock, R_POWER, PW_VCC_5V | PW_OUTPUT);     /* 5V: this machine */
    delay_ms(300);

    pc_set(sock, R_INTCTL, (u8)(IC_NORESET | (io_card ? IC_IOCARD : 0)));
    delay_ms(20);

    /* READY goes high when the card has finished thinking about it. */
    for (tries = 0; tries < 100; tries++) {
        if (pc_get(sock, R_STATUS) & ST_READY) return 1;
        delay_ms(10);
    }
    return 0;
}

void pcic_power_down(int sock)
{
    if (!s_index) return;
    pc_set(sock, R_WINDOWS, 0);
    pc_set(sock, R_INTCTL, 0);
    pc_set(sock, R_POWER, 0);
}

/* ---- the card's own description ---------------------------------------
 * A card keeps its CIS in attribute memory, which is a separate address
 * space reached by setting one bit in the window's offset register, and in
 * which only the even bytes mean anything -- the odd ones are not wired.
 * So a window mapped at CIS_HOST shows the CIS stretched to twice its
 * length, and reading it means taking every other byte.
 */
static void map_attr(int sock)
{
    u32 start = CIS_HOST >> 12;
    u32 stop  = (CIS_HOST + (CIS_PAGES << 12) - 1) >> 12;

    pc_set(sock, R_MEM(0) + 0, (u8)(start & 0xFF));
    pc_set(sock, R_MEM(0) + 1, (u8)((start >> 8) & 0x0F));
    pc_set(sock, R_MEM(0) + 2, (u8)(stop & 0xFF));
    pc_set(sock, R_MEM(0) + 3, (u8)((stop >> 8) & 0x0F));
    /* Card address zero under host address CIS_HOST: the offset the chip
     * wants is the difference, in pages, truncated to its fourteen bits. */
    pc_set(sock, R_MEM(0) + 4, (u8)((-(s32)start) & 0xFF));
    pc_set(sock, R_MEM(0) + 5, (u8)((((-(s32)start) >> 8) & 0x3F) | MO_REG));
    pc_set(sock, R_WINDOWS, WIN_MEM(0));
}

static void unmap(int sock)
{
    pc_set(sock, R_WINDOWS, 0);
}

static u8 attr_byte(u32 off)
{
    /* Even bytes only, so the nth byte of the CIS is at twice n. */
    return *(volatile u8 *)(CIS_HOST + off * 2);
}

/* The CIS as bytes, for when it is the interpretation that is in doubt and
 * not the reading.  The card must already be powered. */
int pcic_cis_read(int sock, u32 off, u8 *buf, int len)
{
    int i;

    if (!s_index || !pcic_present(sock)) return 0;

    map_attr(sock);
    for (i = 0; i < len; i++) buf[i] = attr_byte(off + (u32)i);
    unmap(sock);
    return len;
}

/* Walk the tuple chain, handing each one to the caller.  A tuple is a code
 * byte, a length byte, and that many bytes of body; 0xFF ends the chain and
 * 0x00 is padding.  The cap on the walk is there because a card that is
 * half seated reads as an endless run of one value and would otherwise
 * take the machine with it. */
int pcic_cis_walk(int sock, CisFn fn, void *ctx)
{
    u32 off = 0;
    int n = 0;
    u8 body[CIS_MAX_BODY];

    if (!s_index || !pcic_present(sock)) return -1;

    map_attr(sock);

    while (n < 64 && off < 0x800) {
        u8 code = attr_byte(off++);
        u8 len, i;

        if (code == 0xFF) break;             /* the end of the chain       */
        if (code == 0x00) continue;          /* padding                    */

        len = attr_byte(off++);
        if (len == 0xFF) break;

        for (i = 0; i < len && i < CIS_MAX_BODY; i++)
            body[i] = attr_byte(off + i);
        off += len;

        n++;
        if (fn && fn(code, body, len > CIS_MAX_BODY ? CIS_MAX_BODY : len, ctx))
            break;
    }

    unmap(sock);
    return n;
}
