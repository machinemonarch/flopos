/* tls.c -- the secure connection (kernel/tls.c), given a garbage server.
 *
 * An input is everything a server sent in one real conversation, as it
 * came off the wire.  The random numbers here are the same every time
 * (kshim.c), so FLOPOS makes the same keys as when the conversation was
 * recorded and an undamaged input plays back to the end.  Damage in the
 * sealed parts would only fail the seal and be thrown away unread, so
 * here the seals are opened whatever they say: a flipped bit comes
 * through as a flipped bit, and the code behind the seal gets garbage
 * to read as well.
 *
 *   tls --record PORT OUT [pq]     talk to a server on this PC, keep what it said
 */
#include "flopos.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

const char *fuzz_name = "tls";
const u32 fuzz_max = 48 * 1024;

void kshim_reset(void);
int real_aead_open(const u8 key[32], const u8 nonce[12], const u8 *aad, u32 alen,
                   const u8 *ct, u32 len, const u8 tag[16], u8 *pt);
int real_aesgcm_open(const u8 key[16], const u8 iv[12], const u8 *aad, u32 alen,
                     const u8 *ct, u32 len, const u8 tag[16], u8 *pt);

static int s_sock = -1;                  /* recording: a real server        */
static FILE *s_keep;
static u8 *s_in; static u32 s_len, s_at, s_mode;
static int s_honest;                     /* seals must hold (recording)     */

int aead_open(const u8 key[32], const u8 nonce[12], const u8 *aad, u32 alen,
              const u8 *ct, u32 len, const u8 tag[16], u8 *pt)
{
    int ok = real_aead_open(key, nonce, aad, alen, ct, len, tag, pt);
    return s_honest ? ok : 1;
}
int aesgcm_open(const u8 key[16], const u8 iv[12], const u8 *aad, u32 alen,
                const u8 *ct, u32 len, const u8 tag[16], u8 *pt)
{
    int ok = real_aesgcm_open(key, iv, aad, alen, ct, len, tag, pt);
    return s_honest ? ok : 1;
}

int tcp_write(const u8 *data, int len)
{
    if (s_sock >= 0) return (int)send(s_sock, data, (size_t)len, 0);
    { volatile u8 x = 0; int i; for (i = 0; i < len; i++) x ^= data[i]; (void)x; }
    return len;
}
int tcp_read(u8 *buf, int max, int wait_ms)
{
    (void)wait_ms;
    if (s_sock >= 0) {
        int n = (int)recv(s_sock, buf, (size_t)max, 0);
        if (n > 0 && s_keep) fwrite(buf, 1, (size_t)n, s_keep);
        return n > 0 ? n : 0;
    }
    {
        u32 piece = 1 + ((s_mode >> 6) & 0x7FF);
        if (s_at >= s_len) return ((s_mode >> 4) & 3) == 3 ? -2 : 0;
        if (piece > s_len - s_at) piece = s_len - s_at;
        if ((int)piece > max) piece = (u32)max;
        memcpy(buf, s_in + s_at, piece);
        s_at += piece;
        return (int)piece;
    }
}
int tcp_done(void) { return s_sock >= 0 ? 0 : s_at >= s_len; }

static void talk(int pq, int check)
{
    static u8 page[4096];
    int i;
    kshim_reset();
    tls_offer_pq(pq);
    tls_check(check);
    if (tls_connect("fuzz.test")) {
        tls_write((const u8 *)"GET / HTTP/1.0\r\n\r\n", 18);
        for (i = 0; i < 40 && tls_read(page, (int)sizeof page, 100) > 0; i++) { }
    }
    tls_close();
}

int fuzz_extra(int argc, char **argv)
{
    struct sockaddr_in a;
    if (argc < 4) return 2;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons((unsigned short)atoi(argv[2]));
    a.sin_addr.s_addr = inet_addr("127.0.0.1");
    s_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (connect(s_sock, (struct sockaddr *)&a, sizeof a)) { fprintf(stderr, "no server on port %s\n", argv[2]); return 1; }
    if (!(s_keep = fopen(argv[3], "wb"))) return 2;
    s_honest = 1;
    talk(argc > 4, 1);
    fclose(s_keep);
    close(s_sock);
    printf("%s\n", tls_why()[0] ? tls_why() : "ok");
    return tls_why()[0] ? 1 : 0;
}

void fuzz_one(const u8 *data, u32 len, u32 mode)
{
    s_in = malloc(len ? len : 1);
    memcpy(s_in, data, len);
    s_len = len; s_at = 0; s_mode = mode;
    s_honest = 0;
    /* With or without the post-quantum offer (as each was recorded: the
     * low bit of the input's first... the mode tries both), and with the
     * server's proof asked for or waived ("go there anyway"). */
    talk(mode & 1, (mode >> 1) & 3 ? 1 : 0);
    if (getenv("FUZZ_SAY")) printf("%s\n", tls_why()[0] ? tls_why() : "connected and read");   /* for --one */
    free(s_in);
}
