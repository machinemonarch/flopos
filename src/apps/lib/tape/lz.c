/* lz.c -- LZSS, to make a file shorter on the tape.
 *
 * The method is Storer and Szymanski's of 1982, as Haruhiko Okumura's
 * well-known LZSS.C of 1989 laid it out (this is not his code): a flag
 * byte, then eight items, lowest bit first; a set bit is one byte as it
 * is, a clear bit is two bytes saying "the same as LEN bytes, DIST back":
 * DIST-1 in twelve bits, LEN-3 in four.  So a copy reaches 4096 bytes
 * back and runs 3 to 18 bytes.
 */
#include "tape.h"

#define WIN   4096
#define MINM  3
#define MAXM  18
#define HASHN 4096
#define TRIES 48                         /* earlier places looked at, at most */

static int s_head[HASHN];
static int s_prev[WIN];

static u32 hash3(const u8 *p) { return ((u32)p[0] * 2654435761u ^ (u32)p[1] * 40503u ^ (u32)p[2] * 251u) >> 4 & (HASHN - 1); }

static void note(const u8 *in, u32 n, u32 i)
{
    u32 h;
    if (i + MINM > n) return;
    h = hash3(in + i);
    s_prev[i & (WIN - 1)] = s_head[h];
    s_head[h] = (int)i;
}

u32 lz_pack(const u8 *in, u32 n, u8 *out, u32 cap)
{
    u32 i = 0, o = 0, flag_at = 0;
    int bit = 8, k;
    for (k = 0; k < HASHN; k++) s_head[k] = -1;
    while (i < n) {
        u32 best = 0, dist = 0;
        if (bit == 8) {
            if (o >= cap) return 0;
            flag_at = o; out[o++] = 0; bit = 0;
        }
        if (i + MINM <= n) {
            int at = s_head[hash3(in + i)], tries = TRIES;
            while (at >= 0 && i - (u32)at <= WIN && tries--) {
                u32 len = 0, max = n - i < MAXM ? n - i : MAXM;
                while (len < max && in[(u32)at + len] == in[i + len]) len++;
                if (len > best) { best = len; dist = i - (u32)at; if (len == MAXM) break; }
                if (i - (u32)at == WIN) break;
                at = s_prev[(u32)at & (WIN - 1)];
                if (at >= 0 && (u32)at >= i) break;          /* an entry since written over */
            }
        }
        if (best >= MINM) {
            if (o + 2 > cap) return 0;
            out[o++] = (u8)(dist - 1);
            out[o++] = (u8)(((dist - 1) >> 8) | ((best - MINM) << 4));
            while (best--) note(in, n, i++);
        } else {
            if (o >= cap) return 0;
            out[flag_at] |= (u8)(1 << bit);
            out[o++] = in[i];
            note(in, n, i++);
        }
        bit++;
    }
    return o;
}

u32 lz_unpack(const u8 *in, u32 n, u8 *out, u32 cap)
{
    u32 i = 0, o = 0;
    while (i < n) {
        int flags = in[i++], bit;
        for (bit = 0; bit < 8 && i < n; bit++) {
            if (flags >> bit & 1) {
                if (o >= cap) return 0xFFFFFFFFu;
                out[o++] = in[i++];
            } else {
                u32 dist, len;
                if (i + 2 > n) return 0xFFFFFFFFu;
                dist = ((u32)in[i] | ((u32)in[i + 1] & 0x0F) << 8) + 1;
                len = (in[i + 1] >> 4) + MINM;
                i += 2;
                if (dist > o || o + len > cap) return 0xFFFFFFFFu;
                while (len--) { out[o] = out[o - dist]; o++; }
            }
        }
    }
    return o;
}
