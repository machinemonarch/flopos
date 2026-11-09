/* ===========================================================================
 * tcp.c -- a conversation that does not lose words.
 *
 * IP delivers packets when it feels like it, in whatever order, or not at
 * all.  TCP turns that into a stream: every byte numbered, every byte
 * acknowledged by the other end, anything unacknowledged sent again after
 * a while, anything arriving out of turn dropped and asked for again.  It
 * is the thing the web, and everything on top of the web, is built on.
 *
 * One conversation at a time, from this end only -- FLOPOS calls out; it
 * does not listen.  That is all a web page needs, and it keeps the whole
 * thing small enough to read in one go.
 *
 * Two numbers matter more than anything else here:
 *
 *   The window we offer is small, three full segments.  The 3C589 has a
 *   few kilobytes of receive FIFO, and drawing one frame of the screen
 *   takes the best part of 30ms on the laptop; a server allowed to send
 *   more than the FIFO holds while we are drawing would have the rest
 *   thrown away by the card, and then have to send it all again.
 *
 *   The retransmit timeout starts at a second and doubles, eight times,
 *   before the conversation is given up for dead.
 * ===========================================================================
 */
#include "flopos.h"
#include "io.h"

enum { CLOSED, SYN_SENT, ESTAB, FIN_WAIT1, FIN_WAIT2, CLOSE_WAIT, LAST_ACK };

#define F_FIN 0x01
#define F_SYN 0x02
#define F_RST 0x04
#define F_PSH 0x08
#define F_ACK 0x10

#define MSS      1460
#define RX_SIZE  16384                   /* a power of two                 */
#define RX_WIN   (3 * MSS)               /* what we let them have in flight */
#define TX_SIZE  8192

static int s_state;
static u32 s_peer;
static u16 s_lport, s_rport;
static u32 s_iss, s_snd_una, s_snd_nxt;  /* our numbers                    */
static u32 s_rcv_nxt;                    /* theirs                         */
static u32 s_peer_win;
static u32 s_peer_mss = 536;
static int s_peer_fin;
static const char *s_why = "";

static u8  s_rx[RX_SIZE];
static u32 s_rx_in, s_rx_out;            /* counters; the difference is held */

static u8  s_tx[TX_SIZE];                /* from s_snd_una onwards         */
static int s_tx_len;                     /* sent-unacked plus unsent       */

static u32 s_rtx_at, s_rto;
static int s_tries;

const char *tcp_why(void) { return s_why; }
int tcp_open(void) { return s_state == ESTAB || s_state == CLOSE_WAIT; }

/* Nothing more will ever arrive: they have finished and all of it has
 * been read, or the conversation is gone. */
int tcp_done(void)
{
    return s_state == CLOSED || (s_peer_fin && s_rx_in == s_rx_out);
}

static u16 get16(const u8 *p) { return (u16)((p[0] << 8) | p[1]); }
static u32 get32(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}
static void put16(u8 *p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
static void put32(u8 *p, u32 v)
{
    p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v;
}

/* Sequence numbers wrap, so "before" means "less, by a signed difference". */
static int before(u32 a, u32 b) { return (s32)(a - b) < 0; }

static u32 sum16(u32 sum, const u8 *p, int len)
{
    int i;
    for (i = 0; i + 1 < len; i += 2) sum += (u32)((p[i] << 8) | p[i + 1]);
    if (i < len) sum += (u32)(p[i] << 8);
    return sum;
}

/* TCP's checksum covers a pretend header with both addresses in it, so a
 * segment delivered to the wrong machine is caught as well as one that
 * was damaged on the way. */
static u16 tcp_sum(u32 src, u32 dst, const u8 *seg, int len)
{
    u32 sum = (src >> 16) + (src & 0xFFFF) + (dst >> 16) + (dst & 0xFFFF) +
              6 + (u32)len;
    sum = sum16(sum, seg, len);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (u16)(~sum & 0xFFFF);
}

static u32 rx_free(void) { return RX_SIZE - (s_rx_in - s_rx_out); }

static u16 window(void)
{
    u32 w = rx_free();
    return (u16)(w > RX_WIN ? RX_WIN : w);
}

static int segment(u8 flags, u32 seq, const u8 *data, int len)
{
    u8 seg[20 + 4 + MSS];
    int hl = (flags & F_SYN) ? 24 : 20, i;

    if (len > MSS) len = MSS;
    put16(seg + 0, s_lport);
    put16(seg + 2, s_rport);
    put32(seg + 4, seq);
    put32(seg + 8, (flags & F_ACK) ? s_rcv_nxt : 0);
    seg[12] = (u8)((hl / 4) << 4);
    seg[13] = flags;
    put16(seg + 14, window());
    put16(seg + 16, 0);
    put16(seg + 18, 0);
    if (flags & F_SYN) {                 /* the biggest segment we take    */
        seg[20] = 2; seg[21] = 4; put16(seg + 22, MSS);
    }
    for (i = 0; i < len; i++) seg[hl + i] = data[i];
    put16(seg + 16, tcp_sum(ip_addr(), s_peer, seg, hl + len));
    return ip_send(6, s_peer, seg, hl + len);
}

static void ack_now(void) { segment(F_ACK, s_snd_nxt, 0, 0); }

/* Send whatever is waiting and fits in what they will take. */
static void output(void)
{
    for (;;) {
        u32 flight = s_snd_nxt - s_snd_una;
        u32 room = s_peer_win > flight ? s_peer_win - flight : 0;
        int unsent = s_tx_len - (int)flight;
        int n = unsent;

        if (unsent <= 0 || room == 0) return;
        if (n > (int)s_peer_mss) n = (int)s_peer_mss;
        if (n > (int)room) n = (int)room;

        if (flight == 0) s_rtx_at = ms_now() + s_rto;
        segment(F_ACK | F_PSH, s_snd_nxt, s_tx + flight, n);
        s_snd_nxt += (u32)n;
    }
}

/* Anything sent and not acknowledged in time is sent again, from the
 * oldest byte -- everything after it too, which is wasteful and simple
 * and on a single cable almost never needed. */
static void tick(void)
{
    if (s_state == CLOSED || s_snd_nxt == s_snd_una) return;
    if (before(ms_now(), s_rtx_at)) return;

    if (++s_tries > 8) {
        s_state = CLOSED;
        s_why = "the other end stopped answering";
        return;
    }
    s_rto *= 2;
    if (s_rto > 8000) s_rto = 8000;
    s_rtx_at = ms_now() + s_rto;

    if (s_state == SYN_SENT) {
        segment(F_SYN, s_iss, 0, 0);
    } else if (s_state == FIN_WAIT1 || s_state == LAST_ACK) {
        if (s_tx_len) { s_snd_nxt = s_snd_una; output(); }
        else segment(F_FIN | F_ACK, s_snd_una, 0, 0);
    } else {
        s_snd_nxt = s_snd_una;
        output();
    }
}

void tcp_input(u32 from, const u8 *seg, int len)
{
    u32 seq, ack;
    u8 flags;
    int hl, dlen, ack_wanted = 0;
    const u8 *data;

    if (len < 20 || s_state == CLOSED) return;
    if (from != s_peer || get16(seg) != s_rport || get16(seg + 2) != s_lport)
        return;
    if (tcp_sum(from, ip_addr(), seg, len) != 0) return;   /* damaged      */

    seq = get32(seg + 4);
    ack = get32(seg + 8);
    hl = (seg[12] >> 4) * 4;
    flags = seg[13];
    if (hl < 20 || hl > len) return;
    data = seg + hl;
    dlen = len - hl;

    if (flags & F_RST) {
        if (s_state == SYN_SENT ? (flags & F_ACK) && ack == s_iss + 1
                                : !before(seq, s_rcv_nxt)) {
            s_state = CLOSED;
            s_why = "the other end hung up";
        }
        return;
    }

    if (s_state == SYN_SENT) {
        int i;
        if ((flags & (F_SYN | F_ACK)) != (F_SYN | F_ACK) || ack != s_iss + 1)
            return;
        /* The one option worth reading: how much they take at a time. */
        for (i = 20; i + 1 < hl; ) {
            u8 kind = seg[i];
            if (kind == 0) break;
            if (kind == 1) { i++; continue; }
            if (seg[i + 1] < 2) break;
            if (kind == 2 && seg[i + 1] == 4 && i + 3 < hl) {
                s_peer_mss = get16(seg + i + 2);
                if (s_peer_mss > MSS) s_peer_mss = MSS;
                if (s_peer_mss < 64) s_peer_mss = 64;
            }
            i += seg[i + 1];
        }
        s_rcv_nxt = seq + 1;
        s_snd_una = ack;
        s_peer_win = get16(seg + 14);
        s_state = ESTAB;
        s_tries = 0;
        s_rto = 1000;
        ack_now();
        return;
    }

    /* What they have received of ours. */
    if (flags & F_ACK) {
        if (before(s_snd_una, ack) && !before(s_snd_nxt, ack)) {
            int done = (int)(ack - s_snd_una);
            int data_done = done > s_tx_len ? s_tx_len : done;
            int i;
            for (i = 0; i + data_done < s_tx_len; i++) s_tx[i] = s_tx[i + data_done];
            s_tx_len -= data_done;
            s_snd_una = ack;
            s_tries = 0;
            s_rto = 1000;
            s_rtx_at = ms_now() + s_rto;
            if (s_snd_una == s_snd_nxt && s_tx_len == 0) {
                if (s_state == FIN_WAIT1) s_state = FIN_WAIT2;
                else if (s_state == LAST_ACK) s_state = CLOSED;
            }
        }
        s_peer_win = get16(seg + 14);
    }

    /* What they have sent us.  Only the next byte we were expecting is
     * any use; a later one means something in between went missing, and
     * acknowledging what we have makes them send it again. */
    if (dlen > 0) {
        if (seq == s_rcv_nxt || (before(seq, s_rcv_nxt) &&
                                 before(s_rcv_nxt, seq + (u32)dlen))) {
            int skip = (int)(s_rcv_nxt - seq), i;
            int take = dlen - skip;
            if (take > (int)rx_free()) take = (int)rx_free();
            for (i = 0; i < take; i++)
                s_rx[(s_rx_in + (u32)i) & (RX_SIZE - 1)] = data[skip + i];
            s_rx_in += (u32)take;
            s_rcv_nxt += (u32)take;
        }
        ack_wanted = 1;
    }

    if ((flags & F_FIN) && seq + (u32)dlen == s_rcv_nxt) {
        s_rcv_nxt++;
        s_peer_fin = 1;
        if (s_state == ESTAB) s_state = CLOSE_WAIT;
        else if (s_state == FIN_WAIT1 || s_state == FIN_WAIT2) s_state = CLOSED;
        ack_wanted = 1;
    }

    if (ack_wanted) ack_now();
    output();
}

/* Wait a little, keeping everything else alive: the network read, the
 * timers run, the screen drawn now and then. */
static void idle(void)
{
    ip_poll();
    tick();
    main_pump(0);
}

int tcp_connect(u32 to, u16 port, int wait_ms)
{
    u32 until, r;

    if (s_state != CLOSED) tcp_close();

    r = g_cpu_mhz ? tsc32() : g_ticks * 2654435761u;
    s_peer = to;
    s_rport = port;
    s_lport = (u16)(49152 + ((r >> 7) & 0x3FFF));
    s_iss = r * 69069u + 1;
    s_snd_una = s_iss;
    s_snd_nxt = s_iss + 1;
    s_rcv_nxt = 0;
    s_peer_win = MSS;
    s_peer_mss = 536;
    s_peer_fin = 0;
    s_rx_in = s_rx_out = 0;
    s_tx_len = 0;
    s_tries = 0;
    s_rto = 1000;
    s_why = "";
    s_state = SYN_SENT;

    if (!ip_route(to, 2000)) {
        s_state = CLOSED;
        s_why = "nobody on the way there answered";
        return 0;
    }
    s_rtx_at = ms_now() + s_rto;
    segment(F_SYN, s_iss, 0, 0);

    until = ms_now() + (u32)wait_ms;
    while (s_state == SYN_SENT && before(ms_now(), until)) idle();

    if (s_state == ESTAB) return 1;
    if (s_state == SYN_SENT) { s_why = "nobody answered"; s_state = CLOSED; }
    return 0;
}

/* Queue it all, sending as the window allows; returns how much went. */
int tcp_write(const u8 *data, int len)
{
    int done = 0;
    u32 until = ms_now() + 20000;

    while (done < len && tcp_open()) {
        int room = TX_SIZE - s_tx_len, n = len - done, i;
        if (n > room) n = room;
        for (i = 0; i < n; i++) s_tx[s_tx_len + i] = data[done + i];
        s_tx_len += n;
        done += n;
        output();
        if (done < len) {
            if (!before(ms_now(), until)) break;
            idle();
        }
    }
    return done;
}

/* Up to max bytes.  0 when they have finished and everything is read;
 * -1 when nothing came in time; -2 when the conversation is gone. */
int tcp_read(u8 *buf, int max, int wait_ms)
{
    u32 until = ms_now() + (u32)wait_ms;

    for (;;) {
        u32 held = s_rx_in - s_rx_out;
        if (held) {
            u32 before_free = rx_free(), i;
            u32 n = held < (u32)max ? held : (u32)max;
            for (i = 0; i < n; i++) buf[i] = s_rx[(s_rx_out + i) & (RX_SIZE - 1)];
            s_rx_out += n;
            /* If our window had shut, tell them it is open again, or they
             * will sit there waiting to be told. */
            if (before_free < MSS && rx_free() >= MSS) ack_now();
            return (int)n;
        }
        if (s_peer_fin) return 0;
        if (s_state == CLOSED) return -2;
        if (!before(ms_now(), until)) return -1;
        idle();
    }
}

void tcp_close(void)
{
    u32 until = ms_now() + 3000;

    if (s_state == CLOSED || s_state == SYN_SENT) { s_state = CLOSED; return; }

    /* Whatever is still waiting to go, then our end of the goodbye. */
    while (s_tx_len && tcp_open() && before(ms_now(), until)) idle();
    if (s_state == CLOSED) return;

    segment(F_FIN | F_ACK, s_snd_nxt, 0, 0);
    s_snd_nxt++;
    s_state = (s_state == CLOSE_WAIT) ? LAST_ACK : FIN_WAIT1;
    s_rtx_at = ms_now() + s_rto;

    while (s_state != CLOSED && s_state != FIN_WAIT2 && before(ms_now(), until))
        idle();
    s_state = CLOSED;
}
