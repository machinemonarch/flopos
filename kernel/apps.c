/* ===========================================================================
 * apps.c -- running programs that live on the floppy, not in the kernel.
 *
 * The kernel sits in the first 640KB, under a ceiling set in 1981, and
 * every feature built into it eats into what is left.  An app lives in
 * its own file instead -- WRITE.APP for `write` -- and is read into a
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
#include "../apps/app.h"
#include "version.h"

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
static int  api_write(const AppFile *f, const u8 *b)
{
    int ok = fs_write_inplace((const FsFile *)f, b);
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
};

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
        term_print("Apps need 18MB of memory; this machine has less.");
        term_newline();
        return 1;
    }
    if (f.size < sizeof(AppHeader) || f.size > APP_MAX) {
        term_printf("%s is not an app I can load.", file);
        term_newline();
        return 1;
    }
    if (fs_read(&f, (u8 *)APP_BASE, f.size) != (int)f.size) {
        term_printf("The floppy would not give up %s.", file);
        term_newline();
        return 1;
    }
    if (h->magic[0] != 'F' || h->magic[1] != 'L' || h->magic[2] != 'O' ||
        h->magic[3] != 'P' || h->magic[4] != 'A' || h->magic[5] != 'P' ||
        h->magic[6] != 'P' || h->load_addr != APP_BASE ||
        h->bss_end > APP_BASE + APP_MAX || h->image_end > h->bss_end ||
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
    r = ((AppMain)h->entry)(&s_api, argc, argv);
    s_app_bytes = 0;
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
