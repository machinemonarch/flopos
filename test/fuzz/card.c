/* card.c -- the two network card drivers (src/kernel/ne2000.c, el3.c)
 * behind a pretend card that says whatever the input says.
 *
 * A card is hardware, but what it hands over -- a frame's length, where
 * the next one starts, how far it has filled its ring -- comes from the
 * wire, and QEMU's NE2000 has been seen to write over its own ring.  So
 * the drivers must take none of it on trust.  The mode picks the card
 * (bit 0: the 3C589, else the NE2000) and how big a buffer each frame is
 * read into (bits 1-3: odd sizes too, so a frame one byte too long shows);
 * the input is the card's memory and the numbers its registers give.
 * Every frame is read into a buffer exactly that size, under the memory
 * checker, and the driver's own idea of the ring must stay inside it.
 */
#include "flopos.h"
#include <stdlib.h>
#include <string.h>

const char *fuzz_name = "card";
const u32 fuzz_max = 20 * 1024;

static const u8 *s_in;
static u32 s_len, s_at, s_mode;
static int s_el3;                        /* which card is pretended        */

static u8 nextb(void) { return s_at < s_len ? s_in[s_at++] : 0; }

/* What the kernel would have given. */
u32  g_cpu_mhz;                          /* 0: hardware waits count turns  */
volatile u32 g_ticks;
int  spun(u32 start, u32 turn, u32 us) { (void)start; return turn >= us; }
void delay_ms(u32 ms) { (void)ms; }
void serial_print(const char *s) { (void)s; }
void serial_printf(const char *fmt, ...) { (void)fmt; }
int  pcic_probe(void) { return 1; }
int  pcic_present(int sock) { (void)sock; return 1; }
int  pcic_claimed(int sock) { (void)sock; return 0; }
void pcic_claim(int sock, int on) { (void)sock; (void)on; }
int  pcic_power_up(int sock, int on) { (void)sock; (void)on; return 1; }
void pcic_power_down(int sock) { (void)sock; }
int  pcic_map_io(int sock, u16 io, int n) { (void)sock; (void)io; (void)n; return 1; }
void pcic_set_irq(int sock, int irq) { (void)sock; (void)irq; }
void pcic_attr_write(int sock, u32 a, u8 v) { (void)sock; (void)a; (void)v; }
u8   pcic_attr_read(int sock, u32 a) { (void)sock; (void)a; return 1; }
int  pcic_cis_walk(int sock, int (*fn)(u8, const u8 *, int, void *), void *ctx) { (void)sock; (void)fn; (void)ctx; return 0; }

/* ---- the NE2000: a DP8390's registers and 32 KB ------------------------- */
static u8  ne_mem[0x8000], ne_cmd, ne_pstart = 0x46, ne_pstop = 0x80;
static u32 ne_rsar;

static u8 ne_read(int r)
{
    int page = ne_cmd >> 6;
    if (r == 0x1F) return 0;
    if (r == 0x00) return ne_cmd;
    if (page == 1 && r == 0x07) return nextb() | ((s_mode & 16) ? 0x40 : 0);   /* CURR: wherever it says */
    if (r == 0x07) return 0x80 | 0x40 | 0x02 | ((nextb() & 7) == 0 ? 0x10 : 0);   /* ISR, now and then full */
    return 0;                            /* RBCR and the rest: not echoed  */
}

static void ne_write(int r, u8 v)
{
    int page = ne_cmd >> 6;
    if (r == 0x00) { ne_cmd = v; return; }
    if (page != 0) return;
    if (r == 0x01) ne_pstart = v;
    if (r == 0x02) ne_pstop = v;
    if (r == 0x08) ne_rsar = (ne_rsar & 0xFF00) | v;
    if (r == 0x09) ne_rsar = (ne_rsar & 0x00FF) | ((u32)v << 8);
}

static u8 ne_byte(void)                  /* through the hatch, wrapping as QEMU's does */
{
    u8 v = ne_mem[ne_rsar & 0x7FFF];
    if (++ne_rsar == (u32)ne_pstop << 8) ne_rsar = (u32)ne_pstart << 8;
    return v;
}

/* ---- the 3C589: status words and a FIFO, from the input ---------------- */
static int el_win;
static u16 el_ee;

static u16 el_read16(int r)
{
    if (r == 0x0E) return 0;                                  /* status: not busy */
    if (el_win == 0 && r == 0x0A) return 0;                   /* eeprom: done   */
    if (el_win == 0 && r == 0x0C) {
        int a = el_ee & 0x3F;
        return a == 7 ? 0x6D50 : a == 3 ? 0x9058 : a == 0 ? 0x0050 : a == 1 ? 0x0412 : 0x3456;
    }
    if (el_win == 1 && r == 0x08) { u16 v = nextb(); return (u16)(v | nextb() << 8); }   /* RX status */
    if (el_win == 1 && r == 0x0C) return 2048;                /* room to send   */
    return 0;
}

/* ---- the ports ------------------------------------------------------------ */
void outb(u16 port, u8 v) { if (!s_el3 && port >= 0x300 && port < 0x320) ne_write(port - 0x300, v); }
u8 inb(u16 port)
{
    if (s_el3) return 0;                 /* TX status: nothing to report    */
    return port >= 0x300 && port < 0x320 ? ne_read(port - 0x300) : 0xFF;
}
void outw(u16 port, u16 v)
{
    if (!s_el3) { ne_write(port - 0x300, (u8)v); return; }
    if (port == 0x30E && (v >> 11) == 1) el_win = v & 7;
    if (port == 0x30A && el_win == 0) el_ee = v;
}
u16 inw(u16 port) { return s_el3 ? el_read16(port - 0x300) : ne_read(port - 0x300); }
void insw(u16 port, void *buf, int n)
{
    u8 *b = (u8 *)buf;
    int i;
    for (i = 0; i < 2 * n; i++) b[i] = s_el3 ? nextb() : (port == 0x310 ? ne_byte() : 0);
}
void outsw(u16 port, const void *buf, int n)
{
    const u8 *b = (const u8 *)buf;
    int i;
    for (i = 0; i < 2 * n; i++) if (!s_el3 && port == 0x310) ne_mem[ne_rsar++ & 0x7FFF] = b[i];
}
void insl(u16 port, void *buf, int n) { insw(port, buf, 2 * n); }
void outsl(u16 port, const void *buf, int n) { outsw(port, buf, 2 * n); }

int el3_start(int sock, u32 cor, u16 io);

void fuzz_one(const u8 *data, u32 len, u32 mode)
{
    static const int sizes[8] = { 1514, 1514, 1513, 61, 60, 14, 1, 1515 };
    const Nic *nic;
    int i, max = sizes[(mode >> 1) & 7];
    u8 *buf;

    s_in = data; s_len = len; s_at = 0; s_mode = mode;
    s_el3 = (int)(mode & 1);
    memset(ne_mem, 0, sizeof ne_mem);
    for (i = 0; i < 6; i++) ne_mem[2 * i] = ne_mem[2 * i + 1] = (u8)(0x52 + i * 2);   /* the PROM's address */
    if (!s_el3) {                        /* the ring, from the input       */
        for (i = 0x4600; i < 0x8000 && s_at < s_len; i++) ne_mem[i] = s_in[s_at++];
        s_at = 0;
    }
    nic = s_el3 ? &g_nic_el3 : &g_nic_ne2000;
    if (s_el3 ? !el3_start(0, 0x3F8, 0x300) : !nic->start()) return;

    buf = malloc((size_t)max);
    for (i = 0; i < 200 && (s_at < s_len || i < 4); i++) {
        int n = nic->recv(buf, max);
        if (n < 0 || n > max) { extern void fuzz_fail(const char *); fuzz_fail("a frame longer than its buffer"); }
    }
    memset(buf, 0x5A, (size_t)max);
    nic->send(buf, max < 60 ? max : 60);
    free(buf);
    nic->stop();
}
