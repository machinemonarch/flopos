/* net.c -- the network's front door (src/kernel/net.c, ip.c, tcp.c) given
 * garbage: everything a stranger's frame touches first.
 *
 * An input is frames, each a 16-bit length then that many bytes, handed
 * over by a pretend card one at a time as FLOPOS asks for them.  The mode
 * picks what FLOPOS is doing meanwhile (bits 0-1): asking DHCP for an
 * address, looking up a name, holding a TCP conversation, or pinging and
 * asking the time.  Bit 2 makes the pretend card helpful, the way a real
 * server is: each frame is addressed to what FLOPOS just sent (addresses,
 * ports, the DHCP and DNS question numbers, TCP's acknowledgement) and its
 * checksums (IP, TCP, ping) put right, so garbage gets past the doors to
 * the readers behind them.  Bit 3 also puts the IP and UDP lengths right.
 * The card answers every ARP question at once, and hands over the input's
 * frames only after FLOPOS has said something, the way answers come after
 * questions: three for each IP packet it sends, or (bit 4, patient) one
 * for each that is more than a bare acknowledgement.  Bit 5 moves the
 * clock on two minutes with every frame, for what only happens late:
 * addresses asked again, a window asked about, a conversation given up.
 * Bit 6: nobody answers ARP; bit 7: answers come from changing cards.
 * Bit 8: FLOPOS hangs up first; bit 9: it sends more than the other end
 * takes, then waits long enough for TCP to give up.  Bit 10: settings
 * typed by hand are on the floppy.  The input's start is also read as an
 * address typed by hand.
 * What is left of the input comes at the end.
 */
#include "flopos.h"
#include <stdlib.h>
#include <string.h>

const char *fuzz_name = "net";
const u32 fuzz_max = 16 * 1024;

static const u8 *s_in;
static u32 s_len, s_at, s_mode;
static u32 s_now, s_rand = 1;
static u8 s_last_udp[NET_MTU], s_last_tcp[NET_MTU], s_arp[60];
static int s_last_udp_len, s_last_tcp_len, s_arp_len, s_credit;
static const u8 s_mac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };

/* What the kernel would have given. */
volatile u32 g_ticks = 12345;
u32  g_cpu_mhz;
void *halloc(usize bytes) { return calloc(1, bytes ? bytes : 1); }
u32  ms_now(void) { return s_now += 7; }
void rand_add(u32 v) { (void)v; }
void rand_bytes(u8 *out, int n)
{
    int i;
    for (i = 0; i < n; i++) { s_rand ^= s_rand << 13; s_rand ^= s_rand >> 17; s_rand ^= s_rand << 5; out[i] = (u8)(s_rand >> 9); }
}
void serial_print(const char *s) { (void)s; }
void serial_printf(const char *fmt, ...) { (void)fmt; }
int  store_net_get(u32 *how, u32 *ip, u32 *mask, u32 *gw, u32 *dns)    /* bit 10: settings kept by hand */
{
    if (!(s_mode & 1024)) return 0;
    *how = 1; *ip = 0x0A00020Fu; *mask = 0xFFFFFF00u; *gw = 0x0A000202u; *dns = 0x0A000203u;
    return 1;
}
int  clock_from_network(void) { return 0; }
void main_pump(int now) { (void)now; }
int  pcic_probe(void) { return 0; }
const char *ne2000_state(void) { return "fuzz"; }

/* TCP is built into this file, so its state can be checked against its
 * rules after every frame: what it holds fits, what it has sent is what
 * it has, the pieces kept ahead are in order, apart, and inside the
 * buffer, and the numbers it sends by stay in their bounds. */
#include "../../src/kernel/tcp.c"
void fuzz_fail(const char *why);

static void tcp_rules(void)
{
    int k;
    u32 held = s_rx_in - s_rx_out;
    if (s_state == CLOSED || s_state == SYN_SENT) return;
    if (held > RX_SIZE) fuzz_fail("more held than the buffer");
    if (s_tx_len < 0 || s_tx_len > TX_SIZE) fuzz_fail("send buffer count out of bounds");
    if (before(s_snd_nxt, s_snd_una)) fuzz_fail("sent before acknowledged");
    if (s_snd_nxt - s_snd_una > (u32)s_tx_len + 2) fuzz_fail("more on the way than was written");
    if (s_nooo < 0 || s_nooo > OOO_MAX) fuzz_fail("too many pieces ahead");
    for (k = 0; k < s_nooo; k++) {
        if (!before(s_ooo[k].start, s_ooo[k].end)) fuzz_fail("an empty or backwards piece");
        if (!before(s_rcv_nxt, s_ooo[k].start)) fuzz_fail("a piece ahead that is not ahead");
        if (s_ooo[k].end - s_rcv_nxt > RX_SIZE - held) fuzz_fail("a piece past the buffer");
        if (k && !before(s_ooo[k - 1].end, s_ooo[k].start)) fuzz_fail("pieces touching or out of order");
    }
    if (s_peer_mss < 64 || s_peer_mss > MSS) fuzz_fail("their segment size out of bounds");
    if (s_rto < RTO_MIN || s_rto > RTO_MAX) fuzz_fail("resend wait out of bounds");
    if (!s_cwnd || s_cwnd > 65535 + 3 * MSS + 65535) fuzz_fail("congestion window out of bounds");
}

static u16 g16(const u8 *p) { return (u16)(p[0] << 8 | p[1]); }
static void p16(u8 *p, u32 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
static u32 g32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static void p32(u8 *p, u32 v) { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }

/* ---- the pretend card -------------------------------------------------- */
static int c_start(void) { return 1; }
static void c_stop(void) { }
static const u8 *c_mac(void) { return s_mac; }
static const char *c_why(void) { return "fuzz"; }
static int c_link(void) { return 1; }
static int c_none(void) { return 0; }

static int c_send(const u8 *f, int len)
{
    if (len >= 34 && g16(f + 12) == 0x0800) {            /* an answer comes after a question */
        int bare = f[23] == 6 && len >= 54 && f[47] == 0x10 && g16(f + 16) == 20 + (f[46] >> 4) * 4;
        s_credit += !(s_mode & 16) ? 3 : bare ? 0 : 1;
    }
    if (len >= 42 && g16(f + 12) == 0x0806 && g16(f + 20) == 1 && !(s_mode & 64)) {   /* who has: it is us */
        memcpy(s_arp, f, 42);
        memcpy(s_arp, s_mac, 6); memcpy(s_arp + 6, "\x02\x00\x00\x00\x00\x01", 6);
        p16(s_arp + 20, 2);
        memcpy(s_arp + 22, "\x02\x00\x00\x00\x00\x01", 6); memcpy(s_arp + 28, f + 38, 4);
        memcpy(s_arp + 32, s_mac, 6); memcpy(s_arp + 38, f + 28, 4);
        if (s_mode & 128) s_arp[27] ^= (u8)s_now;        /* now and then from another card */
        s_arp_len = 42;
    }
    if (len >= 34 && g16(f + 12) == 0x0800) {
        if (f[23] == 17) { memcpy(s_last_udp, f, (size_t)len); s_last_udp_len = len; }
        if (f[23] == 6)  { memcpy(s_last_tcp, f, (size_t)len); s_last_tcp_len = len; }
    }
    return len;
}

static u16 tsum(const u8 *ip, const u8 *seg, int len)
{
    u32 s = g16(ip + 12) + g16(ip + 14) + g16(ip + 16) + g16(ip + 18) + 6 + (u32)len;
    int i;
    for (i = 0; i + 1 < len; i += 2) s += g16(seg + i);
    if (i < len) s += (u32)seg[i] << 8;
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return (u16)~s;
}

static u16 isum(const u8 *p, int len)
{
    u32 s = 0;
    int i;
    for (i = 0; i + 1 < len; i += 2) s += g16(p + i);
    if (i < len) s += (u32)p[i] << 8;
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return (u16)~s;
}

/* A frame made to answer what was just sent, where it can. */
static void helpful(u8 *f, int len)
{
    u8 *h = f + 14;
    int hl;
    if (len < 34 || g16(f + 12) != 0x0800) return;
    hl = (h[0] & 15) * 4;
    if (hl < 20 || 14 + hl + 8 > len) return;
    memcpy(f, s_mac, 6);
    if (s_mode & 8) p16(h + 2, (u32)(len - 14));
    if (h[9] == 17 && s_last_udp_len >= 42) {
        const u8 *q = s_last_udp + 34;
        u8 *u = h + hl;
        memcpy(h + 12, s_last_udp + 30, 4);              /* from whom we asked */
        if (g32(s_last_udp + 30) != 0xFFFFFFFFu) memcpy(h + 16, s_last_udp + 26, 4);
        p16(u, g16(q + 2)); p16(u + 2, g16(q));
        if (s_mode & 8) p16(u + 4, (u32)(len - 14 - hl));
        if (g16(q) == 68 && 14 + hl + 8 + 8 <= len && s_last_udp_len >= 42 + 8)
            memcpy(u + 8 + 4, q + 8 + 4, 4);             /* DHCP: the question's number */
        else if (14 + hl + 8 + 2 <= len)
            memcpy(u + 8, q + 8, 2);                     /* DNS: the question's number */
    }
    if (h[9] == 6 && s_last_tcp_len >= 54) {
        const u8 *ts = s_last_tcp + 34;
        u8 *t = h + hl;
        int tl = (s_mode & 8) ? len - 14 - hl : g16(h + 2) - hl;
        memcpy(h + 12, s_last_tcp + 30, 4);
        memcpy(h + 16, s_last_tcp + 26, 4);
        p16(t, g16(ts + 2)); p16(t + 2, g16(ts));
        if (14 + hl + 20 > len || tl < 20 || 14 + hl + tl > len) return;
        if (t[13] & 0x10) {                              /* acknowledging what we sent */
            u32 sl = (u32)(g16(s_last_tcp + 16) - 20 - (s_last_tcp[46] >> 4) * 4);
            p32(t + 8, g32(ts + 4) + sl + ((ts[13] & 3) ? 1 : 0));
        }
        p16(t + 16, 0);
        p16(t + 16, tsum(h, t, tl));
    }
    if (h[9] == 1 && g16(h + 2) >= hl + 4 && 14 + g16(h + 2) <= len) {   /* a ping's own sum */
        p16(h + hl + 2, 0);
        p16(h + hl + 2, isum(h + hl, g16(h + 2) - hl));
    }
    p16(h + 10, 0);                                      /* and the header's */
    p16(h + 10, isum(h, hl));
}

static int c_recv(u8 *f, int max)
{
    u32 n;
    tcp_rules();
    if (s_arp_len) { memcpy(f, s_arp, 42); s_arp_len = 0; return 42; }
    if (s_at + 2 > s_len || s_credit <= 0) return 0;
    s_credit--;
    n = (u32)g16(s_in + s_at); s_at += 2;
    if (n > s_len - s_at) n = s_len - s_at;
    if (n > (u32)max) n = (u32)max;
    memcpy(f, s_in + s_at, n);
    s_at += n;
    if (s_mode & 4) helpful(f, (int)n);
    if (s_mode & 32) s_now += 120000;
    return (int)n;
}

const Nic g_nic_el3 = { "none", c_none, c_stop, c_mac, c_send, c_recv, c_why, c_link };
const Nic g_nic_ne2000 = { "fuzz", c_start, c_stop, c_mac, c_send, c_recv, c_why, c_link };

int ip_apply_saved(void);
u32 ip_dns(void);
int ip_how(void);
u32 ip_ntp_server(void);
int ip_parse(const char *s, u32 *out);
int ip_udp_ask(u32 to, u16 port, const u8 *q, int qlen, u8 *ans, int max, int wait_ms);
u32 ip_lookup(const char *name);
void ip_set_dns(u32 addr, u16 port);
int  ip_dhcp(void);
int  ip_ping(u32 to, int seq, int wait_ms);

void fuzz_one(const u8 *data, u32 len, u32 mode)
{
    static u8 buf[70000];
    int i;
    s_in = data; s_len = len; s_at = 0; s_mode = mode; s_arp_len = 0; s_credit = 0;
    s_last_udp_len = s_last_tcp_len = 0;
    net_up();
    {                                                   /* an address typed by hand: the input as text */
        char t[24]; u32 a;
        for (i = 0; i < 23 && (u32)i < len; i++) t[i] = (char)data[i];
        t[i] = 0;
        (void)ip_parse(t, &a);
        (void)ip_parse("10.0.2.15", &a);
    }
    ip_configure(0, 0, 0);
    if (ip_apply_saved()) (void)(ip_gateway() + ip_netmask() + ip_dns() + (u32)ip_how() + ip_ntp_server());
    if ((mode & 3) != 0) {
        ip_configure(0x0A00020Fu, 0xFFFFFF00u, 0x0A000202u);
        ip_set_dns(0x0A000203u, 53);
    }
    switch (mode & 3) {
    case 0: ip_dhcp(); ip_lookup("www.fuzz.test"); break;
    case 1: ip_lookup("www.fuzz.test"); ip_lookup("WWW.Fuzz.test");   /* the seeds' question */ break;
    case 2:
        if (tcp_connect(0x5DB8D822u, 443, 3000)) {
            if (mode & 256) {                            /* hanging up first: theirs comes while ours is out */
                tcp_close();
                break;
            }
            tcp_write((const u8 *)"GET / HTTP/1.0\r\n\r\n", 18);
            for (i = 0; i < 64; i++) if (tcp_read(buf, (int)sizeof buf, 300) < 0) break;
            if (mode & 512) {                            /* more than they take, then a long wait */
                tcp_write(buf, 20000);
                tcp_read(buf, 100, 120000);
            }
        }
        (void)tcp_done(); (void)tcp_why();
        tcp_close();
        break;
    case 3: {
        u8 q[48] = { 0x23 }, a[1500];
        ip_ping(0x0A000202u, 1, 500);
        ip_udp_ask(0x0A000203u, 123, q, 48, a, (int)sizeof a, 500);
        break;
    }
    }
    s_credit = 1 << 30;
    while (s_at + 2 <= s_len) ip_poll();                /* whatever is left, read anyway */
    tcp_rules();
}
