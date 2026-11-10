/* ===========================================================================
 * conn.c -- one connection to somewhere, plain or secure, for apps.
 *
 * Everything an app needs to talk to a server without knowing how: look
 * the name up, open TCP, and for a secure connection agree on keys and
 * make the server prove who it is.  What comes back is a pipe to read
 * and write, and a note of what was checked.  browzer uses it for the
 * web; anything else that wants a server can use it the same way.
 *
 * One at a time, because the TCP underneath keeps one conversation.
 *
 * While it works it reports progress through the app's note function,
 * and every failure leaves a whole sentence in conn_why() that an app can
 * show as it is.
 * ===========================================================================
 */
#include "flopos.h"
#include "../apps/app.h"

int  conn_open(const char *host, u16 port, int secure, void (*note)(const char *t));
void conn_close(void);

static char s_why[160];
static int  s_secure, s_open;
static void (*s_note)(const char *);

static void say(const char *t) { if (s_note) s_note(t); }

int net_online(void)
{
    return net_up() && (ip_addr() || ip_apply_saved() || ip_dhcp());
}

int conn_open(const char *host, u16 port, int secure, void (*note)(const char *t))
{
    char line[96];
    u32 to;

    s_why[0] = 0;
    s_note = note;
    if (s_open) conn_close();

    if (!ip_parse(host, &to)) {
        fmt_str(line, sizeof line, "looking up %s...", host);
        say(line);
        to = ip_lookup(host);
        if (!to) { fmt_str(s_why, sizeof s_why, "Nobody knows a %s.", host); return 0; }
    }
    fmt_str(line, sizeof line, "%s ...", host);
    say(line);
    if (!tcp_connect(to, port, 6000)) {
        fmt_str(s_why, sizeof s_why, "Could not reach it: %s.", tcp_why());
        return 0;
    }

    s_secure = secure;
    if (secure) {
        tls_set_note(note);
        say("agreeing on a key...");
        if (!tls_connect(host)) {
            int ok = 0;
            /* The post-quantum handshake was chosen and then failed: that
             * is ours to fix, not the site's, so try once more without it
             * and say so down the wire, where it can be read. */
            if (tls_pq_broke()) {
                serial_printf("[tls] post-quantum handshake with %s failed (%s); retrying with X25519 alone\n",
                              host, tls_why());
                tcp_close();
                if (tcp_connect(to, port, 6000)) {
                    tls_offer_pq(0);
                    ok = tls_connect(host);
                    tls_offer_pq(1);
                }
            }
            if (!ok) {
                if (tls_untrusted())
                    fmt_str(s_why, sizeof s_why, "It could not prove it is %s: %s.", host, tls_why());
                else
                    fmt_str(s_why, sizeof s_why, "The secure handshake failed: %s.", tls_why());
                tcp_close();
                return 0;
            }
        }
        serial_printf("[tls] %s: %s, %s, %s\n", host, tls_cipher(),
                      tls_kex(),
                      tls_checked() ? tls_who() : "NOT CHECKED");
    }
    s_open = 1;
    return 1;
}

int conn_write(const u8 *d, int n)
{
    if (!s_open) return 0;
    return s_secure ? tls_write(d, n) : tcp_write(d, n);
}

/* >0 bytes, 0 when the other end has finished, -1 when it went quiet for
 * wait_ms, -2 when the connection broke. */
int conn_read(u8 *b, int n, int wait_ms)
{
    if (!s_open) return 0;
    return s_secure ? tls_read(b, n, wait_ms) : tcp_read(b, n, wait_ms);
}

void conn_close(void)
{
    if (s_open && s_secure) tls_close();
    if (s_open) tcp_close();
    s_open = 0;
}

const char *conn_why(void)
{
    if (s_why[0]) return s_why;
    return s_secure ? tls_why() : tcp_why();
}

void conn_info(ConnInfo *ci)
{
    int i;
    const char *w = tls_who(), *c = tls_cipher();
    ci->secure = s_secure;
    ci->pq = s_secure && tls_post_quantum();
    ci->checked = s_secure && tls_checked();
    ci->untrusted = tls_untrusted();
    for (i = 0; w[i] && i < (int)sizeof ci->who - 1; i++) ci->who[i] = w[i];
    ci->who[i] = 0;
    for (i = 0; c[i] && i < (int)sizeof ci->cipher - 1; i++) ci->cipher[i] = c[i];
    ci->cipher[i] = 0;
}
