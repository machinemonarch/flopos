/* browzer.c -- the whole browser (apps/browzer), given garbage from the
 * network.
 *
 * The app is built for this PC and handed a pretend system: every
 * connection it opens answers with the input, in pieces, and the keys
 * are a script made from the mode (Tab, Enter, arrows, typing, then Esc
 * until it leaves).  So the input is what a web server says: the status
 * line, the headers, and the page, song or stream after them.
 *
 * Ways in: `browzer fetch`, which reads a page out as text, and the
 * browser proper, which lays it out, follows links, fills in forms and
 * plays what it is given.  Every page is built into a tree and its
 * styles read (dom.c, css.c).  In half the runs the site's scripts are
 * allowed, and a pretend script room knocks on browzer's door (script.c)
 * with requests made from the input -- the door must survive anything
 * the real room could send. */
#include "app.h"
#include "../../room/room.h"
#include "../../room/door.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

const char *fuzz_name = "browzer";
const u32 fuzz_max = 64 * 1024;

int app_main(const Api *api, int argc, char **argv);
void fmt_vstr(char *dst, unsigned long cap, const char *fmt, __builtin_va_list ap);   /* kernel/fmt.c */

static u8 *s_in; static u32 s_len, s_at, s_mode;
static int s_opens, s_keys, s_quiet;
static u32 s_ms;
static void (*s_feeder)(void);
static u8 s_marks[8192];                 /* BOOKMARK.TXT                    */

static int  win_open(int w, int h) { (void)w; (void)h; return 1; }
static void win_close(void) { }
static int  win_width(void) { return 78; }
static int  win_height(void) { return 34; }
static void win_put(int x, int y, int ch, u8 fg, u8 bg, u8 attr) { (void)x; (void)y; (void)ch; (void)fg; (void)bg; (void)attr; }
static int  win_text(int x, int y, const char *t, u8 fg, u8 bg) { (void)y; (void)fg; (void)bg; return x + (int)strlen(t); }
static void win_band(int y, int x0, int x1, u8 bg) { (void)y; (void)x0; (void)x1; (void)bg; }
static void win_frame(const char *title, u8 fg) { if (title) (void)strlen(title); (void)fg; }
static int  screen_cols(void) { return 80; }
static int  screen_rows(void) { return 36; }
static void print(const char *t) { (void)strlen(t); }     /* strlen: the text must end */
static void newline(void) { }
static void colour(u8 c) { (void)c; }
static void pump(int draw) { (void)draw; s_ms += 2; if (s_feeder) s_feeder(); }
static u32  ms(void) { return s_ms += 3; }

/* The keys: nothing two times in three, then the next of the script;
 * when the script is over, Esc and q until the app goes. */
static int key(int *mods)
{
    static const int keys[] = { K_TAB, K_TAB, K_ENTER, K_DOWN, K_PGDN, K_TAB, 'x', ' ', K_ENTER, K_BTAB,
                                K_UP, K_BKSP, K_RIGHT, K_LEFT, K_HOME, K_END, K_DEL, K_PGUP, 'b', 'g', '/', '1', K_TAB };
    int n = s_keys++;
    if (mods) *mods = 0;
    if (n > 600) return (n & 1) ? K_ESC : 'y';     /* Esc asks; y leaves */
    if (n % 3) return -1;
    n /= 3;
    if (n >= 24 + (int)((s_mode >> 20) & 31)) return K_ESC;
    return keys[(s_mode >> (n % 13) ^ (u32)n * 7) % (sizeof keys / sizeof keys[0])];
}

static u8 s_sites[2048];                 /* SITES.TXT: fuzz.test allowed, half the time */
static int  file_find(const char *name, AppFile *f)
{
    if (!strcmp(name, "SITES.TXT")) { f->first = 3; f->size = sizeof s_sites; return 1; }
    if (strcmp(name, "BOOKMARK.TXT")) return 0;
    f->first = 2; f->size = sizeof s_marks;
    return 1;
}
static int  file_read(const AppFile *f, u8 *buf, u32 max)
{
    const u8 *src = f->first == 3 ? s_sites : s_marks;
    u32 n = f->size < max ? f->size : max;
    memcpy(buf, src, n);
    return (int)n;
}
static int  file_write(const AppFile *f, const u8 *buf) { memcpy(f->first == 3 ? s_sites : s_marks, buf, f->size); return 1; }
static void file_each(void (*fn)(const char *, u32, void *), void *ctx) { (void)fn; (void)ctx; }
static void format(char *out, u32 cap, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fmt_vstr(out, cap, fmt, ap);
    va_end(ap);
}
static const char *os_version(void) { return "0.0.0"; }
static void put_c(int c) { (void)c; }
static void plain(int on) { (void)on; }
static void wire(const char *t) { (void)strlen(t); }

static int  net_online(void) { return 1; }
static int  conn_open(const char *host, u16 port, int secure, void (*note)(const char *t))
{
    (void)strlen(host); (void)port; (void)secure;
    if (note) note("connecting");
    if (++s_opens > 6) return 0;
    s_at = 0;
    return 1;
}
static int  conn_write(const u8 *d, int n) { volatile u8 x = 0; int i; for (i = 0; i < n; i++) x ^= d[i]; (void)x; return n; }
static int  conn_read(u8 *b, int n, int wait_ms)
{
    u32 piece = 1 + ((s_mode >> 4) & 0x3FF);
    (void)wait_ms;
    s_ms += 5;
    if (s_at >= s_len) return ((s_mode >> 15) & 3) == 3 ? -2 : 0;
    if (((s_mode >> 14) & 1) && (++s_quiet & 7) == 0) return -1;      /* gone quiet for a moment */
    if (piece > s_len - s_at) piece = s_len - s_at;
    if ((int)piece > n) piece = (u32)n;
    memcpy(b, s_in + s_at, piece);
    s_at += piece;
    return (int)piece;
}
static void conn_close(void) { }
static const char *conn_why(void) { return "it did not answer."; }
static void conn_info(ConnInfo *ci) { memset(ci, 0, sizeof *ci); }
static void conn_check(int on) { (void)on; }
static int  audio_open(int rate) { (void)rate; return 1; }
static int  audio_space(void) { return 4096; }
static int  audio_queued(void) { return (int)(s_ms & 1) * 512; }
static int  audio_write(const u8 *p, int n) { volatile u8 x = 0; int i; for (i = 0; i < n; i++) x ^= p[i]; (void)x; return n; }
static void audio_close(void) { s_feeder = 0; }
static void audio_feeder(void (*fn)(void)) { s_feeder = fn; }
static int  audio_listen(int rate, int source) { (void)rate; (void)source; return 0; }
static int  audio_heard(u8 *out, int max) { (void)out; (void)max; return 0; }
static int  inv_list(const char *d, void (*fn)(const InvItem *, void *), void *ctx) { (void)d; (void)fn; (void)ctx; return 0; }
static int  inv_stat(const char *p, InvItem *it) { (void)p; (void)it; return 0; }
static int  inv_read(const char *p, u8 *b, u32 max) { (void)p; (void)b; (void)max; return -1; }
static int  inv_write(const char *p, const u8 *b, u32 n) { volatile u8 x = 0; u32 i; (void)strlen(p); for (i = 0; i < n; i++) x ^= b[i]; (void)x; return 1; }
static int  inv_mkdir(const char *p) { (void)p; return 1; }
static int  inv_remove(const char *p) { (void)p; return 1; }
static int  inv_move(const char *a, const char *b) { (void)a; (void)b; return 1; }
static u32  inv_room(void) { return 600000; }
static const char *inv_why(void) { return "no."; }
static void then(const char *c) { (void)strlen(c); }
static void win_ember(int on) { (void)on; }
static void crypt_key(const char *p, const u8 *salt, u32 sl, u32 it, u8 key[32]) { (void)p; (void)salt; (void)sl; (void)it; memset(key, 0, 32); }
static void crypt_seal(int a, const u8 k[32], const u8 n[12], const u8 *aad, u32 al, const u8 *in, u32 len, u8 *out, u8 tag[16])
{ (void)a; (void)k; (void)n; (void)aad; (void)al; memcpy(out, in, len); memset(tag, 0, 16); }
static int  crypt_open(int a, const u8 k[32], const u8 n[12], const u8 *aad, u32 al, const u8 *in, u32 len, const u8 tag[16], u8 *out)
{ (void)a; (void)k; (void)n; (void)aad; (void)al; (void)tag; memcpy(out, in, len); return 1; }
static void rnd(u8 *out, u32 n) { u32 i; for (i = 0; i < n; i++) out[i] = (u8)(i * 7); }
static int  clock6(u8 out[6]) { out[0] = 26; out[1] = 11; out[2] = 16; out[3] = 12; out[4] = 0; out[5] = 0; return 1; }
static int  glyph(int ch, u8 cols[5]) { (void)ch; memset(cols, 0, 5); return 1; }

/* A pretend script room: every task that would run script knocks on the
 * door a few times with requests made from the input. */
static u32 s_knock;                      /* where in the input the requests come from */
static int room_open(void) { return 1; }
static const char *room_why(void) { return "pretend"; }
static int room_task(const u8 *task, u32 len, u8 *answer, u32 cap, u32 ms_, int (*door)(u8 *, u32, u32), u8 *buf, u32 bcap)
{
    int k, knocks = 0;
    (void)ms_;
    if (len && (task[0] == T_RUN || task[0] == T_CALL || task[0] == T_TIMERS)) knocks = 1 + (int)(s_mode >> 24) % 24;
    for (k = 0; k < knocks && s_len; k++) {
        u32 n = 0, f, at = s_knock;
        u8 op = (u8)(1 + s_in[at % s_len] % (D_COUNT - 1));
        buf[n++] = op;
        for (f = 0; f < 4 && n + 300 < bcap; f++) {
            u8 b = s_in[(at + 1 + f) % s_len];
            if (b & 1) {                  /* a number: small, mostly an element */
                u32 v = (b & 2) ? (u32)(s_in[(at + 5 + f) % s_len] % 64) : (u32)s_in[(at + 9 + f) % s_len] * 977u;
                buf[n++] = (u8)v; buf[n++] = (u8)(v >> 8); buf[n++] = (u8)(v >> 16); buf[n++] = (u8)(v >> 24);
            } else {                      /* a text: a piece of the input   */
                u32 l = (u32)(b >> 1) % 200, i;
                if ((b & 0xF0) == 0xF0) l = 0xFFFFFF00u;  /* a length that lies */
                buf[n++] = (u8)l; buf[n++] = (u8)(l >> 8); buf[n++] = (u8)(l >> 16); buf[n++] = (u8)(l >> 24);
                if (l > 200) l = 0;
                for (i = 0; i < l && n < bcap; i++) buf[n++] = s_in[(at + 13 + i) % s_len];
            }
        }
        s_knock = at + 17 + (u32)op;
        (void)door(buf, n, bcap);
    }
    if (cap < 6) return -1;
    memset(answer, 0, 6);
    return 6;
}


static const Api s_api = {
    APP_API_VERSION,
    win_open, win_close, win_width, win_height, win_put, win_text, win_band, win_frame, screen_cols, screen_rows,
    print, newline, colour, key, pump, ms, file_find, file_read, file_write, file_each, format,
    os_version, put_c, plain, wire,
    net_online, conn_open, conn_write, conn_read, conn_close, conn_why, conn_info, conn_check,
    audio_open, audio_space, audio_queued, audio_write, audio_close, audio_feeder,
    audio_listen, audio_heard,
    inv_list, inv_stat, inv_read, inv_write, inv_mkdir, inv_remove, inv_move, inv_room, inv_why, then,
    win_ember, crypt_key, crypt_seal, crypt_open, rnd, clock6, glyph,
    room_open, room_task, room_why,
};

void fuzz_one(const u8 *data, u32 len, u32 mode)
{
    static char a0[] = "browzer", a1[] = "fetch", url[] = "http://fuzz.test/page?q=1";
    char *argv[3];
    s_in = malloc(len ? len : 1);
    memcpy(s_in, data, len);
    s_len = len; s_at = 0; s_mode = mode; s_opens = 0; s_keys = 0; s_quiet = 0; s_feeder = 0;
    s_knock = 0;
    memset(s_sites, ' ', sizeof s_sites);
    if (mode & 8) memcpy(s_sites, "fuzz.test\n", 10);   /* scripts allowed, half the time */
    argv[0] = a0;
    if ((mode & 7) == 0) { argv[1] = a1; argv[2] = url; app_main(&s_api, 3, argv); }
    else                 { argv[1] = url; app_main(&s_api, 2, argv); }
    free(s_in);
}
