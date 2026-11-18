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
#define PROTO_TCP   6

#define ICMP_ECHO_REPLY   0
#define ICMP_ECHO_REQUEST 8

/* Where we are.  Nothing until the network says otherwise: an address
 * guessed is an address that is wrong everywhere but one place.  DHCP
 * fills these in; `ip` can set them by hand. */
static u32 s_ip, s_mask, s_gw, s_dns;
static u16 s_dns_srv_port = 53;          /* settable, for testing          */
static int s_how;                        /* 0 none, 1 by hand, 2 by DHCP   */

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

u32  ip_dns(void)     { return s_dns; }

/* Settings made by hand in the network menu are kept on the disk; this
 * puts them back when the network is first wanted after a boot. */
int ip_apply_saved(void)
{
    u32 how, a, m, g, d;
    if (!store_net_get(&how, &a, &m, &g, &d) || how != 1 || !a) return 0;
    ip_configure(a, m, g);
    ip_set_dns(d, 53);
    clock_from_network();
    return 1;
}

/* Who to ask about names, when DHCP named nobody or named somebody wrong. */
void ip_set_dns(u32 addr, u16 port)
{
    s_dns = addr;
    s_dns_srv_port = port ? port : 53;
}
int  ip_how(void)     { return s_how; }

void ip_configure(u32 addr, u32 mask, u32 gw)
{
    s_ip = addr; s_mask = mask; s_gw = gw;
    s_how = addr ? 1 : 0;
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

/* What is known is never replaced: once the router (or anyone) has a
 * card to its name, a later claim to be it is ignored.  The table starts
 * again when the address is set.  A newcomer takes the oldest slot, but
 * never the router's. */
static u32 s_ask_ip, s_ask_until;        /* the question we are waiting on  */

static void arp_learn(u32 ip, const u8 *mac)
{
    int i;

    if (!ip) return;
    for (i = 0; i < ARP_N; i++)
        if (s_arp[i].ip == ip) return;
    i = s_arp_next;
    if (s_gw && s_arp[i].ip == s_gw) i = (i + 1) % ARP_N;
    s_arp_next = (i + 1) % ARP_N;

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

    s_ask_ip = ip;
    s_ask_until = ms_now() + 1000;
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
        u32 src = get32(ip_hdr + 12);
        const u8 *mac = arp_lookup(((src ^ s_ip) & s_mask) ? s_gw : src);
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

const u8 *ip_route(u32 to, int wait_ms);

/* ---- UDP ---------------------------------------------------------------
 * Eight bytes of header -- two ports, a length, and a checksum that IPv4
 * lets you leave as zero, which every DHCP and DNS server on earth
 * accepts.  Everything above it here is a question and an answer, so
 * there is no more to it than that.
 */
static u16 s_ip_id = 1;

static int ip_header(u8 *f, int n, u8 proto, u32 to, int payload)
{
    f[n + 0] = 0x45;                     /* version four, five words long  */
    f[n + 1] = 0;
    put16(f + n + 2, (u16)(20 + payload));
    put16(f + n + 4, s_ip_id++);
    put16(f + n + 6, 0);
    f[n + 8] = 64;                       /* sixty-four machines, at most   */
    f[n + 9] = proto;
    put16(f + n + 10, 0);
    put32(f + n + 12, s_ip);
    put32(f + n + 16, to);
    put16(f + n + 10, checksum(f + n, 20));
    return n + 20;
}

/* Any protocol's payload, wrapped and sent: what TCP uses.  The payload
 * is copied in after the headers; the caller's checksum, if its protocol
 * has one, has already been worked out. */
int ip_send(u8 proto, u32 to, const u8 *payload, int len)
{
    u8 f[NET_MTU];
    const u8 *mac;
    int n, i;

    if (!s_ip || len < 0 || len > NET_MTU - 14 - 20) return 0;
    if (!(mac = ip_route(to, 1500))) return 0;
    n = net_frame_begin(f, mac, ET_IPV4);
    n = ip_header(f, n, proto, to, len);
    for (i = 0; i < len; i++) f[n + i] = payload[i];
    return net_send(f, n + len);
}

static int udp_send(u32 to, u16 sport, u16 dport, const u8 *data, int len)
{
    u8 f[NET_MTU];
    const u8 *mac;
    int n, i;

    if (len < 0 || len > NET_MTU - 14 - 20 - 8) return 0;
    if (to == 0xFFFFFFFF) mac = g_mac_broadcast;
    else if (!(mac = ip_route(to, 1500))) return 0;

    n = net_frame_begin(f, mac, ET_IPV4);
    n = ip_header(f, n, PROTO_UDP, to, 8 + len);
    put16(f + n + 0, sport);
    put16(f + n + 2, dport);
    put16(f + n + 4, (u16)(8 + len));
    put16(f + n + 6, 0);
    for (i = 0; i < len; i++) f[n + 8 + i] = data[i];
    return net_send(f, n + 8 + len);
}

/* ---- DHCP --------------------------------------------------------------
 * Four messages.  Anybody give me an address?  (DISCOVER.)  Here is one.
 * (OFFER.)  I will have that one, please.  (REQUEST.)  It is yours.  (ACK.)
 * Everything about where this machine is -- its address, how big its bit
 * of network is, the way out, who to ask about names -- arrives in the
 * last of them, which is why a laptop plugged into anything just works.
 *
 * Asked for by broadcast, from address zero, with the flag that asks the
 * server to answer by broadcast too: until it does, we have nowhere for
 * it to send anything.
 */
#define DHCP_DISCOVER 1
#define DHCP_OFFER    2
#define DHCP_REQUEST  3
#define DHCP_ACK      5
#define DHCP_NAK      6

static u32 s_xid;
static int s_dhcp;                       /* 1 waiting offer, 2 ack, 3 done */
static u32 s_off_ip, s_off_server, s_off_mask, s_off_gw, s_off_dns;

static int dhcp_send(int type)
{
    u8 b[320];
    const u8 *mac = net_mac();
    int n, i;

    for (i = 0; i < (int)sizeof b; i++) b[i] = 0;
    b[0] = 1;                            /* a request                      */
    b[1] = 1;                            /* over ethernet                  */
    b[2] = 6;
    put32(b + 4, s_xid);
    put16(b + 10, 0x8000);               /* answer by broadcast, please    */
    for (i = 0; i < 6; i++) b[28 + i] = mac[i];
    b[236] = 99; b[237] = 130; b[238] = 83; b[239] = 99;   /* the cookie  */

    n = 240;
    b[n++] = 53; b[n++] = 1; b[n++] = (u8)type;
    b[n++] = 61; b[n++] = 7; b[n++] = 1;           /* who we are          */
    for (i = 0; i < 6; i++) b[n++] = mac[i];
    if (type == DHCP_REQUEST) {
        b[n++] = 50; b[n++] = 4; put32(b + n, s_off_ip);     n += 4;
        b[n++] = 54; b[n++] = 4; put32(b + n, s_off_server); n += 4;
    }
    b[n++] = 12; b[n++] = 6;                       /* what to call us     */
    b[n++] = 'f'; b[n++] = 'l'; b[n++] = 'o';
    b[n++] = 'p'; b[n++] = 'o'; b[n++] = 's';
    b[n++] = 55; b[n++] = 4; b[n++] = 1; b[n++] = 3; b[n++] = 6; b[n++] = 42;
    b[n++] = 255;

    /* BOOTP's own minimum, which some servers still insist on. */
    if (n < 300) n = 300;
    return udp_send(0xFFFFFFFF, 68, 67, b, n);
}

static u32 s_ntp_srv;                    /* the time server DHCP named, or 0 */
u32 ip_ntp_server(void) { return s_ntp_srv; }

static void take_dhcp(const u8 *p, int len)
{
    const u8 *mac = net_mac();
    int i, type = 0;
    u32 server = 0, mask = 0, gw = 0, dns = 0, ntp = 0;

    if (len < 240 || p[0] != 2 || get32(p + 4) != s_xid) return;
    for (i = 0; i < 6; i++) if (p[28 + i] != mac[i]) return;
    if (p[236] != 99 || p[237] != 130 || p[238] != 83 || p[239] != 99) return;

    for (i = 240; i + 1 < len && p[i] != 255; ) {
        u8 code = p[i], l;
        if (code == 0) { i++; continue; }
        l = p[i + 1];
        if (i + 2 + l > len) break;
        if (code == 53 && l >= 1) type = p[i + 2];
        if (code == 54 && l >= 4) server = get32(p + i + 2);
        if (code == 1  && l >= 4) mask = get32(p + i + 2);
        if (code == 3  && l >= 4) gw = get32(p + i + 2);
        if (code == 6  && l >= 4) dns = get32(p + i + 2);
        if (code == 42 && l >= 4) ntp = get32(p + i + 2);   /* a time server (nts.c) */
        i += 2 + l;
    }

    if (type == DHCP_OFFER && s_dhcp == 1) {
        s_off_ip = get32(p + 16);
        s_off_server = server;
        s_off_mask = mask ? mask : 0xFFFFFF00;
        s_off_gw = gw;
        s_off_dns = dns;
        s_dhcp = 2;
    } else if (type == DHCP_ACK && s_dhcp == 2) {
        s_ip = get32(p + 16);
        s_mask = mask ? mask : s_off_mask;
        s_gw = gw ? gw : s_off_gw;
        s_dns = dns ? dns : s_off_dns;
        s_ntp_srv = ntp;
        s_how = 2;
        s_dhcp = 3;
    } else if (type == DHCP_NAK && s_dhcp == 2) {
        s_dhcp = 0;
    }
}

/* Three tries, each waiting a little longer: a second, then two, then
 * three.  A router answers in a few milliseconds; a network with no DHCP
 * server at all costs six seconds to find out, once. */
int ip_dhcp(void)
{
    int attempt;
    const u8 *mac = net_mac();

    if (!net_ready()) return 0;

    for (attempt = 0; attempt < 3; attempt++) {
        u32 until;

        s_xid = (g_cpu_mhz ? tsc32() : g_ticks * 2654435761u) ^
                ((u32)mac[2] << 24 | (u32)mac[3] << 16 | (u32)mac[4] << 8 | mac[5]);
        s_dhcp = 1;
        if (!dhcp_send(DHCP_DISCOVER)) return 0;
        until = ms_now() + 1000u * (u32)(attempt + 1);
        while (ms_now() < until && s_dhcp == 1) ip_poll();
        if (s_dhcp != 2) continue;

        if (!dhcp_send(DHCP_REQUEST)) return 0;
        until = ms_now() + 2000;
        while (ms_now() < until && s_dhcp == 2) ip_poll();
        if (s_dhcp == 3) {
            s_arp_next = 0;
            memset8((u8 *)s_arp, 0, sizeof s_arp);
            clock_from_network();
            return 1;
        }
    }
    s_dhcp = 0;
    return 0;
}

/* ---- DNS ---------------------------------------------------------------
 * A name, asked of the server DHCP named, as one UDP packet: a header,
 * the name spelled as length-prefixed labels, and "an address, please".
 * The answer repeats the question and then lists records; the first one
 * that is an address is the one we want.  Names in answers can be
 * pointers back to earlier names -- the format's one bit of compression
 * -- which only matters here for knowing how far to skip.
 */
static u16 s_dns_id, s_dns_port;
static u32 s_dns_got;
static int s_dns_done;

static int skip_name(const u8 *p, int i, int len)
{
    while (i < len) {
        u8 l = p[i];
        if (l == 0) return i + 1;
        if ((l & 0xC0) == 0xC0) return i + 2;
        i += 1 + l;
    }
    return len;
}

static void take_dns(const u8 *p, int len)
{
    int i, q, a;

    if (len < 12 || get16(p) != s_dns_id || !(p[2] & 0x80)) return;
    s_dns_done = 1;
    if ((p[3] & 0x0F) != 0) return;      /* the server said no              */

    q = get16(p + 4);
    a = get16(p + 6);
    i = 12;
    while (q-- > 0 && i < len) i = skip_name(p, i, len) + 4;
    while (a-- > 0 && i < len) {
        u16 type, rdlen;
        i = skip_name(p, i, len);
        if (i + 10 > len) return;
        type = get16(p + i);
        rdlen = get16(p + i + 8);
        i += 10;
        if (i + rdlen > len) return;
        if (type == 1 && rdlen == 4) { s_dns_got = get32(p + i); return; }
        i += rdlen;
    }
}

/* Returns the address, or 0. */
u32 ip_lookup(const char *name)
{
    u8 q[300];
    int n = 12, i, start;
    u32 until;

    if (!net_ready() || !s_dns || !name[0]) return 0;

    /* The question's number and the port it is asked from are two
     * separate random numbers, so a faked answer has to guess both. */
    {
        u8 b[4];
        rand_bytes(b, 4);
        s_dns_id = (u16)(b[0] | b[1] << 8);
        s_dns_port = (u16)(49152 + ((b[2] | b[3] << 8) & 0x3FFF));
    }
    for (i = 0; i < 12; i++) q[i] = 0;
    put16(q, s_dns_id);
    q[2] = 0x01;                         /* recursion, please              */
    put16(q + 4, 1);

    /* "www.example.com" -> 3 www 7 example 3 com 0 */
    while (*name && n < (int)sizeof q - 6) {
        start = n++;
        while (*name && *name != '.' && n < (int)sizeof q - 6) q[n++] = (u8)*name++;
        if (n - start - 1 == 0 || n - start - 1 > 63) return 0;
        q[start] = (u8)(n - start - 1);
        if (*name == '.') name++;
    }
    q[n++] = 0;
    put16(q + n, 1); n += 2;             /* an address                     */
    put16(q + n, 1); n += 2;             /* on the internet                */

    for (i = 0; i < 2; i++) {
        s_dns_got = 0;
        s_dns_done = 0;
        if (!udp_send(s_dns, s_dns_port, s_dns_srv_port, q, n)) return 0;
        until = ms_now() + 2000;
        while (ms_now() < until && !s_dns_done) ip_poll();
        if (s_dns_done) { s_dns_port = 0; return s_dns_got; }
    }
    s_dns_port = 0;                      /* nobody is waiting any more     */
    return 0;
}

/* ---- a question over UDP ----------------------------------------------
 * One datagram out to a server, and its answer back (nts.c asks the time
 * this way).  The answer is believed only if it comes from the address
 * asked, to the random port it was asked from, while the question is open. */
static u16 s_ask_port;
static u32 s_ask_to;
static u8 *s_ask_buf;
static int s_ask_max, s_ask_len;

static void take_answer(const u8 *ip_hdr, const u8 *p, int len)
{
    if (get32(ip_hdr + 12) != s_ask_to || s_ask_len) return;
    if (len > s_ask_max) len = s_ask_max;
    { int i; for (i = 0; i < len; i++) s_ask_buf[i] = p[i]; }
    s_ask_len = len ? len : -1;
}

int ip_udp_ask(u32 to, u16 port, const u8 *q, int qlen, u8 *ans, int max, int wait_ms)
{
    u16 r;
    u32 until;
    if (!net_ready() || !s_ip || !to) return 0;
    rand_bytes((u8 *)&r, 2);
    s_ask_to = to; s_ask_buf = ans; s_ask_max = max; s_ask_len = 0;
    s_ask_port = (u16)(49152 + (r & 0x3FFF));
    if (!udp_send(to, s_ask_port, port, q, qlen)) { s_ask_port = 0; return 0; }
    until = ms_now() + (u32)wait_ms;
    while (ms_now() < until && !s_ask_len) ip_poll();
    s_ask_port = 0;
    return s_ask_len > 0 ? s_ask_len : 0;
}

static void take_udp(const u8 *ip_hdr, const u8 *u, int len)
{
    u16 dport;
    int ulen;

    if (len < 8) return;
    dport = get16(u + 2);
    ulen = get16(u + 4);
    if (ulen < 8 || ulen > len) return;

    if (dport == 68 && s_dhcp && s_dhcp < 3) take_dhcp(u + 8, ulen - 8);
    else if (dport == s_dns_port && s_dns_port) {
        /* Only the name server we asked may answer, from the port we asked. */
        if (get32(ip_hdr + 12) == s_dns && get16(u) == s_dns_srv_port)
            take_dns(u + 8, ulen - 8);
    }
    else if (dport == s_ask_port && s_ask_port) take_answer(ip_hdr, u + 8, ulen - 8);
}

/* ---- everything that arrives ------------------------------------------
 * Called often and from one place.  Nothing here blocks: a frame is
 * either there or it is not, and if it is not we go and draw another
 * frame of the screen instead.
 */
/* For testing on the emulator, whose card never loses anything: throw
 * away every Nth frame that arrives, the way a full 3C589 would (`net
 * drop N`; 0, the default, keeps them all). */
u32 g_net_drop;
static int s_taking;                     /* inside ip_poll: sends must not wait */

static void poll_frames(void);
void ip_poll(void)
{
    s_taking++;
    poll_frames();
    s_taking--;
}

static void poll_frames(void)
{
    static u32 seen;
    u8 f[NET_MTU];
    int len, guard;

    for (guard = 0; guard < 48; guard++) {
        len = net_recv(f, (int)sizeof f);
        if (len < 14) return;
        if (g_net_drop && ++seen % g_net_drop == 0) continue;
        rand_add((u32)len ^ ((u32)f[len - 1] << 16));

        switch (get16(f + 12)) {
        case ET_ARP: {
            const u8 *a = f + 14;
            if (len < 42) break;
            if (get16(a + 2) != ET_IPV4) break;
            /* Believed only as the answer to a question just asked. */
            if (get16(a + 6) == ARP_REPLY && s_ask_ip &&
                get32(a + 14) == s_ask_ip && ms_now() < s_ask_until) {
                arp_learn(s_ask_ip, a + 8);
                s_ask_ip = 0;
            }
            if (s_ip && get16(a + 6) == ARP_REQUEST && get32(a + 24) == s_ip)
                arp_answer(f);
            break;
        }
        case ET_IPV4: {
            const u8 *h = f + 14;
            int hl;
            if (len < 34) break;
            hl = (h[0] & 0x0F) * 4;
            if (hl < 20 || 14 + hl > len) break;
            {
                /* Ours, or everybody's.  Before DHCP has finished we have
                 * no address at all, and the reply that gives us one is
                 * addressed to the address it is giving -- so UDP to the
                 * DHCP client port is let through whatever it says. */
                u32 dst = get32(h + 16);
                int mine = (s_ip && dst == s_ip) || dst == 0xFFFFFFFF ||
                           (s_ip && dst == (s_ip | ~s_mask));
                int dhcp = h[9] == PROTO_UDP && 14 + hl + 4 <= len &&
                           get16(h + hl + 2) == 68;
                int tot = get16(h + 2);
                if (!mine && !dhcp) break;
                if (tot < hl || 14 + tot > len) break;
                /* A neighbour who writes to us is remembered, so a ping
                 * can be answered; the router and strangers beyond it
                 * are not learned this way. */
                if (s_ip && dst == s_ip && get32(h + 12) != s_gw &&
                    !((get32(h + 12) ^ s_ip) & s_mask))
                    arp_learn(get32(h + 12), f + 6);
                if (h[9] == PROTO_ICMP && mine)
                    take_icmp(h, h + hl, tot - hl);
                else if (h[9] == PROTO_UDP)
                    take_udp(h, h + hl, tot - hl);
                else if (h[9] == PROTO_TCP && s_ip && dst == s_ip)
                    tcp_input(get32(h + 12), h + hl, tot - hl);
            }
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

    /* A send set off by something arriving never waits here: waiting takes
     * in more arrivals, which send, which wait, each a level deeper on the
     * stack.  Ask once and give up; TCP sends again later. */
    if (s_taking) {
        if (!s_ask_ip || ms_now() >= s_ask_until) arp_ask(target);
        return 0;
    }

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

    if (!net_ready() || !s_ip) return -1;

    mac = ip_route(to, wait_ms);
    if (!mac) return -2;                 /* nobody admitted to that address */

    s_ping_id  = 0x464C;                 /* "FL"                           */
    s_ping_seq = (u16)seq;
    s_ping_got = 0;

    n = net_frame_begin(f, mac, ET_IPV4);
    (void)ip_header(f, n, PROTO_ICMP, to, 16);

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
