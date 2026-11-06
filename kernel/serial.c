/* ===========================================================================
 * serial.c -- COM1, and the wire out of this machine.
 *
 * The 430CDT's port is 16550 compliant (Toshiba's own spec sheet says so),
 * which means a 16-byte FIFO in both directions.  That matters more than it
 * sounds: without it, every byte sent costs an interrupt or a spin, and
 * mirroring a whole screen of text would be slower than drawing it.
 *
 * Two jobs:
 *
 *   out   Everything FLOPOS prints to its own terminal also goes down the
 *         wire, so whoever is on the other end sees the session live.
 *
 *   in    Characters arriving from the other end are fed to the line editor
 *         exactly as if they had been typed on the laptop's keyboard, so
 *         the far end can drive the machine.
 *
 * Transmission is polled rather than interrupt-driven. The FIFO holds 16
 * bytes and at 115200 baud drains in about 1.4ms, so the spin is short, and
 * it means output works from anywhere -- including the panic handler, with
 * interrupts off and the rest of the kernel in pieces.  Receiving is on
 * IRQ4, because characters arrive when they feel like it.
 * ===========================================================================
 */
#include "flopos.h"
#include "io.h"

#define COM1 0x3F8

#define REG_DATA  0                      /* when DLAB=0: rx / tx           */
#define REG_IER   1                      /* interrupt enable               */
#define REG_DLL   0                      /* when DLAB=1: divisor low       */
#define REG_DLM   1                      /*              divisor high      */
#define REG_IIR   2                      /* interrupt id / FIFO control    */
#define REG_FCR   2
#define REG_LCR   3                      /* line control                   */
#define REG_MCR   4                      /* modem control                  */
#define REG_LSR   5                      /* line status                    */
#define REG_MSR   6

#define LSR_RX_READY 0x01
#define LSR_TX_EMPTY 0x20

#define RXLEN 256

static volatile u8  s_rx[RXLEN];
static volatile u16 s_head, s_tail;
static int s_present;
static int s_echo = 1;                   /* mirror the terminal out        */
static u32 s_sent, s_got;

static u8 rd(int reg) { return inb((u16)(COM1 + reg)); }
static void wr(int reg, u8 v) { outb((u16)(COM1 + reg), v); }

int serial_present(void) { return s_present; }
u32 serial_sent(void)    { return s_sent; }
u32 serial_received(void){ return s_got; }
int serial_echo(void)    { return s_echo; }
void serial_set_echo(int on) { s_echo = on; }

/* ---- bring the port up ------------------------------------------------ */
void serial_init(void)
{
    u8 probe;

    s_head = s_tail = 0;

    /* Is anything there?  The scratch register at offset 7 exists on a
     * real UART and holds whatever you put in it.  On an empty address the
     * read comes back off an open bus and will not match. */
    outb(COM1 + 7, 0x5A);
    io_wait();
    probe = inb(COM1 + 7);
    outb(COM1 + 7, 0xA5);
    io_wait();
    s_present = (probe == 0x5A) && (inb(COM1 + 7) == 0xA5);
    if (!s_present) return;

    wr(REG_IER, 0x00);                   /* no interrupts while we set up  */
    wr(REG_LCR, 0x80);                   /* DLAB: the divisor is visible   */
    wr(REG_DLL, 0x01);                   /* 115200 = 1843200 / 16 / 1      */
    wr(REG_DLM, 0x00);
    wr(REG_LCR, 0x03);                   /* 8 bits, no parity, 1 stop      */
    wr(REG_FCR, 0xC7);                   /* FIFOs on, cleared, trigger 14  */
    wr(REG_MCR, 0x0B);                   /* DTR, RTS, and OUT2 -- without
                                          * OUT2 the IRQ never reaches the
                                          * interrupt controller at all    */
    wr(REG_IER, 0x01);                   /* interrupt when a byte arrives  */

    (void)rd(REG_LSR);                   /* clear any stale status         */
    (void)rd(REG_DATA);
}

/* ---- out -------------------------------------------------------------- */
void serial_putc(int c)
{
    u32 guard = 0;

    if (!s_present) return;

    /* A dead or unplugged line must never hang the machine, so the wait
     * for room in the FIFO is bounded. */
    while (!(rd(REG_LSR) & LSR_TX_EMPTY))
        if (++guard > 100000) return;

    if (c == '\n') {                     /* the far end expects CRLF       */
        wr(REG_DATA, '\r');
        guard = 0;
        while (!(rd(REG_LSR) & LSR_TX_EMPTY))
            if (++guard > 100000) return;
    }
    wr(REG_DATA, (u8)c);
    s_sent++;
}

void serial_print(const char *s)
{
    while (*s) serial_putc((u8)*s++);
}

void serial_printf(const char *fmt, ...)
{
    char buf[192];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    fmt_vstr(buf, sizeof buf, fmt, ap);
    __builtin_va_end(ap);
    serial_print(buf);
}

/* ---- in --------------------------------------------------------------- */
void serial_irq(void)
{
    int guard = 0;

    while ((rd(REG_LSR) & LSR_RX_READY) && ++guard < 64) {
        u8 c = rd(REG_DATA);
        u16 nxt = (u16)((s_head + 1) % RXLEN);
        if (nxt != s_tail) { s_rx[s_head] = c; s_head = nxt; }
        s_got++;
    }
}

int serial_getc(void)
{
    int c;
    if (s_tail == s_head) return -1;
    c = s_rx[s_tail];
    s_tail = (u16)((s_tail + 1) % RXLEN);
    return c;
}
