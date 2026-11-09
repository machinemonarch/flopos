/* ===========================================================================
 * ip.c -- addresses, and finding who is behind one.
 *
 * Three things live here because they are one thing in practice.
 *
 * ARP is how a machine finds out which card to hand a frame to.  Shouting
 * "who has 10.0.2.2" at everybody and waiting for one of them to admit it
 * is, remarkably, still exactly how this works.
 *
 * IP is a twenty-byte header saying who it is from, who it is for, what is
 * inside, and how many more machines may pass it along before giving up.
 *
 * ICMP is the part of IP that talks about IP, and the only bit of it we
 * want yet is echo -- ping.  Ping is the right first thing to aim at
 * because it is the smallest round trip there is: if an echo comes back,
 * then the card works, the cable works, ARP works, the checksum is right
 * and somebody out there agrees we exist.
 *
 * Everything here is big-endian on the wire and little-endian in the
 * machine, which is a permanent small tax and the reason for all the
 * shifting.  Nothing is assumed about alignment; the bytes are placed one
 * at a time on purpose, because a frame is a stream of bytes and not a
 * struct, whatever the diagrams suggest.
 * ===========================================================================
 */
#include "flopos.h"
#include "io.h"

#define ET_ARP  0x0806
#define ET_IPV4 0x0800

#define ARP_REQUEST 1
#define ARP_REPLY   2

#define PROTO_ICMP  1
#define PROTO_UDP   17

#define ICMP_ECHO_REPLY   0
#define ICMP_ECHO_REQUEST 8

/* Where we think we are.  QEMU hands out 10.0.2.15 with the gateway at
 * 10.0.2.2, so those are the defaults -- not because they are right in
 * general but because they make the thing testable without asking. */
static u32 s_ip   = 0x0A00020F;          /* 10.0.2.15                      */
static u32 s_mask = 0xFFFFFF00;
static u32 s_gw   = 0x0A000202;          /* 10.0.2.2                       */

typedef struct {
    u32 ip;
    u8  mac[6];
    u32 at;                              /* when we learned it             */
} ArpEntry;

#define ARP_N 8
static ArpEntry s_arp[ARP_N];
static int s_arp_next;

/* ---- putting numbers on the wire -------------------------------------- */
static void put16(u8 *p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
static void put32(u8 *p, u32 v)
{
    p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >> 8);  p[3] = (u8)v;
}
static u16 get16(const u8 *p) { return (u16)((p[0] << 8) | p[1]); }
static u32 get32(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

/* The one's complement sum that every one of these headers carries.  Add
 * the whole header as sixteen-bit words, fold the carries back in, invert.
 * It catches almost nothing by modern standards and costs almost nothing,
 * which is why it has outlasted everything that was going to replace it. */
static u16 checksum(const u8 *p, int len)
{
    u32 sum = 0;
    int i;

    for (i = 0; i + 1 < len; i += 2) sum += (u32)((p[i] << 8) | p[i + 1]);
    if (i < len) sum += (u32)(p[i] << 8);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (u16)(~sum & 0xFFFF);
}

u32  ip_addr(void)    { return s_ip; }
u32  ip_gateway(void) { return s_gw; }
u32  ip_netmask(void) { return s_mask; }

void ip_configure(u32 addr, u32 mask, u32 gw)
{
    s_ip = addr; s_mask = mask; s_gw = gw;
    s_arp_next = 0;
    memset8((u8 *)s_arp, 0, sizeof s_arp);
}

/* ---- who is behind an address ----------------------------------------- */
static const u8 *arp_lookup(u32 ip)
{
    int i;
    for (i = 0; i < ARP_N; i++)
        if (s_arp[i].ip == ip) return s_arp[i].mac;
    return 0;
}

static void arp_learn(u32 ip, const u8 *mac)
{
    int i;

    for (i = 0; i < ARP_N; i++)
        if (s_arp[i].ip == ip) break;
    if (i == ARP_N) { i = s_arp_next; s_arp_next = (s_arp_next + 1) % ARP_N; }

    s_arp[i].ip = ip;
    for (int j = 0; j < 6; j++) s_arp[i].mac[j] = mac[j];
    s_arp[i].at = ms_now();
}

static void arp_ask(u32 ip)
{
    u8 f[42];
    int n, i;

    n = net_frame_begin(f, g_mac_broadcast, ET_ARP);
    put16(f + n + 0, 1);                 /* over ethernet                  */
    put16(f + n + 2, ET_IPV4);           /* looking for an IP address      */
    f[n + 4] = 6;
    f[n + 5] = 4;
    put16(f + n + 6, ARP_REQUEST);
    for (i = 0; i < 6; i++) f[n + 8 + i] = net_mac()[i];
    put32(f + n + 14, s_ip);
    for (i = 0; i < 6; i++) f[n + 18 + i] = 0;
    put32(f + n + 24, ip);

    net_send(f, n + 28);
}

static void arp_answer(const u8 *in)
{
    u8 f[42];
    int n, i;
    const u8 *a = in + 14;

    n = net_frame_begin(f, a + 8, ET_ARP);   /* straight back to the asker */
    put16(f + n + 0, 1);
    put16(f + n + 2, ET_IPV4);
    f[n + 4] = 6;
    f[n + 5] = 4;
    put16(f + n + 6, ARP_REPLY);
    for (i = 0; i < 6; i++) f[n + 8 + i] = net_mac()[i];
    put32(f + n + 14, s_ip);
    for (i = 0; i < 6; i++) f[n + 18 + i] = a[8 + i];
    put32(f + n + 24, get32(a + 14));

    net_send(f, n + 28);
}

/* ---- the one echo we are waiting for ---------------------------------- */
static u16 s_ping_id, s_ping_seq;
static int s_ping_got;
static u32 s_ping_from;

static void take_icmp(const u8 *ip_hdr, const u8 *p, int len)
{
    if (len < 8) return;

    if (p[0] == ICMP_ECHO_REPLY &&
        get16(p + 4) == s_ping_id && get16(p + 6) == s_ping_seq) {
        s_ping_got = 1;
        s_ping_from = get32(ip_hdr + 12);
        return;
    }

    /* Somebody is pinging us.  Answering is politeness and it is also the
     * only way anything out there can tell that we are here. */
    if (p[0] == ICMP_ECHO_REQUEST) {
        u8 f[NET_MTU];
        int n, hl = (ip_hdr[0] & 0x0F) * 4;
        const u8 *mac = arp_lookup(get32(ip_hdr + 12));
        int i;

        if (!mac || len > NET_MTU - 34) return;

        n = net_frame_begin(f, mac, ET_IPV4);
        f[n + 0] = 0x45;
        f[n + 1] = 0;
        put16(f + n + 2, (u16)(20 + len));
        put16(f + n + 4, 0);
        put16(f + n + 6, 0);
        f[n + 8] = 64;
        f[n + 9] = PROTO_ICMP;
        put16(f + n + 10, 0);
        put32(f + n + 12, s_ip);
        put32(f + n + 16, get32(ip_hdr + 12));
        put16(f + n + 10, checksum(f + n, 20));

        for (i = 0; i < len; i++) f[n + 20 + i] = p[i];
        f[n + 20] = ICMP_ECHO_REPLY;
        put16(f + n + 20 + 2, 0);
        put16(f + n + 20 + 2, checksum(f + n + 20, len));

        (void)hl;
        net_send(f, n + 20 + len);
    }
}

/* ---- everything that arrives ------------------------------------------
 * Called often and from one place.  Nothing here blocks: a frame is
 * either there or it is not, and if it is not we go and draw another
 * frame of the screen instead.
 */
void ip_poll(void)
{
    u8 f[NET_MTU];
    int len, guard;

    for (guard = 0; guard < 8; guard++) {
        len = net_recv(f, (int)sizeof f);
        if (len < 14) return;

        switch (get16(f + 12)) {
        case ET_ARP: {
            const u8 *a = f + 14;
            if (len < 42) break;
            if (get16(a + 2) != ET_IPV4) break;
            arp_learn(get32(a + 14), a + 8);
            if (get16(a + 6) == ARP_REQUEST && get32(a + 24) == s_ip)
                arp_answer(f);
            break;
        }
        case ET_IPV4: {
            const u8 *h = f + 14;
            int hl;
            if (len < 34) break;
            hl = (h[0] & 0x0F) * 4;
            if (hl < 20 || 14 + hl > len) break;
            if (get32(h + 16) != s_ip) break;        /* not for us         */
            arp_learn(get32(h + 12), f + 6);
            if (h[9] == PROTO_ICMP)
                take_icmp(h, h + hl, get16(h + 2) - hl);
            break;
        }
        default:
            break;
        }
    }
}

/* Find the card behind an address, asking if we have to.  Anything off
 * our own stretch of wire goes to the gateway instead, which is the whole
 * of routing as far as a machine with one card is concerned. */
const u8 *ip_route(u32 to, int wait_ms)
{
    u32 target = ((to ^ s_ip) & s_mask) ? s_gw : to;
    const u8 *mac;
    u32 until;
    int tries;

    mac = arp_lookup(target);
    if (mac) return mac;

    for (tries = 0; tries < 3; tries++) {
        arp_ask(target);
        until = ms_now() + (u32)(wait_ms / 3);
        while (ms_now() < until) {
            ip_poll();
            mac = arp_lookup(target);
            if (mac) return mac;
        }
    }
    return 0;
}

/* ---- ping -------------------------------------------------------------
 * Returns how long it took in milliseconds, or -1.
 */
int ip_ping(u32 to, int seq, int wait_ms)
{
    u8 f[64];
    const u8 *mac;
    u32 start, until;
    int n, i;

    if (!net_ready()) return -1;

    mac = ip_route(to, wait_ms);
    if (!mac) return -2;                 /* nobody admitted to that address */

    s_ping_id  = 0x464C;                 /* "FL"                           */
    s_ping_seq = (u16)seq;
    s_ping_got = 0;

    n = net_frame_begin(f, mac, ET_IPV4);
    f[n + 0] = 0x45;                     /* version four, five words long  */
    f[n + 1] = 0;
    put16(f + n + 2, 20 + 16);
    put16(f + n + 4, (u16)seq);
    put16(f + n + 6, 0);
    f[n + 8] = 64;                       /* sixty-four machines, at most   */
    f[n + 9] = PROTO_ICMP;
    put16(f + n + 10, 0);
    put32(f + n + 12, s_ip);
    put32(f + n + 16, to);
    put16(f + n + 10, checksum(f + n, 20));

    f[n + 20] = ICMP_ECHO_REQUEST;
    f[n + 21] = 0;
    put16(f + n + 22, 0);
    put16(f + n + 24, s_ping_id);
    put16(f + n + 26, s_ping_seq);
    for (i = 0; i < 8; i++) f[n + 28 + i] = (u8)('a' + i);
    put16(f + n + 22, checksum(f + n + 20, 16));

    start = ms_now();
    if (!net_send(f, n + 36)) return -3;

    until = start + (u32)wait_ms;
    while (ms_now() < until) {
        ip_poll();
        if (s_ping_got) return (int)(ms_now() - start);
    }
    return -1;
}

/* ---- reading and writing an address as text ---------------------------- */
int ip_parse(const char *s, u32 *out)
{
    u32 v = 0;
    int part = 0, digits = 0, n = 0;

    for (;;) {
        if (*s >= '0' && *s <= '9') {
            n = n * 10 + (*s - '0');
            if (n > 255) return 0;
            digits++;
        } else if (*s == '.' || *s == 0) {
            if (!digits) return 0;
            v = (v << 8) | (u32)n;
            part++;
            n = 0; digits = 0;
            if (*s == 0) break;
            if (part > 3) return 0;
        } else {
            return 0;
        }
        s++;
    }
    if (part != 4) return 0;
    *out = v;
    return 1;
}
