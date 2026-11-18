/* nts.c -- the signed-time reader (src/kernel/nts.c) given garbage.
 *
 * An input is two answers: a 16-bit length, then that many bytes the key
 * exchange's TLS connection gives (its records), and after them what the
 * time server sends back.  Everything else nts.c leans on is pretended
 * here: a connection that always opens, keys that are always made, a
 * name that always resolves.  The mode picks the ways in: bit 0 takes
 * every signature as good (so the cookies sealed inside an answer are
 * read as well -- a real one could not be forged), bit 1 hands the
 * question's timestamp and identifier back as a real server would, so
 * the answer gets past those checks to the rest, and bit 2 has the
 * network name a plain time server, so the unsigned fallback is read too. */
#include "flopos.h"
#include <string.h>

const char *fuzz_name = "nts";
const u32 fuzz_max = 8 * 1024;

void kshim_reset(void);
static const u8 *s_in;
static u32 s_len, s_at, s_ke_end, s_mode;

int  conn_open(const char *host, u16 port, int secure, void (*note)(const char *t))
{ (void)host; (void)port; (void)secure; (void)note; s_at = 2; return 1; }
int  conn_write(const u8 *d, int n) { (void)d; return n; }
int  conn_read(u8 *b, int n, int wait_ms)
{
    u32 k = s_ke_end - s_at;
    (void)wait_ms;
    if (s_at >= s_ke_end) return 0;
    if (k > 700) k = 700;                    /* a few records at a time */
    if (k > (u32)n) k = (u32)n;
    memcpy(b, s_in + s_at, k);
    s_at += k;
    return (int)k;
}
void conn_close(void) { }
const char *conn_why(void) { return "fuzz"; }
int  conn_busy(void) { return 0; }
int  tls_checking(void) { return 1; }
void tls_check(int on) { (void)on; }
void tls_alpn(const char *p) { (void)p; }
const char *tls_why(void) { return "fuzz"; }
int  tls_export(const char *label, const u8 *ctx, int clen, u8 *out, int olen)
{ (void)label; (void)ctx; (void)clen; memset(out, 7, (size_t)olen); return 1; }
u32  ip_lookup(const char *name) { (void)name; return 0x0A000202u; }
u32  ip_ntp_server(void) { return (s_mode & 4) ? 0x0A000203u : 0; }   /* bit 2: the network names one */
int  store_clockset_get(u32 *trust, char server[3][64]) { (void)trust; (void)server; return 0; }
int  store_clockset_set(u32 trust, const char server[3][64]) { (void)trust; (void)server; return 1; }

int ip_udp_ask(u32 to, u16 port, const u8 *q, int qlen, u8 *ans, int max, int wait_ms)
{
    u32 n = s_len - s_ke_end, i;
    (void)to; (void)port; (void)wait_ms;
    if (n > (u32)max) n = (u32)max;
    memcpy(ans, s_in + s_ke_end, n);
    if ((s_mode & 2) && n >= 48 && qlen >= 48) {
        memcpy(ans + 24, q + 40, 8);         /* our timestamp, carried back */
        for (i = 48; qlen >= 84 && i + 36 <= n; i += 4)    /* and our identifier, where it goes */
            if (ans[i] == 0x01 && ans[i + 1] == 0x04) { memcpy(ans + i + 4, q + 52, 32); break; }
    }
    return (int)n;
}

int real_aessiv_open(const u8 key[32], const u8 *const *ad, const u32 *adlen, int nad,
                     const u8 *in, u32 n, u8 *p);
int aessiv_open(const u8 key[32], const u8 *const *ad, const u32 *adlen, int nad,
                const u8 *in, u32 n, u8 *p)
{
    if (!(s_mode & 1)) return real_aessiv_open(key, ad, adlen, nad, in, n, p);
    if (n < 16) return 0;
    memcpy(p, in + 16, n - 16);              /* taken as good: read what is inside */
    return 1;
}

void fuzz_one(const u8 *data, u32 len, u32 mode)
{
    if (len < 2) return;
    s_in = data; s_len = len; s_mode = mode;
    s_ke_end = 2 + ((u32)data[0] << 8 | data[1]);
    if (s_ke_end > len) s_ke_end = len;
    kshim_reset();
    (void)nts_time();
    (void)nts_time();                        /* again, with what cookies it kept */
}
