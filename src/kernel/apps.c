/* ===========================================================================
 * apps.c -- running programs that live on the floppy, not in the kernel.
 *
 * The kernel sits in the first 640KB, under a ceiling set in 1981, and
 * every feature built into it eats into what is left.  An app lives in
 * its own file instead -- SCRIBE.APP for `scribe` -- and is read into a
 * 4MB space of its own at 10MB only when it is asked for, which also
 * means it costs nothing at boot.
 *
 * The app gets a table of the kernel's services (app.h) and nothing else.
 * It runs with the machine to itself, as everything does here: there is
 * no protection, so a broken app can still take the kernel down -- the
 * point of this is room to grow, not a wall between them.
 * ===========================================================================
 */
#include "flopos.h"
#include "io.h"
#include "inflate.h"
#include "../apps/sdk/app.h"
#include "gen/version.h"

/* The app header's copies of kernel constants, checked against the
 * originals here so that the two can never quietly disagree. */
_Static_assert(A_TEXT == C_TEXT, "app.h colour");
_Static_assert(A_DIM == C_DIM, "app.h colour");
_Static_assert(A_BRIGHT == C_BRIGHT, "app.h colour");
_Static_assert(A_ACCENT == C_ACCENT, "app.h colour");
_Static_assert(A_GREY == C_GREY, "app.h colour");
_Static_assert(A_BANANA == PAL_BANANA + 6, "app.h colour");
_Static_assert(A_PAPER == WIN_PAPER, "app.h colour");
_Static_assert(A_HILITE == WIN_HILITE, "app.h colour");
_Static_assert(AT_BOLD == ATTR_BOLD && AT_ITALIC == ATTR_ITALIC &&
               AT_UNDERLINE == ATTR_UNDERLINE, "app.h attributes");
_Static_assert(A_FR_H == FR_H && A_FR_V == FR_V && A_FR_TL == FR_TL &&
               A_FR_TR == FR_TR && A_FR_BL == FR_BL && A_FR_BR == FR_BR &&
               A_FR_TEE_L == FR_TEE_L && A_FR_TEE_R == FR_TEE_R, "app.h frames");
_Static_assert(K_UP == UK_UP && K_DOWN == UK_DOWN && K_LEFT == UK_LEFT &&
               K_RIGHT == UK_RIGHT && K_ENTER == UK_ENTER && K_ESC == UK_ESC &&
               K_BKSP == UK_BKSP && K_TAB == UK_TAB && K_BTAB == UK_BTAB &&
               K_PGUP == UK_PGUP && K_PGDN == UK_PGDN && K_HOME == UK_HOME &&
               K_END == UK_END && K_DEL == UK_DEL, "app.h keys");
_Static_assert(M_SHIFT == KEV_SHIFT && M_CTRL == KEV_CTRL && M_ALT == KEV_ALT, "app.h mods");
_Static_assert(sizeof(AppFile) == sizeof(FsFile), "app.h files");
_Static_assert(A_CH_FILL == CH_FILL, "app.h fill");

/* The connections, in conn.c. */
int  net_online(void);
int  conn_open(const char *host, u16 port, int secure, void (*note)(const char *t));
int  conn_write(const u8 *d, int n);
int  conn_read(u8 *b, int n, int wait_ms);
void conn_close(void);
const char *conn_why(void);
void conn_info(ConnInfo *ci);

static const char *api_version(void) { return FLOPOS_VERSION; }
static void api_wire(const char *t) { if (serial_present()) serial_print(t); }
_Static_assert(A_CH_ARING == CH_ARING && A_CH_ADIA == CH_ADIA && A_CH_ODIA == CH_ODIA &&
               A_CH_ARING_L == CH_ARING_L && A_CH_ADIA_L == CH_ADIA_L &&
               A_CH_ODIA_L == CH_ODIA_L, "app.h letters");
_Static_assert(A_CH_WIDE == CH_WIDE && A_CH_WIDE_ESC == CH_WIDE_ESC, "app.h wide characters");
_Static_assert(A_CH_IEXCL == CH_IEXCL && A_CH_EURO == CH_EURO && A_CH_DEGREE == CH_DEGREE &&
               A_CH_LATIN == CH_LATIN && A_LATIN_MASK0 == LATIN_MASK0 &&
               A_LATIN_MASK1 == LATIN_MASK1 && A_LATIN_MASK2 == LATIN_MASK2,
               "app.h's other letters must match the font: copy LATIN_MASK* from fontmap.h");

static int  api_win_open(int w, int h) { return term_win_open(w, h); }
static void api_win_put(int x, int y, int ch, u8 fg, u8 bg, u8 at) { term_win_put_at(x, y, ch, fg, bg, at); }
static int  api_key(int *mods) { return ui_key_mods(mods); }
static int  api_find(const char *n, AppFile *f) { return fs_find(n, (FsFile *)f); }

/* The journal and what FLOPOS has learned live in memory while it runs
 * (store.c).  An app that reads those two files gets them as they stand
 * now, and one that writes them is believed: memory follows the floppy. */
static int kept_file(const AppFile *f)
{
    FsFile k;
    return (fs_find("JOURNAL.TXT", &k) && k.first == f->first) ||
           (fs_find("STATE.BIN", &k) && k.first == f->first);
}
static int  api_read(const AppFile *f, u8 *b, u32 m)
{
    if (kept_file(f) && store_unsaved()) store_save();
    return fs_read((const FsFile *)f, b, m);
}
/* Only three of the system's files may be written in place by an app:
 * the bookmarks, and the two the tape puts back.  Never the system itself. */
static int may_write(const AppFile *f)
{
    static const char *const names[] = { "BOOKMARK.TXT", "JOURNAL.TXT", "STATE.BIN" };
    FsFile k;
    int i;
    for (i = 0; i < 3; i++)
        if (fs_find(names[i], &k) && k.first == f->first && k.size == f->size) return 1;
    return 0;
}
static int  api_write(const AppFile *f, const u8 *b)
{
    int ok = may_write(f) && fs_write_inplace((const FsFile *)f, b);
    if (ok && kept_file(f)) store_reload();
    return ok;
}

_Static_assert(sizeof(InvItem) == sizeof(FsItem), "app.h inventory items");
static int api_inv_list(const char *dir, void (*fn)(const InvItem *it, void *ctx), void *ctx)
{
    return inv_list(dir, (void (*)(const FsItem *, void *))fn, ctx);
}
static int inv_stat_app(const char *path, InvItem *it) { return inv_stat(path, (FsItem *)it); }

/* What an app asked to have run after it. */
static char s_then[INV_PATH + 16];
static void api_then(const char *command)
{
    u32 i;
    for (i = 0; command[i] && i < sizeof s_then - 1; i++) s_then[i] = command[i];
    s_then[i] = 0;
}

/* The date and time, from the clock chip: year less 2000, month, day,
 * hour, minute, second.  0: the clock cannot be read. */
static int api_clock(u8 out[6])
{
    u8 h, m, sec, mo, d;
    u16 y;
    if (!rtc_read(&h, &m, &sec, &y, &mo, &d)) return 0;
    out[0] = (u8)(y - 2000); out[1] = mo; out[2] = d; out[3] = h; out[4] = m; out[5] = sec;
    return 1;
}

/* A letter's shape in the system's face: five columns, bit 0 at the top,
 * eight rows.  0: the font has no such letter. */
static int api_glyph(int ch, u8 cols[5])
{
    int g = glyph_index(ch), i;
    if (g < 0 || g >= FONT_GLYPHS) return 0;
    for (i = 0; i < 5; i++) cols[i] = g_font5x8[g][i];
    return 1;
}

static int api_room_task(const u8 *task, u32 len, u8 *answer, u32 cap, u32 ms,
                         int (*door)(u8 *buf, u32 len, u32 cap), u8 *door_buf, u32 door_cap)
{
    return room_task(task, len, answer, cap, ms, (u32)door, door_buf, door_cap);
}

static void api_cbc(const u8 *key, const u8 *iv, const u8 *in, u32 n, u8 *out)
{
    aes_cbc_decrypt(key, 16, iv, in, out, n & ~15u);
}

static const Api s_api = {
    APP_API_VERSION,
    api_win_open, term_win_close, term_win_width, term_win_height,
    api_win_put, term_win_text, term_win_band, term_win_frame,
    term_cols, term_rows,
    term_print, term_newline, term_colour,
    api_key, main_pump, ms_now,
    api_find, api_read, api_write, fs_each,
    fmt_str,
    /* version 2 */
    api_version, term_putc, term_plain, api_wire,
    net_online, conn_open, conn_write, conn_read, conn_close, conn_why, conn_info,
    tls_check,
    /* version 3 */
    audio_open, audio_space, audio_queued, audio_write, audio_close,
    /* version 4 */
    audio_feeder,
    /* version 5 */
    audio_listen, audio_heard,
    /* version 6 */
    api_inv_list, inv_stat_app, inv_read, inv_write, inv_mkdir, inv_remove, inv_move, inv_room, inv_why,
    api_then,
    /* version 7 */
    mood_ember,
    /* version 8 */
    crypt_key, crypt_seal, crypt_open, crypt_random,
    /* version 9 */
    api_clock, api_glyph,
    /* version 10 */
    room_open, api_room_task, room_why,
    /* version 11 */
    snd_hush,
    /* version 12 */
    api_cbc, inv_size,
};

/* ---- behind the wall (wall.c): one request from an app, checked -----------
 * n is which function of the table, a its arguments as the app pushed
 * them.  Everything that is a pointer is held against the app's own
 * memory before it is used: a string must end inside it, a buffer must
 * lie inside it for its whole length, a routine the system is to call
 * must be inside it.  Returns 0 if the app asked for something it may
 * not, and it is then closed. */
#define APP_END (APP_BASE + APP_MAX)
static int ok_buf(u32 at, u32 len) { return at >= APP_BASE && at <= APP_END && len <= APP_END - at; }
static int ok_str(u32 at)
{
    const char *s = (const char *)at;
    u32 i;
    if (!ok_buf(at, 1)) return 0;
    for (i = 0; at + i < APP_END; i++) if (!s[i]) return 1;
    return 0;
}

/* The system calls back into the app through these; what the app is
 * handed (a name, an item, a line of text) is copied into its own memory
 * first, since it cannot read the system's. */
static u32 s_cb_each, s_cb_inv, s_cb_note, s_cb_feed;
static void each_back(const char *name, u32 size, void *ctx)
{
    char *d = (char *)wall_scratch();
    int i;
    for (i = 0; name[i] && i < 15; i++) d[i] = name[i];
    d[i] = 0;
    wall_call(s_cb_each, (u32)d, size, (u32)ctx);
}
static void inv_back(const FsItem *it, void *ctx)
{
    FsItem *d = (FsItem *)(wall_scratch() + 32);
    *d = *it;
    wall_call(s_cb_inv, (u32)d, (u32)ctx, 0);
}
static void note_back(const char *t)
{
    char *d = (char *)(wall_scratch() + 96);
    int i;
    for (i = 0; t[i] && i < 255; i++) d[i] = t[i];
    d[i] = 0;
    wall_call(s_cb_note, (u32)d, 0, 0);
}
static void feed_back(void) { wall_call(s_cb_feed, 0, 0, 0); }

/* format(out, cap, fmt, ...): the arguments are words on the app's stack;
 * each %s among them is a pointer and is checked like any other string. */
static int ask_format(const u32 *a)
{
    const char *f = (const char *)a[2];
    u32 args[12];
    int n = 0;
    if (!ok_buf(a[0], a[1]) || !a[1] || !ok_str(a[2])) return 0;
    for (; *f; f++) {
        if (*f != '%') continue;
        f++;
        while (*f >= '0' && *f <= '9') f++;
        if (!*f) break;
        if (*f == '%') continue;
        if (n >= 12) return 0;
        if (*f == 's' && a[3 + n] && !ok_str(a[3 + n])) return 0;
        args[n] = a[3 + n];
        n++;
    }
    for (; n < 12; n++) args[n] = 0;
    fmt_str((char *)a[0], a[1], (const char *)a[2], args[0], args[1], args[2], args[3], args[4], args[5],
            args[6], args[7], args[8], args[9], args[10], args[11]);
    return 1;
}

int app_ask(u32 n, const u32 *a, u32 *result)
{
    const Api *k = &s_api;
    u32 r = 0;
    AppFile f;

#define STR(i)     do { if (!ok_str(a[i])) return 0; } while (0)
#define BUF(i, n)  do { if ((s32)(n) < 0 || !ok_buf(a[i], (u32)(n))) return 0; } while (0)
#define FN(i)      do { if (a[i] && !ok_buf(a[i], 1)) return 0; } while (0)
    switch (n) {
    case 0:  r = (u32)k->win_open((int)a[0], (int)a[1]); break;
    case 1:  k->win_close(); break;
    case 2:  r = (u32)k->win_width(); break;
    case 3:  r = (u32)k->win_height(); break;
    case 4:  k->win_put((int)a[0], (int)a[1], (int)a[2], (u8)a[3], (u8)a[4], (u8)a[5]); break;
    case 5:  STR(2); r = (u32)k->win_text((int)a[0], (int)a[1], (const char *)a[2], (u8)a[3], (u8)a[4]); break;
    case 6:  k->win_band((int)a[0], (int)a[1], (int)a[2], (u8)a[3]); break;
    case 7:  if (a[0]) STR(0); k->win_frame((const char *)a[0], (u8)a[1]); break;
    case 8:  r = (u32)k->screen_cols(); break;
    case 9:  r = (u32)k->screen_rows(); break;
    case 10: STR(0); k->print((const char *)a[0]); break;
    case 11: k->newline(); break;
    case 12: k->colour((u8)a[0]); break;
    case 13: if (a[0]) BUF(0, 4); r = (u32)k->key((int *)a[0]); break;
    case 14: k->pump((int)a[0]); break;
    case 15: r = k->ms(); break;
    case 16: STR(0); BUF(1, sizeof f); r = (u32)k->file_find((const char *)a[0], (AppFile *)a[1]); break;
    case 17:                                     /* file_read: the file must be one that exists */
        BUF(0, sizeof f); f = *(const AppFile *)a[0];
        BUF(1, a[2]);
        if (!fs_is_file((const FsFile *)&f)) { r = (u32)-1; break; }
        r = (u32)k->file_read(&f, (u8 *)a[1], a[2]);
        break;
    case 18:                                     /* file_write: as long as the file is */
        BUF(0, sizeof f); f = *(const AppFile *)a[0];
        if (!may_write(&f)) { r = 0; break; }
        BUF(1, f.size);
        r = (u32)k->file_write(&f, (const u8 *)a[1]);
        break;
    case 19: FN(0); if (!a[0]) break; s_cb_each = a[0]; k->file_each(each_back, (void *)a[1]); break;
    case 20: if (!ask_format(a)) return 0; break;
    case 21: r = wall_string(0, k->os_version()); break;
    case 22: k->putc((int)a[0]); break;
    case 23: k->plain((int)a[0]); break;
    case 24: STR(0); k->wire((const char *)a[0]); break;
    case 25: r = (u32)k->net_online(); break;
    case 26: STR(0); FN(3); s_cb_note = a[3];
             r = (u32)k->conn_open((const char *)a[0], (u16)a[1], (int)a[2], a[3] ? note_back : 0); break;
    case 27: BUF(0, a[1]); r = (u32)k->conn_write((const u8 *)a[0], (int)a[1]); break;
    case 28: BUF(0, a[1]); r = (u32)k->conn_read((u8 *)a[0], (int)a[1], (int)a[2]); break;
    case 29: k->conn_close(); break;
    case 30: r = wall_string(1, k->conn_why()); break;
    case 31: BUF(0, sizeof(ConnInfo)); k->conn_info((ConnInfo *)a[0]); break;
    case 32: k->conn_check((int)a[0]); break;
    case 33: r = (u32)k->audio_open((int)a[0]); break;
    case 34: r = (u32)k->audio_space(); break;
    case 35: r = (u32)k->audio_queued(); break;
    case 36: BUF(0, a[1]); r = (u32)k->audio_write((const u8 *)a[0], (int)a[1]); break;
    case 37: k->audio_close(); break;
    case 38: FN(0); s_cb_feed = a[0]; k->audio_feeder(a[0] ? feed_back : 0); break;
    case 39: r = (u32)k->audio_listen((int)a[0], (int)a[1]); break;
    case 40: BUF(0, a[1]); r = (u32)k->audio_heard((u8 *)a[0], (int)a[1]); break;
    case 41: STR(0); FN(1); if (!a[1]) break; s_cb_inv = a[1];
             r = (u32)inv_list((const char *)a[0], inv_back, (void *)a[2]); break;
    case 42: STR(0); BUF(1, sizeof(InvItem)); r = (u32)k->inv_stat((const char *)a[0], (InvItem *)a[1]); break;
    case 43: STR(0); BUF(1, a[2]); r = (u32)k->inv_read((const char *)a[0], (u8 *)a[1], a[2]); break;
    case 44: STR(0); BUF(1, a[2]); r = (u32)k->inv_write((const char *)a[0], (const u8 *)a[1], a[2]); break;
    case 45: STR(0); r = (u32)k->inv_mkdir((const char *)a[0]); break;
    case 46: STR(0); r = (u32)k->inv_remove((const char *)a[0]); break;
    case 47: STR(0); STR(1); r = (u32)k->inv_move((const char *)a[0], (const char *)a[1]); break;
    case 48: r = k->inv_room(); break;
    case 49: r = wall_string(2, k->inv_why()); break;
    case 50: STR(0); k->then((const char *)a[0]); break;
    case 51: k->win_ember((int)a[0]); break;
    case 52: STR(0); BUF(1, a[2]); BUF(4, 32);
             k->crypt_key((const char *)a[0], (const u8 *)a[1], a[2], a[3] > 200000 ? 200000 : a[3], (u8 *)a[4]); break;
    case 53: BUF(1, 32); BUF(2, 12); BUF(3, a[4]); BUF(5, a[6]); BUF(7, a[6]); BUF(8, 16);
             k->crypt_seal((int)a[0], (const u8 *)a[1], (const u8 *)a[2], (const u8 *)a[3], a[4],
                           (const u8 *)a[5], a[6], (u8 *)a[7], (u8 *)a[8]); break;
    case 54: BUF(1, 32); BUF(2, 12); BUF(3, a[4]); BUF(5, a[6]); BUF(7, 16); BUF(8, a[6]);
             r = (u32)k->crypt_open((int)a[0], (const u8 *)a[1], (const u8 *)a[2], (const u8 *)a[3], a[4],
                                    (const u8 *)a[5], a[6], (const u8 *)a[7], (u8 *)a[8]); break;
    case 55: BUF(0, a[1]); k->random((u8 *)a[0], a[1]); break;
    case 56: BUF(0, 6); r = (u32)k->clock((u8 *)a[0]); break;
    case 57: BUF(1, 5); r = (u32)k->glyph((int)a[0], (u8 *)a[1]); break;
    case 58: r = (u32)room_open(); break;
    case 59: BUF(0, a[1]); BUF(2, a[3]); FN(5); BUF(6, a[7]);
             r = (u32)room_task((const u8 *)a[0], a[1], (u8 *)a[2], a[3], a[4], a[5], (u8 *)a[6], a[7]); break;
    case 60: r = wall_string(2, room_why()); break;
    case 61: k->hush((int)a[0]); break;
    case 62: BUF(0, 16); BUF(1, 16); BUF(2, a[3]); BUF(4, a[3]);
             k->crypt_cbc((const u8 *)a[0], (const u8 *)a[1], (const u8 *)a[2], a[3], (u8 *)a[4]); break;
    case 63: r = k->inv_size(); break;
    default: return 0;
    }
#undef STR
#undef BUF
#undef FN
    *result = r;
    return 1;
}
_Static_assert((sizeof(Api) - 4) / 4 == 64, "a function was added to the table: give it a case in app_ask");

static int memory_for_apps(void)
{
    u32 top = 0x00100000u + BOOTINFO_EXTKB * 1024u;
    return !heap_is_low() && top >= APP_BASE + APP_MAX;
}

/* Run NAME.APP if there is one.  Returns 0 if there is no such app, so
 * the shell can carry on and say it does not know the word. */
static u32 s_app_bytes;                  /* the open app's memory, or 0     */
int app_active(void) { return s_app_bytes != 0; }
u32 app_bytes(void)  { return s_app_bytes; }

/* The file an app's word names: the word and .APP.  A word longer than
 * eight letters has its whole name on the floppy as a long name
 * (INVENTORY.APP), which is what another computer shows, and FAT12's own
 * short name beside it, which is the one looked for here. */
static const struct { const char *word, *file; } s_long[] = {
    { "inventory", "INVENT~1.APP" },
};
#define LONGS ((int)(sizeof s_long / sizeof s_long[0]))

static int app_file(const char *word, char file[13])
{
    int i, k, n = 0;
    for (k = 0; k < LONGS; k++) {
        const char *w = s_long[k].word;
        for (i = 0; w[i] && (word[i] | 32) == w[i]; i++) { }
        if (w[i] || word[i]) continue;
        for (i = 0; s_long[k].file[i]; i++) file[i] = s_long[k].file[i];
        file[i] = 0;
        return 1;
    }
    for (i = 0; word[i]; i++) {
        char c = word[i];
        if (n >= 8) return 0;
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return 0;
        file[n++] = c;
    }
    if (!n) return 0;
    file[n++] = '.'; file[n++] = 'A'; file[n++] = 'P'; file[n++] = 'P'; file[n] = 0;
    return 1;
}

/* Is there an app by this name on the floppy? */
int app_exists(const char *word)
{
    char file[13];
    FsFile f;
    return app_file(word, file) && fs_find(file, &f);
}

static int run_one(const char *word, int argc, char **argv)
{
    char file[13];
    FsFile f;
    AppHeader *h = (AppHeader *)APP_BASE;
    int r;

    if (!app_file(word, file) || !fs_find(file, &f)) return 0;

    term_colour(C_ACCENT);
    if (!memory_for_apps()) {
        term_print("Apps need 24MB of memory; this machine has less.");
        term_newline();
        return 1;
    }
    {
        const char *why = "";
        int got = load_packed(&f, (u8 *)APP_BASE, APP_MAX - APP_STACK, &why);   /* packed or not */
        if (got < (int)sizeof(AppHeader)) {
            term_printf("%s did not load: %s.", file, got < 0 ? why : "it is too short");
            term_newline();
            return 1;
        }
        f.size = (u32)got;                       /* from here on, as unpacked */
    }
    if (h->magic[0] != 'F' || h->magic[1] != 'L' || h->magic[2] != 'O' ||
        h->magic[3] != 'P' || h->magic[4] != 'A' || h->magic[5] != 'P' ||
        h->magic[6] != 'P' || h->load_addr != APP_BASE ||
        h->bss_end > APP_BASE + APP_MAX - APP_STACK || h->image_end > h->bss_end ||
        h->entry < APP_BASE || h->entry >= h->image_end) {
        term_printf("%s does not look like an app.", file);
        term_newline();
        return 1;
    }
    /* The table only ever grows at the end, so an app built for an older
     * version still finds everything it knows about where it expects. */
    if (h->api_version == 0 || h->api_version > APP_API_VERSION) {
        term_printf("%s was built for a newer FLOPOS (it wants version %u, this is %u).",
                    file, h->api_version, (u32)APP_API_VERSION);
        term_newline();
        return 1;
    }

    memset8((void *)h->image_end, 0, h->bss_end - h->image_end);
    serial_printf("[app] %s: %u bytes, %u in memory\n", file, f.size, h->bss_end - APP_BASE);
    s_app_bytes = h->bss_end - APP_BASE;
    /* Behind the wall, at its own privilege and on its own stack (the top
     * of its memory); or, on a CPU that cannot do that, as part of the
     * system, the way it used to be. */
    if (wall_on()) { wall_protect(h->ro_end); r = wall_run(h->entry, argc, argv); wall_unprotect(); }
    else           r = ((AppMain)h->entry)(&s_api, argc, argv);
    s_app_bytes = 0;
    mood_ember(0);                       /* frames back to neon, keys to pink */
    snd_hush(0);                         /* the music and key sounds may play again */
    audio_feeder(0);                     /* its memory is no longer its own */
    audio_close();
    term_win_close();
    (void)r;
    return 1;
}

/* Run the app, and then whatever it asked to have run after it: an app
 * hands over to another that way (the inventory to write, and back). */
int app_run(const char *word, int argc, char **argv)
{
    static char line[sizeof s_then];
    static char *av[6];
    int hops = 0;

    s_then[0] = 0;
    if (!run_one(word, argc, argv)) return 0;
    while (s_then[0] && hops++ < 64) {
        u32 i;
        int n = 0;
        for (i = 0; i < sizeof line; i++) line[i] = s_then[i];
        s_then[0] = 0;
        for (i = 0; line[i] && n < 6; ) {
            while (line[i] == ' ') i++;
            if (!line[i]) break;
            av[n++] = line + i;
            while (line[i] && line[i] != ' ') i++;
            if (line[i]) line[i++] = 0;
        }
        if (!n || !run_one(av[0], n, av)) break;
    }
    return 1;
}

/* ---- `apps`: what is on the floppy to run ------------------------------- */
static void list_one(const char *name, u32 size, void *ctx)
{
    int i, k, n = 0;
    int *count = (int *)ctx;
    for (i = 0; name[i]; i++) n = i;
    if (n < 4 || name[n - 3] != '.' || name[n - 2] != 'A' || name[n - 1] != 'P' || name[n] != 'P')
        return;
    term_colour(C_BRIGHT);
    term_print("  ");
    for (k = 0; k < LONGS; k++) {            /* a long name is shown whole */
        for (i = 0; name[i] && name[i] == s_long[k].file[i]; i++) { }
        if (!name[i] && !s_long[k].file[i]) break;
    }
    if (k < LONGS) term_print(s_long[k].word);
    else for (i = 0; i < n - 3; i++) term_putc(name[i] >= 'A' && name[i] <= 'Z' ? name[i] + 32 : name[i]);
    term_colour(C_DIM);
    term_printf("   %u kb", (size + 1023) / 1024);
    term_newline();
    (*count)++;
}

void apps_list(void)
{
    int count = 0;
    term_colour(C_DIM);
    term_print("On this floppy:");
    term_newline();
    fs_each(list_one, &count);
    if (!count) { term_colour(C_DIM); term_print("  nothing yet."); term_newline(); }
}
