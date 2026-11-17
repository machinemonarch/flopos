/* ===========================================================================
 * maelcum.c -- backup: your files, kept on an audio cassette.
 *
 * Named for the pilot in William Gibson's Neuromancer (1984), who got
 * people home.
 *
 * MAELCUM.APP on the floppy.  One full screen: the settings at the top
 * (for writing, and for reading), a cassette and the progress under them,
 * then a list of files.  The list is empty until there is something to
 * do: W fills it with the floppy's files, a folder at a time, to tick and
 * write; R fills it with what the cassette holds, to tick and restore.
 * Writing plays the ticked files out of the headphone socket as two
 * tones, for a cassette recorder to record; reading listens to the
 * recorder playing them back, keeps the whole save in memory, and puts
 * the files you tick on the floppy, in a folder of their own named by
 * the date and time.  The sound itself is src/apps/lib/tape.
 *
 * Only what is yours goes to tape or comes back from it: everything in
 * the inventory, and three of the system's own records (the bookmarks,
 * the journal and the settings).  What comes back goes into the
 * inventory, or over one of those three; nothing a tape says can touch
 * any other file.
 *
 * On the tape, a save is a long leader and then each file in turn:
 *
 *      HEAD (twice)   its name with its folders, full size, how much of it
 *                     is sent, the byte the rest is filled with, which
 *                     file of how many this is, a check of the whole, this
 *                     save's number, whether it is one of the system's,
 *                     and its packed length (0: sent as it is)
 *      DATA ...       256 bytes a block; the block's number carries which
 *                     file it belongs to
 *      END  (twice)
 *
 * A file that ends in a long run of one byte (the system's three are
 * mostly empty filling) is sent without the run; with packing on, what is
 * left is sent as LZSS (src/apps/lib/tape/lz.c) when that is shorter.  A block
 * that is spoiled is simply missing: the tape can be played again and
 * only the gaps are filled.
 * ===========================================================================
 */
#include "app.h"
#include "tape.h"
#include "sleeve.h"

#define PIECES   255                     /* a file's most blocks: 65280 bytes */
#define FILE_MAX (PIECES * TAPE_PIECE)
#define MAXF     200
#define MAXD     64
#define HEAD_OLD (INV_PATH + 16)         /* a header before packing came    */
#define HEAD_LEN (INV_PATH + 20)
#define SEAL_PRE 26                      /* a sealed header's open part: mark, cipher, place, save, rounds, salt */
#define HEAD_SEAL (SEAL_PRE + HEAD_LEN + 16)
#define SEAL_MARK 0xEC                   /* no name starts with this       */
#define KEY_ROUNDS 10000u                /* about two seconds on the laptop */
#define ARENA    (800 * 1024)            /* more than a floppy has free    */

enum { ST_NONE, ST_PART, ST_SAME, ST_DIFF, ST_BACK, ST_BAD };
enum { M_IDLE, M_PICK, M_ARMED, M_WRITE, M_WROTE, M_READ, M_RESULT };

typedef struct {
    char name[INV_PATH];                 /* in the inventory: its path     */
    int  sys;                            /* one of the system's three      */
    int  dir;                            /* which folder; -1 the system's, -2 only the tape has it */
    AppFile f;                           /* where a system one is          */
    int  here;                           /* it is on the floppy            */
    int  on;                             /* ticked                         */
    int  take;                           /* ticked to be restored from the tape */
    int  card;                           /* on the last tape written or read: for its sleeve */
    u32  card_size;
    u32  size, used;                     /* its length on the floppy; bytes in use, before the filling */
    u32  plen;                           /* packed length, or 0            */
    u32  slen;                           /* sealed length (what is sent, tag and all), or 0 */
    int  ord;                            /* its place in the save           */
    int  seal_alg;                       /* the cipher the tape names       */
    u32  tsize;                          /* its length as the tape has it   */
    u8   pad;                            /* the filling                    */
    u16  crc;                            /* of the part in use             */
    int  pieces, got, bad;               /* blocks: how many, in, spoiled  */
    int  state;
    u8   have[PIECES];
    u8  *disk;                           /* as it is on the floppy         */
    u8  *send;                           /* what goes to tape: that, or packed */
    u8  *tape;                           /* as the tape gave it            */
    u8  *pk;                             /* the packed pieces, as they come */
} File;

static const Api *A;
static File s_f[MAXF];
static int  s_nf;
static int  s_big;                       /* files too long for a tape, left out */
static u8   s_disk[ARENA], s_pack[ARENA], s_tape[2 * ARENA];
static u32  s_disk_n, s_pack_n, s_tape_n;
static char s_dirs[MAXD][INV_PATH];
static int  s_ndirs, s_dir_at;
static int  W, H;                        /* the screen, in characters      */

/* The settings. */
static int s_job;                        /* 0 Standard Backup, 1 Pick & Mix */
static int s_packing = 1, s_fast, s_verify, s_line = 1;

/* The card for a job: from here until card_done no key sounds either
 * (hush 3: music and keys), since a key's sound in the tape would spoil
 * it.  Between jobs only the music stays off (hush 1). */
static int card_take(int listen)
{
    A->hush(3);
    return listen ? A->audio_listen(TAPE_RATE, s_line) : A->audio_open(TAPE_RATE);
}
static void card_done(void) { A->audio_close(); A->hush(1); }
static int s_dest = -2;                  /* restore into: -2 a folder named by the date, -1 where each came from, or a folder */
static int s_auto = -1;                  /* that new folder, once this read has made it */
static int s_menu, s_pick;               /* a list is open (1 the jobs, 2 the ciphers), and the line it is on */
static int s_crypt, s_alg;               /* encrypt what goes to tape, and with which cipher */
static char s_pass[64];                  /* the passphrase, for as long as one write or read lasts */
static u8  s_key[32], s_salt[16];
static int s_have_key;
static int s_locked, s_wrong;            /* a sealed tape and no passphrase; or the wrong one */
static int s_cursor, s_top;
static const char *s_note = "";          /* a line about what happened last */
static u8  s_note_c;
static u32 s_bumps[8];                   /* when the last spoiled pieces came */
static int s_nbumps;
static u32 s_began;                      /* when this write or read started  */
static const char *s_shout = "";         /* said once, by a key: until something else happens */

/* What is said when a tape has been read: five for a tape that came whole,
 * five for one that did not.  (The font has no long dash or curled
 * apostrophe; a plain one stands in.) */
static const char *const s_good[5] = {
    "Irie! Signal-to-noise ratio crisp and righteous - every single bit lock into di oxide with zero parity error, seen!",
    "Big up! Checksum verify clean from di first header right down to di end-of-file; your archive solid like Zion rock, mon!",
    "Seen, bredren! Pure digital clarity pon dat ribbon - no dropouts, no bit-flips, di whole payload read back hundred percent true!",
    "Respect! Magnetic flux strong and steady all di way through; every packet intact and Babylon corruption can't touch dis!",
    "Maximum style! Zero framing errors and di sync pulses sharp like razor - backup complete and verified pure irie!",
};
static const char *const s_poor[5] = {
    "Bumboclaat! Too much tape hiss and oxide dropout - checksum shatter and half your data blocks corrupt beyond recovery, mon!",
    "Rahtid! Di input level clip and saturate di ribbon; square waves mash up into pure distortion, can't decode di payload!",
    "Bloodclaat, bit-error rate dreadfully high - signal drop below di noise floor and scatter bad sectors all through di archive!",
    "Wha' kind a weak transfer dis? Sync headers lost inna static and parity failing left and right; dis backup totally compromised, bredren!",
    "Easy nuh, dis ya write mash up! Heavy signal drop wipe out di whole bitstream - fix up yuh record gain and run di tape back before yuh lose di whole ting, bredren!",
};
static const char *s_outcome = "";
static u8 s_outcome_c;

static const char *const s_alg_name[3] = { "ChaCha20-Poly1305", "AES-256-GCM", "AES-128-GCM" };
static const char *const s_alg_what[3] = {
    "(256-bit key, quantum-safe; quick here)",
    "(256-bit key, quantum-safe; slow here)",
    "(128-bit key, not quantum-safe; slow)",
};
static const char *const s_job_name[2] = { "Standard Backup", "Pick & Mix" };
static const char *const s_job_what[2] = { "(everything of yours on this floppy)", "(only what you tick, after W)" };

/* ---- small things ----------------------------------------------------------- */
static int up(int c) { return c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c; }

static int same_word(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) if (up(*a) != up(*b)) return 0;
    return !*a && !*b;
}

static int len_of(const char *t) { int n = 0; while (t[n]) n++; return n; }

static int text(int x, int y, const char *t, u8 fg) { return A->win_text(x, y, t, fg, A_PAPER); }

static void bar(int x, int y, int w, int fill, u8 c)
{
    int i;
    for (i = 0; i < w; i++)
        A->win_put(x + i, y, i < fill ? A_CH_FILL : '.', i < fill ? c : A_OLIVE, A_PAPER, 0);
}

/* Down the serial cable, for whoever is testing: a word and two numbers. */
static void wire(const char *what, const char *name, u32 a, u32 b)
{
    char m[140];
    A->format(m, sizeof m, "[tape] %s %s %u %u\n", what, name, a, b);
    A->wire(m);
}

/* ---- the files ---------------------------------------------------------------- */
static File *add_file(const char *name, int sys, int dir)
{
    File *f;
    int k;
    if (s_nf >= MAXF) return 0;
    f = &s_f[s_nf++];
    for (k = 0; name[k] && k < INV_PATH - 1; k++) f->name[k] = (char)up(name[k]);
    f->name[k] = 0;
    f->sys = sys; f->dir = dir;
    f->on = 1; f->take = 0; f->card = 0;
    f->here = 0; f->state = ST_NONE; f->got = f->bad = 0; f->plen = 0; f->slen = 0; f->ord = 0;
    f->disk = f->send = f->tape = f->pk = 0;
    return f;
}

static void each(const InvItem *it, void *ctx)
{
    char path[INV_PATH];
    const char *dir = s_dirs[s_dir_at];
    File *f;
    (void)ctx;
    if (len_of(dir) + len_of(it->name) + 2 > INV_PATH) return;
    A->format(path, INV_PATH, "%s%s%s", dir, dir[0] ? "/" : "", it->name);
    if (it->folder) {
        if (s_ndirs < MAXD) A->format(s_dirs[s_ndirs++], INV_PATH, "%s", path);
        return;
    }
    if (it->size > FILE_MAX) { s_big++; return; }
    f = add_file(path, 0, s_dir_at);
    if (f) { f->here = 1; f->size = it->size; }
}

/* The files that are yours and on this floppy. */
static void find_files(void)
{
    static const char *const sys[3] = { "BOOKMARK.TXT", "JOURNAL.TXT", "STATE.BIN" };
    int i;
    s_nf = 0; s_big = 0;
    s_ndirs = 1; s_dirs[0][0] = 0;
    for (s_dir_at = 0; s_dir_at < s_ndirs; s_dir_at++) A->inv_list(s_dirs[s_dir_at], each, 0);
    for (i = 0; i < 3; i++) {
        AppFile af;
        File *f;
        if (!A->file_find(sys[i], &af) || af.size == 0 || af.size > FILE_MAX) continue;
        f = add_file(sys[i], 1, -1);
        if (f) { f->f = af; f->here = 1; f->size = af.size; }
    }
}

/* A file from the floppy, and how much of it need be sent: everything
 * before the run of one repeated byte it ends with; packed, if asked and
 * if that is shorter. */
static int from_floppy(File *f, int pack)
{
    u32 n = f->size;
    int got;
    if (s_disk_n + n > ARENA) return 0;
    f->disk = s_disk + s_disk_n;
    got = f->sys ? A->file_read(&f->f, f->disk, n) : A->inv_read(f->name, f->disk, n);
    if (got != (int)n) { f->disk = 0; return 0; }
    s_disk_n += n;
    f->pad = n ? f->disk[n - 1] : 0;
    while (n > 0 && f->disk[n - 1] == f->pad) n--;
    if (!f->sys && f->size - n < 64) n = f->size;        /* a short run is the file's own */
    f->used = n;
    f->crc = tape_crc(f->disk, (int)n);
    f->send = f->disk; f->plen = 0;
    if (pack && n > 16 && s_pack_n + n <= ARENA) {
        u32 p = lz_pack(f->disk, n, s_pack + s_pack_n, n - 1);
        if (p) { f->send = s_pack + s_pack_n; f->plen = p; s_pack_n += p; }
    }
    f->slen = 0;
    n = f->plen ? f->plen : f->used;
    f->pieces = (int)((n + TAPE_PIECE - 1) / TAPE_PIECE);
    return 1;
}

static void nonce_for(u8 n[12], int kind, int ord);

/* Seal what is to be sent of a file, in place of it.  0: no room. */
static int seal_file(File *f, int ord, u16 set)
{
    u32 n = f->plen ? f->plen : f->used;
    u8 nonce[12], aad[3], *out;
    if (s_pack_n + n + 16 > ARENA) return 0;
    out = s_pack + s_pack_n;
    nonce_for(nonce, 'D', ord);
    aad[0] = (u8)set; aad[1] = (u8)(set >> 8); aad[2] = (u8)ord;
    A->crypt_seal(s_alg, s_key, nonce, aad, 3, f->send, n, out, out + n);
    s_pack_n += n + 16;
    f->send = out; f->slen = n + 16;
    f->pieces = (int)((f->slen + TAPE_PIECE - 1) / TAPE_PIECE);
    return 1;
}

static u32 sent_len(const File *f) { return f->slen ? f->slen : f->plen ? f->plen : f->used; }

/* ---- sealing ---------------------------------------------------------------------
 * One key for one save: the passphrase and a salt drawn fresh each time,
 * through PBKDF2.  So within a save a nonce need only say what is being
 * sealed: a header or a file's bytes, and which file. */
static void nonce_for(u8 n[12], int kind, int ord)
{
    int i;
    for (i = 0; i < 12; i++) n[i] = 0;
    n[0] = (u8)kind; n[1] = (u8)ord;
}

static void forget_key(void)
{
    u32 i;
    for (i = 0; i < sizeof s_pass; i++) s_pass[i] = 0;
    for (i = 0; i < 32; i++) s_key[i] = 0;
    s_have_key = 0;
}

/* ---- the list on the screen: one folder at a time, as the inventory shows it ----
 * A row is a folder (>= 0: its number; -1 the system's records; -2 what
 * only the tape has) or a file (its number + 1000).  Enter goes into a
 * folder and Backspace comes back out. */
static int s_rows[MAXF + MAXD + 4], s_nrows;
static int s_view;                       /* the folder shown: its number, or -1 or -2 */
static int s_mode, s_verifying;

static int parent_of(int d)
{
    char up_[INV_PATH];
    int k, cut = -1, i;
    if (d <= 0) return 0;
    for (k = 0; s_dirs[d][k]; k++) { up_[k] = s_dirs[d][k]; if (up_[k] == '/') cut = k; }
    if (cut < 0) return 0;
    up_[cut] = 0;
    for (i = 1; i < s_ndirs; i++) if (same_word(s_dirs[i], up_)) return i;
    return 0;
}

/* What the list shows.  Nothing, until there is something to do.  While a
 * tape is read and after, what the cassette holds, to tick and restore.
 * Otherwise (choosing what to write, writing, and checking a tape just
 * written) the floppy's files. */
static int tape_list(void) { return (s_mode == M_READ || s_mode == M_RESULT) && !s_verifying; }
static int no_list(void) { return s_mode == M_IDLE; }

static void build_rows(void)
{
    int d, i, any;
    s_nrows = 0;
    if (no_list()) { s_cursor = 0; return; }
    if (tape_list()) {                           /* every file the tape named, whole or not */
        for (i = 0; i < s_nf; i++) if (s_f[i].state >= ST_PART) s_rows[s_nrows++] = 1000 + i;
        if (s_cursor >= s_nrows) s_cursor = s_nrows ? s_nrows - 1 : 0;
        return;
    }
    if (s_view >= 0)
        for (d = 1; d < s_ndirs; d++) if (parent_of(d) == s_view) s_rows[s_nrows++] = d;
    for (i = 0; i < s_nf; i++) if (s_f[i].dir == s_view && s_f[i].here) s_rows[s_nrows++] = 1000 + i;
    if (s_view == 0) {                           /* and the system's three records */
        any = 0;
        for (i = 0; i < s_nf; i++) any |= s_f[i].dir == -1;
        if (any) s_rows[s_nrows++] = -1;
    }
    if (s_cursor >= s_nrows) s_cursor = s_nrows ? s_nrows - 1 : 0;
}

/* Show another folder, with the cursor on a row of it (or the first). */
static void view(int d, int on_row)
{
    int i;
    s_view = d; s_cursor = 0; s_top = 0;
    build_rows();
    for (i = 0; i < s_nrows; i++) if (s_rows[i] == on_row) s_cursor = i;
}

/* Is this file inside that folder row? */
static int inside(const File *f, int d)
{
    const char *dir;
    int k;
    if (d < 0) return f->dir == d;
    if (f->dir < 0) return 0;
    dir = s_dirs[d];
    for (k = 0; dir[k]; k++) if (up(dir[k]) != f->name[k]) return 0;
    return f->name[k] == '/';
}

/* A folder's tick: 0 none of it, 1 all of it, 2 some. */
static int folder_tick(int d)
{
    int i, on = 0, n = 0;
    for (i = 0; i < s_nf; i++) if (inside(&s_f[i], d)) { n++; on += s_f[i].on; }
    return !on ? 0 : on == n ? 1 : 2;
}

static void tick_row(int row)
{
    int r = s_rows[row], i;
    if (tape_list()) {                           /* only what came whole can be restored */
        File *f = &s_f[r - 1000];
        if (s_mode == M_RESULT && (f->state == ST_SAME || f->state == ST_DIFF)) f->take ^= 1;
        return;
    }
    if (r >= 1000) s_f[r - 1000].on ^= 1;
    else {
        int to = folder_tick(r) != 1;
        for (i = 0; i < s_nf; i++) if (inside(&s_f[i], r)) s_f[i].on = to;
    }
    s_job = 1;                                   /* a tick by hand is Pick & Mix */
}

static void tick_all(void)
{
    int i;
    for (i = 0; i < s_nf; i++) s_f[i].on = 1;
}

/* ---- a cassette, in wire ------------------------------------------------------- */
#define CW 19
#define CH 8
static void box(int x, int y, int w, int h, u8 c)
{
    int i;
    for (i = 1; i < w - 1; i++) {
        A->win_put(x + i, y, A_FR_H, c, A_PAPER, 0);
        A->win_put(x + i, y + h - 1, A_FR_H, c, A_PAPER, 0);
    }
    for (i = 1; i < h - 1; i++) {
        A->win_put(x, y + i, A_FR_V, c, A_PAPER, 0);
        A->win_put(x + w - 1, y + i, A_FR_V, c, A_PAPER, 0);
    }
    A->win_put(x, y, A_FR_TL, c, A_PAPER, 0);          A->win_put(x + w - 1, y, A_FR_TR, c, A_PAPER, 0);
    A->win_put(x, y + h - 1, A_FR_BL, c, A_PAPER, 0);  A->win_put(x + w - 1, y + h - 1, A_FR_BR, c, A_PAPER, 0);
}

static void cassette(int x, int y, int turning)
{
    static const char spoke[4] = { '|', '/', '-', '\\' };
    int i, s = turning ? (int)(A->ms() / 140) & 3 : 0;
    u8 c = turning ? A_GOLD : A_OLIVE;
    box(x, y, CW, CH, c);
    box(x + 3, y + 2, CW - 6, 3, c);             /* the window with the two wheels */
    for (i = 0; i < 2; i++) {
        int wx = x + 5 + i * 6;
        A->win_put(wx, y + 3, '(', c, A_PAPER, 0);
        A->win_put(wx + 1, y + 3, spoke[s], turning ? A_ON : A_OLIVE, A_PAPER, 0);
        A->win_put(wx + 2, y + 3, ')', c, A_PAPER, 0);
    }
    /* the opening at the bottom, where the heads go */
    A->win_put(x + 4, y + 6, '/', c, A_PAPER, 0);
    A->win_put(x + CW - 5, y + 6, '\\', c, A_PAPER, 0);
    for (i = 5; i < CW - 5; i++) A->win_put(x + i, y + 6, A_FR_H, c, A_PAPER, 0);
    A->win_put(x + 7, y + 6, 'o', c, A_PAPER, 0);
    A->win_put(x + CW - 8, y + 6, 'o', c, A_PAPER, 0);
}

/* ---- the trudelutt ------------------------------------------------------------- */
static void tune(int sad)
{
    /* pitch in Hz and length in ms; a pitch of 0 rests; a negative one slides up to the next */
    static const short merry[][2] = {
        { 523, 70 }, { 659, 70 }, { 784, 70 }, { 1047, 70 }, { 784, 70 }, { 1047, 140 }, { 0, 60 },
        { -300, 260 }, { 1800, 40 }, { 0, 70 },
        { 988, 60 }, { 0, 40 }, { 988, 60 }, { 0, 40 }, { 1319, 110 }, { 0, 50 },
        { 196, 90 }, { 185, 90 }, { 131, 260 }, { 0, 80 }, { 2093, 50 }, { 0, 30 }, { 2637, 120 },
    };
    /* and for a tape that came badly: down the stairs, and a sigh */
    static const short low[][2] = {
        { 392, 150 }, { 0, 30 }, { 370, 150 }, { 0, 30 }, { 349, 150 }, { 0, 30 }, { 330, 320 }, { 0, 80 },
        { -220, 420 }, { 98, 60 }, { 0, 60 },
    };
    static u8 buf[512];
    const short (*notes)[2] = sad ? low : merry;
    u32 phase = 0;
    int i, n, k, count = sad ? (int)(sizeof low / sizeof low[0]) : (int)(sizeof merry / sizeof merry[0]);
    int rate = card_take(0);
    if (!rate) return;
    for (i = 0; i < count; i++) {
        int hz = notes[i][0], total = rate / 1000 * notes[i][1] + rate % 1000 * notes[i][1] / 1000, at = 0;
        int to = hz < 0 ? notes[i + 1][0] : 0;
        if (hz < 0) hz = -hz;
        while (at < total) {
            n = total - at > (int)sizeof buf ? (int)sizeof buf : total - at;
            for (k = 0; k < n; k++) {
                int f = to ? hz + (to - hz) * (at + k) / total : hz;
                int wob = f && f < 250 ? ((at + k) / 90 & 1 ? 6 : -6) : 0;   /* the low notes wobble */
                int amp = 34 - 20 * (at + k) / total;                        /* each note dies away */
                phase += (u32)(f + wob) * 131072u / (u32)rate;
                buf[k] = (u8)(128 + (f ? (phase & 0x10000 ? amp : -amp) : 0));
            }
            k = 0;
            while (k < n) { k += A->audio_write(buf + k, n - k); if (k < n) A->pump(0); }
            at += n;
        }
    }
    while (A->audio_queued() > 0) A->pump(0);
    {   /* the card's own loop still holds the last notes: let them out */
        u32 t0 = A->ms();
        while (A->ms() - t0 < 1300) A->pump(0);
    }
    card_done();
}

/* ---- writing ------------------------------------------------------------------ */
static int s_at, s_stage, s_ord, s_count;    /* the file, where in it, which of how many */
static u16 s_save_id;
static TapeTx s_tx;
static u32 s_samples, s_sent, s_bytes, s_ended;
static int s_last;

/* How long a block lasts on the tape, in samples. */
static u32 block_samples(int len, int lead_ms)
{
    u32 bits = ((u32)lead_ms * 1200 / 1000 << s_fast) + 8 + (u32)(9 + len) * 10;
    return (bits * (TAPE_RATE / 150) / 8) >> s_fast;
}

/* Queue the next block of the save, or say there is none. */
static int next_block(TapeTx *t)
{
    u8 h[HEAD_LEN];
    File *f;
    int k;

    while (s_at < s_nf && !s_f[s_at].send) s_at++;
    if (s_at >= s_nf) return 0;
    f = &s_f[s_at];
    if (s_stage < 2) {                           /* its name and size, twice */
        for (k = 0; k < INV_PATH; k++) h[k] = 0;
        for (k = 0; f->name[k] && k < INV_PATH - 1; k++) h[k] = (u8)f->name[k];
        k = INV_PATH;
        h[k] = (u8)f->size; h[k + 1] = (u8)(f->size >> 8); h[k + 2] = (u8)(f->size >> 16); h[k + 3] = (u8)(f->size >> 24);
        h[k + 4] = (u8)f->used; h[k + 5] = (u8)(f->used >> 8); h[k + 6] = (u8)(f->used >> 16); h[k + 7] = (u8)(f->used >> 24);
        h[k + 8] = f->pad; h[k + 9] = (u8)s_ord; h[k + 10] = (u8)s_count;
        h[k + 11] = (u8)f->crc; h[k + 12] = (u8)(f->crc >> 8);
        h[k + 13] = (u8)s_save_id; h[k + 14] = (u8)(s_save_id >> 8);
        h[k + 15] = (u8)f->sys;
        h[k + 16] = (u8)f->plen; h[k + 17] = (u8)(f->plen >> 8); h[k + 18] = (u8)(f->plen >> 16); h[k + 19] = (u8)(f->plen >> 24);
        if (s_crypt) {                           /* the name and the rest, sealed */
            u8 o[HEAD_SEAL], nonce[12];
            int j;
            o[0] = SEAL_MARK; o[1] = (u8)s_alg; o[2] = (u8)s_ord; o[3] = (u8)s_count;
            o[4] = (u8)s_save_id; o[5] = (u8)(s_save_id >> 8);
            o[6] = (u8)KEY_ROUNDS; o[7] = (u8)(KEY_ROUNDS >> 8); o[8] = (u8)(KEY_ROUNDS >> 16); o[9] = (u8)(KEY_ROUNDS >> 24);
            for (j = 0; j < 16; j++) o[10 + j] = s_salt[j];
            nonce_for(nonce, 'H', s_ord);
            A->crypt_seal(s_alg, s_key, nonce, o, SEAL_PRE, h, HEAD_LEN, o + SEAL_PRE, o + SEAL_PRE + HEAD_LEN);
            tape_tx_block(t, TAPE_HEAD, s_ord << 8, o, HEAD_SEAL, s_stage ? 100 : (s_ord ? 1000 : 8000));
        } else
        tape_tx_block(t, TAPE_HEAD, s_ord << 8, h, HEAD_LEN, s_stage ? 100 : (s_ord ? 1000 : 8000));
        s_stage++;
        f->got = 0;
    } else if (f->got < f->pieces) {             /* the part in use        */
        u32 off = (u32)f->got * TAPE_PIECE, n = sent_len(f);
        int len = n - off > TAPE_PIECE ? TAPE_PIECE : (int)(n - off);
        tape_tx_block(t, TAPE_DATA, (s_ord << 8) | f->got, f->send + off, len, 100);
        f->got++;
    } else {                                     /* and its end, twice     */
        tape_tx_block(t, TAPE_END, (s_ord << 8) | f->pieces, h, 0, 100);
        if (++s_stage == 4) { s_stage = 0; s_ord++; s_at++; }
    }
    return 1;
}

static void say(const char *t, u8 c) { s_note = t; s_note_c = c; }

/* Read the ticked files and work out the tape they need.  0: nothing to write. */
static int write_ready(void)
{
    int i, n;
    s_disk_n = s_pack_n = 0;
    s_count = 0; s_samples = 0; s_bytes = 0;
    for (i = 0; i < s_nf; i++) {
        File *f = &s_f[i];
        f->disk = f->send = 0; f->state = ST_NONE; f->got = 0;
        if (!f->on || !f->here || !from_floppy(f, s_packing)) continue;
        if (s_crypt && !seal_file(f, s_count, s_save_id)) { f->send = 0; continue; }
        s_samples += block_samples(s_crypt ? HEAD_SEAL : HEAD_LEN, s_count ? 1000 : 8000) + block_samples(s_crypt ? HEAD_SEAL : HEAD_LEN, 100) + 2 * block_samples(0, 100);
        for (n = 0; n < f->pieces; n++) {
            u32 left = sent_len(f) - (u32)n * TAPE_PIECE;
            s_samples += block_samples(left > TAPE_PIECE ? TAPE_PIECE : (int)left, 100);
        }
        s_bytes += f->used;
        s_count++;
    }
    return s_count;
}

static int ask_line(const char *prompt, char *buf, int cap, int hidden);
static void busy_note(const char *t);
static void show_now(void);
static void sleeve_where(char *m, u32 cap);

static void write_arm(void)
{
    s_save_id = (u16)(A->ms() * 2654435761u >> 16);
    if (s_crypt) {
        char again[64];
        int i, same;
        forget_key();
        if (!ask_line("Passphrase for this tape:", s_pass, (int)sizeof s_pass, 1) || !s_pass[0]) { forget_key(); return; }
        if (!ask_line("The same again, to be sure:", again, (int)sizeof again, 1)) { forget_key(); return; }
        for (i = 0, same = 1; i < (int)sizeof again; i++) { same &= again[i] == s_pass[i]; if (!again[i]) break; }
        for (i = 0; i < (int)sizeof again; i++) again[i] = 0;
        if (!same) { forget_key(); say("The two were not the same. Nothing done.", A_RED); return; }
        busy_note("Making the key and sealing the files...");
        A->random(s_salt, 16);
        A->crypt_key(s_pass, s_salt, 16, KEY_ROUNDS, s_key);
        s_have_key = 1;
    }
    if (!write_ready()) { forget_key(); say("Nothing ticked that the floppy can give.", A_RED); return; }
    s_mode = M_ARMED;
    say("", 0);
}

static void write_go(void)
{
    if (!card_take(0)) { s_mode = M_IDLE; say("No sound card that plays answers.", A_RED); return; }
    s_at = s_stage = s_ord = 0;
    tape_tx_begin(&s_tx, s_fast);
    s_sent = 0; s_last = 0;
    s_began = A->ms(); s_shout = "";
    s_mode = M_WRITE;
    wire("saving", "files", (u32)s_count, s_samples / TAPE_RATE + 1);
}

static void read_go(int verifying);
static void done_written(void);

/* While writing: keep a second of sound in hand, and notice the end. */
static void write_step(void)
{
    static u8 buf[1024];
    u32 now = A->ms();
    int n;
    while (A->audio_queued() < TAPE_RATE) {
        if (!tape_tx_busy(&s_tx) && !next_block(&s_tx)) { if (!s_last) s_last = 1; break; }
        n = tape_tx_samples(&s_tx, buf, (int)sizeof buf);
        s_sent += (u32)A->audio_write(buf, n);
    }
    if (s_last == 1 && A->audio_queued() == 0) { s_last = 2; s_ended = now; }
    else if (s_last == 2 && now - s_ended > 1200) {      /* the card's own loop has emptied too */
        card_done();
        wire("saved", "files", (u32)s_count, s_bytes);
        s_mode = M_WROTE;
        if (s_verify) read_go(1);                /* not done yet: the tune waits for the check */
        else { done_written(); forget_key(); say("All on the tape. Stop the recorder.", A_ON); show_now(); tune(0); }
    }
}

/* ---- the last tape, for its sleeve ---------------------------------------------- */
static int s_done;                       /* 0 none yet, 1 written, 2 read, 3 written and checked */
static u8  s_done_when[6];
static int s_done_clock, s_done_fast, s_done_packed, s_done_alg;   /* alg: -1 not sealed */
static u32 s_done_files, s_done_bytes;
static const char *s_done_verdict = "";

static void done_written(void)
{
    int i;
    s_done = 1; s_done_clock = A->clock(s_done_when);
    s_done_fast = s_fast; s_done_packed = s_packing; s_done_alg = s_crypt ? s_alg : -1;
    s_done_files = (u32)s_count; s_done_bytes = s_bytes; s_done_verdict = "";
    for (i = 0; i < s_nf; i++) { s_f[i].card = s_f[i].send != 0; s_f[i].card_size = s_f[i].size; }
}

/* ---- reading ------------------------------------------------------------------ */
static TapeRx s_rx;
static void bump_at(u32 now);
static int s_cur, s_cur_ord;             /* the file the tape is in, or -1  */
static int s_set, s_set_n;               /* which save this is; how many files it has */
static int s_seen[256];                  /* by place in the save: 0 not heard, 1 + the file, or -1 not wanted */
static int s_others;                     /* another save was heard too      */
static u32 s_quiet_since, s_bad_was;
static const char *s_why = "";

/* Every piece of a file has come: is it right, and is it what the floppy has? */
static void settle(File *f)
{
    u32 i;
    int sound = 1;
    u32 n = f->plen ? f->plen : f->used;
    u8 *got = f->pk;                             /* what the pieces made    */
    if (f->slen) {                               /* sealed: open it first   */
        u8 nonce[12], aad[3], *out = f->plen ? f->pk + f->slen : f->tape;
        nonce_for(nonce, 'D', f->ord);
        aad[0] = (u8)s_set; aad[1] = (u8)(s_set >> 8); aad[2] = (u8)f->ord;
        sound = A->crypt_open(f->seal_alg, s_key, nonce, aad, 3, f->pk, n, f->pk + n, out);
        got = out;
    }
    if (sound && f->plen) sound = lz_unpack(got, f->plen, f->tape, f->used) == f->used;
    if (!sound || tape_crc(f->tape, (int)f->used) != f->crc) {     /* pieces of two saves, mixed */
        f->got = 0;
        for (i = 0; i < PIECES; i++) f->have[i] = 0;
        f->state = ST_PART;
        return;
    }
    for (i = f->used; i < f->tsize; i++) f->tape[i] = f->pad;
    f->state = ST_DIFF;
    if (f->here && f->disk && f->tsize == f->size) {
        for (i = 0; i < f->size && f->tape[i] == f->disk[i]; i++) { }
        if (i == f->size) f->state = ST_SAME;
    }
    wire(f->state == ST_SAME ? "same" : "different", f->name, f->used, f->plen);
}

static void heard(void *ctx, int type, int seq, const u8 *d, int len)
{
    File *f;
    (void)ctx;
    if (type == TAPE_HEAD) {
        u32 size, used, plen = 0, n;
        int i, k, ord, count, set, sealed = 0, alg = 0;
        u16 crc;
        static u8 plain[HEAD_LEN];
        if (len == HEAD_SEAL && d[0] == SEAL_MARK) {     /* a sealed header: open it, or stop */
            u8 nonce[12];
            u32 rounds = (u32)d[6] | (u32)d[7] << 8 | (u32)d[8] << 16 | (u32)d[9] << 24;
            if (s_wrong || s_locked) return;
            if (!s_pass[0]) { s_locked = 1; return; }
            if (d[1] > 2 || rounds < 1 || rounds > 200000) return;
            for (i = 0; i < 16 && s_have_key && s_salt[i] == d[10 + i]; i++) { }
            if (i < 16) {                        /* this save's key: about two seconds */
                for (i = 0; i < 16; i++) s_salt[i] = d[10 + i];
                A->crypt_key(s_pass, s_salt, 16, rounds, s_key);
                s_have_key = 1;
            }
            nonce_for(nonce, 'H', d[2]);
            if (!A->crypt_open(d[1], s_key, nonce, d, SEAL_PRE, d + SEAL_PRE, HEAD_LEN, d + SEAL_PRE + HEAD_LEN, plain)) { s_wrong = 1; return; }
            sealed = 1; alg = d[1];
            d = plain; len = HEAD_LEN;
        }
        if ((len != HEAD_LEN && len != HEAD_OLD) || !d[0] || d[INV_PATH - 1]) return;
        k = INV_PATH;
        size = (u32)d[k] | (u32)d[k + 1] << 8 | (u32)d[k + 2] << 16 | (u32)d[k + 3] << 24;
        used = (u32)d[k + 4] | (u32)d[k + 5] << 8 | (u32)d[k + 6] << 16 | (u32)d[k + 7] << 24;
        ord = d[k + 9]; count = d[k + 10];
        crc = (u16)(d[k + 11] | d[k + 12] << 8);
        set = d[k + 13] | d[k + 14] << 8;
        if (len == HEAD_LEN) plen = (u32)d[k + 16] | (u32)d[k + 17] << 8 | (u32)d[k + 18] << 16 | (u32)d[k + 19] << 24;
        if (!count || ord >= count || (seq >> 8) != ord) return;
        if (s_set < 0) { s_set = set; s_set_n = count; }
        /* Another save on the same tape (an older one, say, after the end of
         * this): its pieces carry no save number, only its headers do, so
         * nothing is taken until a header of this save comes again. */
        if (set != s_set || count != s_set_n) { s_others = 1; s_cur = -1; return; }
        for (i = 0; i < s_nf; i++)
            if (s_f[i].sys == (d[k + 15] != 0) && same_word(s_f[i].name, (const char *)d)) break;
        if (size > FILE_MAX || used > size || plen >= used + 1) { s_seen[ord] = -1; return; }
        if (i == s_nf) {
            /* Not on this floppy.  One of the system's three would have
             * been; anything else is a file for the inventory, new here. */
            if (d[k + 15] || !add_file((const char *)d, 0, -2)) { s_seen[ord] = -1; return; }
            s_f[i].on = 1;
            build_rows();
        }
        f = &s_f[i];
        /* One of the system's at a size this floppy does not keep it at. */
        if (f->sys && size != f->size) { s_seen[ord] = -1; return; }
        n = plen ? plen : used;
        if (f->state == ST_NONE || f->used != used || f->crc != crc || f->pad != d[k + 8] || f->tsize != size || f->plen != plen ||
            (f->slen != 0) != sealed) {
            /* room for the file, the sealed pieces, and what opening them gives */
            if (s_tape_n + size + 2 * n + 16 > sizeof s_tape) { s_seen[ord] = -1; return; }
            f->tape = s_tape + s_tape_n;
            f->pk = f->tape + size;
            s_tape_n += size + 2 * n + 16;
            f->tsize = size; f->used = used; f->crc = crc; f->pad = d[k + 8]; f->plen = plen;
            f->slen = sealed ? n + 16 : 0; f->ord = ord; f->seal_alg = alg;
            n = sent_len(f);
            f->pieces = (int)((n + TAPE_PIECE - 1) / TAPE_PIECE);
            f->got = f->bad = 0;
            for (k = 0; k < PIECES; k++) f->have[k] = 0;
            f->state = ST_PART;
            if (!f->pieces) settle(f);
        }
        s_seen[ord] = 1 + i;
        s_cur = i; s_cur_ord = ord;
    } else if (type == TAPE_DATA) {
        int p = seq & 0xFF, want, k;
        u32 n;
        u8 *to;
        if (s_cur < 0 || (seq >> 8) != s_cur_ord) return;
        f = &s_f[s_cur];
        if (f->state != ST_PART || p >= f->pieces || f->have[p]) return;
        n = sent_len(f);
        want = p == f->pieces - 1 ? (int)(n - (u32)p * TAPE_PIECE) : TAPE_PIECE;
        if (len != want) return;
        to = f->plen || f->slen ? f->pk : f->tape;
        for (k = 0; k < len; k++) to[p * TAPE_PIECE + k] = d[k];
        f->have[p] = 1;
        if (++f->got == f->pieces) settle(f);
    }
}

/* Has every file of the save come whole? */
static int all_in(void)
{
    int i;
    if (s_set < 0) return 0;
    for (i = 0; i < s_set_n; i++) {
        if (!s_seen[i]) return 0;
        if (s_seen[i] > 0 && s_f[s_seen[i] - 1].state < ST_SAME) return 0;
    }
    return 1;
}

/* How far through the tape's save, in hundredths. */
static int read_percent(void)
{
    int i, sum = 0;
    if (s_set < 0 || !s_set_n) return 0;
    for (i = 0; i < s_set_n; i++) {
        File *f;
        if (!s_seen[i]) continue;
        if (s_seen[i] < 0) { sum += 100; continue; }
        f = &s_f[s_seen[i] - 1];
        sum += f->state >= ST_SAME || !f->pieces ? 100 : f->got * 100 / f->pieces;
    }
    return sum / s_set_n;
}

static void read_go(int verifying)
{
    int i;
    s_locked = s_wrong = 0;
    if (!verifying) {
        forget_key();
        if (s_crypt && (!ask_line("Passphrase for this tape:", s_pass, (int)sizeof s_pass, 1) || !s_pass[0])) { forget_key(); return; }
    }
    s_disk_n = s_pack_n = s_tape_n = 0;
    for (i = 0; i < s_nf; i++) {
        File *f = &s_f[i];
        f->state = ST_NONE; f->disk = f->send = 0; f->got = f->bad = 0;
        if (f->here) from_floppy(f, 0);
    }
    s_set = -1; s_others = 0;
    for (i = 0; i < 256; i++) s_seen[i] = 0;
    if (!card_take(1)) { s_mode = M_IDLE; say("No sound card that can listen answers.", A_RED); return; }
    tape_rx_begin(&s_rx, heard, 0);
    s_cur = -1; s_bad_was = 0;
    s_quiet_since = A->ms();
    s_verifying = verifying;
    s_began = A->ms(); s_nbumps = 0; s_shout = "";
    s_auto = -1;                                 /* what this read restores gets a folder of its own */
    s_mode = M_READ;
    s_cursor = s_top = 0;
    build_rows();
    say("", 0);
}

static void read_end(int stopped)
{
    static char sum[64];
    int i, same = 0, diff = 0, part = 0, whole, song = -1;
    card_done();
    forget_key();
    if (s_locked || s_wrong) {                   /* nothing about the tape's quality */
        s_mode = M_RESULT;
        s_outcome = ""; s_shout = "";
        build_rows();
        say(s_locked ? "This tape is encrypted. Press E, then R." : "Wrong passphrase, or the tape was altered.", A_RED);
        wire("read", s_locked ? "locked" : "refused", 0, 0);
        return;
    }
    for (i = 0; i < s_nf; i++) {
        same += s_f[i].state == ST_SAME;
        diff += s_f[i].state == ST_DIFF;
        part += s_f[i].state == ST_PART;
    }
    /* Whole means every file the save has, not merely none of them broken
     * off: a file whose name was never heard is missing too. */
    whole = all_in();
    s_mode = M_RESULT;
    /* What starts ticked.  Into a folder: everything that came whole, so
     * the folder is a copy of the tape (the system's three records there
     * are ordinary files, and the system's own are not touched).  Back to
     * where each came from: only what the floppy lacks. */
    for (i = 0; i < s_nf; i++) {
        File *f = &s_f[i];
        int ok = f->state == ST_SAME || f->state == ST_DIFF;
        f->take = !s_verifying && ok && (s_dest == -1 ? f->state == ST_DIFF && !f->here : 1);
    }
    view(0, 0);                                  /* the whole of what was read */
    wire("read", "same/different", (u32)same, (u32)diff);
    {   /* the summing-up: one of five, by how the tape came.  Stopping it by
         * hand says nothing about the tape, unless pieces were spoiled. */
        int pick = (int)((A->ms() >> 3) % 5);
        s_outcome = whole ? s_good[pick] : (stopped && !s_rx.bad) ? "" : s_poor[pick];
        s_outcome_c = whole ? A_ON : A_RED;
        s_shout = "";
    }
    if (same || diff) {                          /* what this tape was, for its sleeve */
        s_done = s_verifying ? 3 : 2; s_done_clock = A->clock(s_done_when);
        s_done_fast = s_verifying ? s_fast : s_rx.fast; s_done_packed = 0; s_done_alg = -1;
        s_done_files = (u32)(same + diff); s_done_bytes = 0; s_done_verdict = s_outcome;
        for (i = 0; i < s_nf; i++) {
            File *f = &s_f[i];
            f->card = f->state >= ST_SAME; f->card_size = f->tsize;
            if (!f->card) continue;
            s_done_bytes += f->used;
            if (f->plen) s_done_packed = 1;
            if (f->slen) s_done_alg = f->seal_alg;
        }
    }
    if (stopped && !same && !diff) say(part ? "Stopped part way. Nothing was judged." : "Stopped before anything was read.", A_CREAM);
    else if (!same && !diff && !part) { say("Nothing of yours was heard.", A_RED); if (!stopped) song = 1; }
    else if (!whole) {
        A->format(sum, sizeof sum, "%u of %u came whole. Rewind, play, \1R\1.", (u32)(same + diff), (u32)(s_set_n > 0 ? s_set_n : same + diff + part));
        say(sum, A_RED);
        if (!stopped || s_rx.bad) song = 1;
    }
    else if (s_verifying) {
        say(diff ? "The tape differs from the floppy." : "Checked: the tape matches the floppy.", diff ? A_RED : A_ON);
        song = diff != 0;
    } else {
        int fresh = 0;
        for (i = 0; i < s_nf; i++) fresh += s_f[i].state == ST_DIFF && !s_f[i].here;
        A->format(sum, sizeof sum, "Read %u: %u new, %u different, %u same.", (u32)(same + diff), (u32)fresh, (u32)(diff - fresh), (u32)same);
        say(sum, A_ON);
        song = 0;
    }
    if (song >= 0) { show_now(); tune(song); }   /* the screen says how it went before the tune does */
}

static void read_step(void)
{
    static u8 buf[2048];
    static u32 told;
    u32 now = A->ms();
    int n;
    while ((n = A->audio_heard(buf, (int)sizeof buf)) > 0) tape_rx_samples(&s_rx, buf, n);
    if (s_rx.bad != s_bad_was) {                 /* a spoiled block: against the file in hand */
        if (s_cur >= 0) s_f[s_cur].bad += (int)(s_rx.bad - s_bad_was);
        while (s_bad_was != s_rx.bad) { s_bad_was++; bump_at(now); }
    }
    if (now - told >= 5000) {
        told = now;
        wire(s_rx.tone ? "tones" : "listening", "level", (u32)s_rx.loud, s_rx.fast);
        wire("pieces", "good/spoiled", s_rx.good, s_rx.bad);
    }
    if (s_rx.loud >= 4) s_quiet_since = now;
    if (s_locked || s_wrong) read_end(0);
    else if (all_in()) read_end(0);
    else if (s_set >= 0 && now - s_quiet_since > 10000) read_end(0);   /* ten seconds of nothing: the tape is over */
}

/* ---- putting files on the floppy --------------------------------------------------- */
/* A folder's place among the folders, added (with those above it) if it
 * is new to the list.  -1: no room for it. */
static int dir_for(const char *dir)
{
    char above[INV_PATH];
    int i, k, cut = -1;
    if (!dir[0]) return 0;
    for (i = 1; i < s_ndirs; i++) if (same_word(s_dirs[i], dir)) return i;
    for (k = 0; dir[k] && k < INV_PATH - 1; k++) { above[k] = dir[k]; if (dir[k] == '/') cut = k; }
    if (cut >= 0) { above[cut] = 0; if (dir_for(above) < 0) return -1; }
    if (s_ndirs >= MAXD) return -1;
    for (k = 0; dir[k] && k < INV_PATH - 1; k++) s_dirs[s_ndirs][k] = (char)up(dir[k]);
    s_dirs[s_ndirs][k] = 0;
    return s_ndirs++;
}

/* A file has just been put on the floppy: the floppy's list has it too. */
static void now_here(const char *path, u32 size)
{
    char dir[INV_PATH];
    int i, k, cut = -1, d;
    File *f;
    for (k = 0; path[k] && k < INV_PATH - 1; k++) { dir[k] = path[k]; if (path[k] == '/') cut = k; }
    dir[cut < 0 ? 0 : cut] = 0;
    d = dir_for(dir);
    for (i = 0; i < s_nf; i++)
        if (!s_f[i].sys && same_word(s_f[i].name, path)) {
            s_f[i].here = 1; s_f[i].size = size;
            if (s_f[i].dir < 0 && d >= 0) s_f[i].dir = d;
            return;
        }
    if (d < 0 || size > FILE_MAX) return;
    f = add_file(path, 0, d);
    if (f) { f->here = 1; f->size = size; f->on = !s_job; }
}

/* The folder a read tape is restored into, named by the day the reading
 * ended: DDMMYYYY, eight digits, which a FAT12 name just holds.  14112026
 * is the 14th of November 2026.  A second read on the same day goes into
 * the same folder.  With no clock: TAPE01, TAPE02... */
static int auto_name(char *m, u32 cap)
{
    const u8 *w = s_done_when;
    if (!s_done_clock) return 0;
    A->format(m, cap, "%02u%02u20%02u", (u32)w[2], (u32)w[1], (u32)w[0]);
    return 1;
}

static int ensure_auto(void)
{
    char name[24];
    InvItem it;
    u32 n;
    if (s_auto >= 0) return 1;
    if (!auto_name(name, sizeof name))
        for (n = 1; n < 100; n++) { A->format(name, sizeof name, "TAPE%02u", n); if (!A->inv_stat(name, &it)) break; }
    if (!A->inv_stat(name, &it) && !A->inv_mkdir(name)) return 0;
    s_auto = dir_for(name);
    wire("made", name, 0, 0);
    return s_auto >= 0;
}

/* Put the ticked files on the floppy.  Into a folder (the read's own, or
 * one that was chosen) everything ticked is written, as ordinary files,
 * and nothing that was there is touched.  Back to where each came from,
 * only what the floppy has not, or has differently; there one of the
 * system's records goes over the system's own. */
static void put_back(void)
{
    static const char sig[] = "FLOPOS journal";
    static char why[80], done[80];
    int i, k, back = 0, failed = 0;
    for (i = 0; i < s_nf && !failed; i++) {
        File *f = &s_f[i];
        char path[INV_PATH], dir[INV_PATH];
        InvItem it;
        int bad = 0, cut = -1;
        if (!f->take || (f->state != ST_DIFF && f->state != ST_SAME)) continue;
        if (s_dest == -1) {
            if (f->state == ST_SAME) { f->take = 0; continue; }       /* it is there already */
            if (f->sys) {
                /* The journal has to be a journal: FLOPOS writes to wherever it is. */
                if (same_word(f->name, "JOURNAL.TXT")) {
                    for (k = 0; sig[k] && f->tape[k] == (u8)sig[k]; k++) { }
                    if (sig[k]) { f->state = ST_BAD; f->take = 0; continue; }
                }
                if (!A->file_write(&f->f, f->tape)) { failed = 1; s_why = "The floppy would not take it."; break; }
                f->state = ST_BACK; f->take = 0; back++;
                wire("put back", f->name, f->used, 0);
                continue;
            }
            A->format(path, INV_PATH, "%s", f->name);
        } else {
            /* The read's own folder keeps the files' folders under it; a
             * chosen folder gets the files themselves. */
            const char *base, *rel = f->name;
            if (s_dest == -2) {
                if (!ensure_auto()) { failed = 1; A->format(why, sizeof why, "%s", A->inv_why()); s_why = why; break; }
                base = s_dirs[s_auto];
            } else {
                base = s_dirs[s_dest];
                for (k = 0; f->name[k]; k++) if (f->name[k] == '/') cut = k;
                rel = f->name + cut + 1;
            }
            if (len_of(base) + len_of(rel) + 2 > INV_PATH) { f->state = ST_BAD; f->take = 0; continue; }
            A->format(path, INV_PATH, "%s%s%s", base, base[0] ? "/" : "", rel);
        }
        for (k = 0; path[k] && !bad; k++) {      /* its folders first, from the top down */
            dir[k] = path[k];
            if (path[k + 1] != '/') continue;
            dir[k + 1] = 0;
            if (!A->inv_stat(dir, &it) && !A->inv_mkdir(dir)) bad = 1;
        }
        if (bad || !A->inv_write(path, f->tape, f->tsize)) {
            failed = 1;
            A->format(why, sizeof why, "%s", A->inv_why());
            s_why = why;
            break;
        }
        now_here(path, f->tsize);
        f->state = ST_BACK; f->take = 0;
        back++;
        wire("put back", f->name, f->used, 0);
    }
    if (failed) say(s_why, A_RED);
    else if (!back) say("Nothing ticked that needs restoring.", A_CREAM);
    else {
        if (s_dest == -1) A->format(done, sizeof done, "%s", "Restored to where each came from.");
        else if (s_dest == 0) A->format(done, sizeof done, "%s", "Restored to the top of the inventory.");
        else A->format(done, sizeof done, "Restored into /%s/.", s_dirs[s_dest == -2 ? s_auto : s_dest]);
        say(done, A_ON);
    }
}

/* ---- how it is going, in a person's words ------------------------------------------
 * One line at the bottom, from what the tape is doing right now.  Writing
 * cannot be heard, so there it goes by how far along it is; reading goes
 * by the spoiled pieces of the last ten seconds. */

static void bump(u32 now);
static void bump_at(u32 now) { bump(now); }
static void bump(u32 now)
{
    int i;
    for (i = 7; i > 0; i--) s_bumps[i] = s_bumps[i - 1];
    s_bumps[0] = now ? now : 1;
    if (s_nbumps < 8) s_nbumps++;
}

static const char *mood_line(u8 *c)
{
    u32 now = A->ms();
    int i, recent = 0, calm = (now / 8000) & 1;
    *c = A_GOLD;
    if (s_mode == M_ARMED) return "Buckle up!";
    if (s_mode == M_WRITE) {
        u32 heard_n = s_sent - (u32)A->audio_queued();
        u32 pc = s_samples ? heard_n / (s_samples / 100 + 1) : 0;
        if (now - s_began < 8000) return "Buckle up!";
        if (pc >= 55 && pc < 85) { *c = A_CREAM; return "Are we there yet?"; }
        return calm ? "Smooth sailing." : "Yeaah. Smoooth.";
    }
    if (s_mode == M_READ) {
        for (i = 0; i < s_nbumps; i++) recent += now - s_bumps[i] < 10000;
        if (s_set < 0 && !s_rx.good) {           /* nothing of a save heard yet */
            if (now - s_began > 20000) { *c = A_CREAM; return "Are we there yet?"; }
            return "Now hear 'dis!";             /* playing back; "Buckle up!" is for recording */
        }
        if (recent >= 6) { *c = A_RED; return "Sorry! Sorry!"; }
        /* the tape running well off its speed */
        if (s_rx.skew <= -7) { *c = A_CREAM; return "Easy now! Dat rust sound like di treble gone pon vacation!"; }
        if (s_rx.skew >= 7) { *c = A_CREAM; return "Wake up, selecta! Di tape a drag like Monday morning!"; }
        if (recent >= 4) { *c = A_RED; return "Bomboclat! Das rough!"; }
        if (recent >= 2) { *c = A_CREAM; return "Just a bit of turbulence! Your data is probably fine lol."; }
        if (recent == 1) { *c = A_CREAM; return now - s_bumps[0] < 4000 ? "Ouff, that's bumpy!" : "Aight, we good!"; }
        if (s_nbumps && now - s_bumps[0] < 20000) return "Aight, we good!";
        if (s_rx.loud < 4 && now - s_quiet_since > 4000) { *c = A_CREAM; return "Are we there yet?"; }
        return calm ? "Smooth sailing." : "Yeaah. Smoooth.";
    }
    if (*s_shout) return s_shout;
    if (s_mode == M_RESULT && *s_outcome) { *c = s_outcome_c; return s_outcome; }
    if (s_mode == M_WROTE) return "Aight, we good!";
    return "";
}

/* ---- the screen ----------------------------------------------------------------- */
#define ROW_SET  4                       /* the settings start here         */
#define ROW_CAS  16                      /* the cassette: the line under its heading */
#define ROW_LIST 26                      /* the files                       */

/* Words in the frame's own turning colours (red, gold and green run
 * through the letters by themselves), and a glint that sweeps across them
 * every few seconds: three letters lit, moved on each time the screen is
 * drawn anyway.  from: how many letters of the whole came on earlier
 * lines, and all: the whole's length, so one glint runs across them all. */
static void shiny(int x, int y, const char *t, int from, int all)
{
    int i, at = (int)(A->ms() / 60 % (u32)(all + 40)) - 3;
    for (i = 0; t[i]; i++) {
        int d = from + i - at;
        u8 c = (u8)(A_NEON + (A_NEON_N - 1 - (from + i) % A_NEON_N));
        if (d == 0) c = A_BRIGHT;
        else if (d == -1 || d == 1) c = A_CREAM;
        else if (d == -2 || d == 2) c = A_GOLD;
        A->win_put(x + i, y, (u8)t[i], c, A_PAPER, d == 0 ? AT_BOLD : 0);
    }
}

/* A setting with two values: the one in force in green. */
static int two(int x, int y, const char *key, const char *label, const char *a, const char *b, int second)
{
    char m[40];
    A->format(m, sizeof m, "\1%s\1 %s  ", key, label);
    x = text(x, y, m, A_CREAM);
    x = text(x, y, a, second ? A_GREY : A_ON) + 2;
    return text(x, y, b, second ? A_ON : A_GREY);
}

/* A section's name, and a rule to the right of it. */
static void head(int y, const char *t)
{
    int x = text(2, y, t, A_GOLD) + 1;
    for (; x < W - 2; x++) A->win_put(x, y, A_FR_H, A_EMBER, A_PAPER, 0);
}

/* The places to restore into, as the list D opens: a folder of the
 * read's own, where each came from, the top, every folder, and a new one.
 * Entry i is s_dest i - 2. */
static int dest_count(void) { return s_ndirs + 3; }
static void dest_name(int i, char *m, u32 cap)
{
    if (i == 0) A->format(m, cap, "%s", "a folder named by the date (DDMMYYYY)");
    else if (i == 1) A->format(m, cap, "%s", "where each file came from");
    else if (i == 2) A->format(m, cap, "%s", "the top of the inventory");
    else if (i <= s_ndirs + 1) A->format(m, cap, "%s/", s_dirs[i - 2]);
    else A->format(m, cap, "%s", "a folder you name...");
}

static void file_state(const File *f, int i, char *m, u32 cap, u8 *c)
{
    *c = A_OLIVE;
    m[0] = 0;
    if (s_mode == M_WRITE || s_mode == M_WROTE) {
        if (!f->send) return;
        if (s_mode == M_WROTE || s_at > i) { A->format(m, cap, "%s", "on the tape"); *c = A_CREAM; }
        else if (s_at == i) { A->format(m, cap, "writing  %u%%", (u32)(f->pieces ? f->got * 100 / f->pieces : 100)); *c = A_GOLD; }
        else A->format(m, cap, "%s", "waiting");
        return;
    }
    if (s_mode == M_READ || s_mode == M_RESULT) {
        u32 acc = f->got + f->bad ? (u32)f->got * 100 / (u32)(f->got + f->bad) : 100;
        switch (f->state) {
            case ST_PART: A->format(m, cap, "%u%% read, accuracy %u%%", (u32)(f->pieces ? f->got * 100 / f->pieces : 0), acc); *c = A_GOLD; break;
            case ST_SAME: A->format(m, cap, "%s, accuracy %u%%", s_verifying ? "matches" : "same", acc); *c = s_verifying ? A_CREAM : A_GREY; break;
            case ST_DIFF: A->format(m, cap, "%s, accuracy %u%%", f->here ? "different" : "new", acc); *c = A_GOLD; break;
            case ST_BACK: A->format(m, cap, "%s", "restored"); *c = A_ON; break;
            case ST_BAD:  A->format(m, cap, "%s", "left alone"); *c = A_RED; break;
            default: break;
        }
        return;
    }
    A->format(m, cap, "%u bytes", f->size);
}

static void draw(void)
{
    char m[100];
    int x, y, i, rows, busy = s_mode == M_WRITE || s_mode == M_READ;
    int px = 2 + CW + 1, pw = W - px - 8;

    A->win_open(W, H);
    A->win_frame("maelcum", A_OLIVE);
    shiny(2, 2, "Magnetic Rust: Riddim 'n' Vibrationz Engine", 0, 43);

    /* the settings: what is in force in green, what is not in grey */
    y = ROW_SET;
    head(y, "Write Options");
    x = text(2, y + 1, "\1J\1 Job  ", A_CREAM);
    x = text(x, y + 1, s_job_name[s_job], A_ON) + 1;
    text(x, y + 1, s_job_what[s_job], A_GREY);
    x = two(2, y + 2, "C", "Compress (LZSS)", "on", "off", !s_packing);
    two(x + 4, y + 2, "F", "Speed", "normal", "fast", s_fast);
    two(2, y + 4, "V", "Verify after writing", "on", "off", !s_verify);
    x = two(2, y + 5, "E", "Encrypt", "on", "off", !s_crypt);
    x = text(x + 4, y + 5, "\1K\1 Cipher  ", A_CREAM);
    text(x, y + 5, s_alg_name[s_alg], s_crypt ? A_ON : A_GREY);
    head(y + 7, "Read Options");
    two(2, y + 8, "Tab", "Listen on", s_line ? "line-in socket active" : "line-in socket", s_line ? "microphone" : "microphone active", !s_line);
    x = text(2, y + 9, "\1D\1 Restore into  ", A_CREAM);
    dest_name(s_dest + 2, m, sizeof m);
    text(x, y + 9, m, A_ON);
    head(y + 11, "Run 'Tingz");

    /* the cassette, and how it is going: eight lines beside it */
    cassette(2, ROW_CAS, busy);
    y = ROW_CAS;
    if (s_mode == M_ARMED || s_mode == M_WRITE) {
        text(px, y + 4, "Cassette Storage Legend (per tape side):", A_OLIVE);
        text(px, y + 5, "C30 = 15 min of data", A_OLIVE);
        text(px, y + 6, "C60 = 30 min of data", A_OLIVE);
        text(px, y + 7, "C90 = 45 min of data", A_OLIVE);
    }
    if (s_mode == M_ARMED) {
        u32 secs = s_samples / TAPE_RATE + 1;
        A->format(m, sizeof m, "%u file%s, %u bytes: %u min %u s of tape.", (u32)s_count, s_count == 1 ? "" : "s", s_bytes, secs / 60, secs % 60);
        text(px, y, m, A_CREAM);
        text(px, y + 1, "Plug a cable from the headphone socket", A_CREAM);
        text(px, y + 2, "(front) into the recorder's MIC socket.", A_CREAM);
        text(px, y + 3, "Set it recording, then press \1Enter\1.", A_CREAM);
    } else if (s_mode == M_WRITE) {
        u32 heard_n = s_sent - (u32)A->audio_queued();
        u32 left = s_samples > heard_n ? (s_samples - heard_n) / TAPE_RATE : 0;
        u32 pc = heard_n >= s_samples ? 100 : heard_n / (s_samples / 100 + 1);
        text(px, y, "Writing: these tones are your files.", A_CREAM);
        bar(px, y + 1, pw, (int)(pc * (u32)pw / 100), A_RED);
        A->format(m, sizeof m, "%u%%", pc);
        text(px + pw + 2, y + 1, m, A_GOLD);
        A->format(m, sizeof m, "file %u of %u    %u:%02u left", (u32)(s_ord + 1 > s_count ? s_count : s_ord + 1), (u32)s_count, left / 60, left % 60);
        text(px, y + 2, m, A_CREAM);
    } else if (s_mode == M_READ) {
        int pc = read_percent();
        u32 all = s_rx.good + s_rx.bad;
        text(px, y, s_verifying ? "Written. Stop, rewind, then press PLAY." : "Reading. Rewind, then press PLAY.", A_CREAM);
        if (s_set >= 0) A->format(m, sizeof m, "accuracy %u%%   %u file%s   %s speed", all ? s_rx.good * 100 / all : 100, (u32)s_set_n, s_set_n == 1 ? "" : "s", s_rx.fast ? "fast" : "normal");
        else A->format(m, sizeof m, "%s", s_line ? "Cable: EAR to line-in (left: note, arrow)" : "Cable: EAR to the microphone socket");
        text(px, y + 1, m, s_set >= 0 ? A_CREAM : A_GOLD);
        bar(px, y + 3, pw, pc * pw / 100, A_RED);
        A->format(m, sizeof m, "%u%%", (u32)pc);
        text(px + pw + 2, y + 3, m, A_GOLD);
        bar(px, y + 5, pw, (s_rx.loud * pw + 63) / 127, A_ON);
        if (s_rx.loud < 4) A->format(m, sizeof m, "%s", "silence");
        else if (s_rx.tone) A->format(m, sizeof m, "%s", "a tape's tones");
        else A->format(m, sizeof m, "sound, not a tape's (%u waves a second)", s_rx.pitch);
        text(px, y + 6, m, s_rx.tone ? A_GOLD : A_OLIVE);
    } else {
        /* the two things to do, from just inside the cassette's top edge */
        text(px, y + 1, s_mode == M_PICK ? "\1W\1  Write the ticked files: press it again" : "\1W\1  Write files to a tape", A_CREAM);
        text(px, y + 2, "\1R\1  Read a tape and restore from it", A_CREAM);
        text(px, y + 3, "\1G\1  Generate Tape Sleeve", A_CREAM);
        if (s_done) {                            /* where G will put it */
            sleeve_where(m, sizeof m);
            text(px + 4, y + 4, m, A_OLIVE);
        }
        if (tape_list() && s_nrows) {
            text(px, y + 6, "Tick below what to restore. \1Enter\1", A_CREAM);
            text(px, y + 7, "puts the ticked files on the floppy.", A_CREAM);
        } else if (s_mode == M_RESULT && s_others) text(px, y + 7, "(The tape holds another save as well.)", A_OLIVE);
        else if (s_mode == M_PICK && s_big) { A->format(m, sizeof m, "(%u too long for a tape, left out.)", (u32)s_big); text(px, y + 7, m, A_OLIVE); }
    }
    if (*s_note && s_mode != M_READ && s_mode != M_WRITE && s_mode != M_ARMED) {
        /* the last thing that happened: broken between words where it
         * would run into the frame, over what is shown under it */
        const char *t = s_note;
        int room = W - 2 - px, row = y + 5, n, k, cut;
        char line[80];
        while (*t && row <= y + 7) {
            for (n = 0, k = 0; t[n] && k < room && n < 79; n++) k += t[n] != 1;
            cut = n;
            if (t[n]) { for (cut = n; cut > 0 && t[cut] != ' '; cut--) { } if (!cut) cut = n; }
            for (k = 0; k < cut; k++) line[k] = t[k];
            line[cut] = 0;
            A->win_band(row, px, W - 2, A_PAPER);
            text(px, row++, line, s_note_c);
            t += cut;
            while (*t == ' ') t++;
        }
    }

    /* the files: one folder, as the inventory shows it */
    rows = H - 6 - ROW_LIST;
    if (s_mode == M_WRITE && s_at < s_nf) {       /* follow the file in hand */
        if (s_f[s_at].dir != s_view || s_rows[s_cursor] != 1000 + s_at) view(s_f[s_at].dir, 1000 + s_at);
    } else if (s_mode == M_READ && tape_list()) {  /* the tape's files as they come, the one in hand marked */
        build_rows();
        for (i = 0; i < s_nrows; i++) if (s_rows[i] == 1000 + s_cur) s_cursor = i;
    } else if (s_mode == M_READ && s_cur >= 0) {
        if (s_f[s_cur].dir != s_view || s_rows[s_cursor] != 1000 + s_cur) view(s_f[s_cur].dir, 1000 + s_cur);
    }
    if (s_cursor < s_top) s_top = s_cursor;
    if (s_cursor >= s_top + rows) s_top = s_cursor - rows + 1;
    for (i = 1; i < W - 1; i++) A->win_put(i, ROW_LIST - 1, A_FR_H, A_EMBER, A_PAPER, 0);
    if (no_list()) A->format(m, sizeof m, "%s", " Files ");
    else if (tape_list()) A->format(m, sizeof m, "%s", s_mode == M_READ ? " On the cassette: coming in " : " On the cassette: tick what to restore ");
    else if (s_view == -1) A->format(m, sizeof m, "%s", " On the floppy: FLOPOS's own records ");
    else A->format(m, sizeof m, " On the floppy: /%s%s ", s_dirs[s_view], s_view ? "/" : "");
    text(2, ROW_LIST - 1, m, A_GOLD);
    if (no_list()) {
        text(2, ROW_LIST, "\1W\1 shows the files on the floppy, to tick and write to a tape.", A_GREY);
        text(2, ROW_LIST + 1, "\1R\1 shows the cassette's files as it plays, to tick and restore.", A_GREY);
    }
    else if (!s_nrows) text(2, ROW_LIST, tape_list() ? (s_mode == M_READ ? "Nothing heard yet." : "Nothing came from the tape.") : "Nothing here yet.", A_GREY);
    for (i = 0; i < rows && s_top + i < s_nrows; i++) {
        int r = s_rows[s_top + i], here = s_top + i == s_cursor && !busy;
        u8 bg = here ? A_GREEN_D : A_PAPER, c;
        y = ROW_LIST + i;
        if (here) A->win_band(y, 1, W - 2, A_GREEN_D);
        if (r < 1000) {
            int t = folder_tick(r), k, cut = -1;
            A->win_text(2, y, t == 1 ? "[x]" : t == 2 ? "[-]" : "[ ]", t ? A_ON : A_GREY, bg);
            if (r == -1) A->format(m, sizeof m, "%s", "FLOPOS's own records");
            else {
                for (k = 0; s_dirs[r][k]; k++) if (s_dirs[r][k] == '/') cut = k;
                A->format(m, sizeof m, "%s/", s_dirs[r] + cut + 1);
            }
            A->win_text(6, y, m, A_GOLD, bg);
            A->win_text(W - 8, y, "folder", A_OLIVE, bg);
        } else {
            const File *f = &s_f[r - 1000];
            const char *name = f->name;
            int k, cut = -1, tl = tape_list(), on = tl ? f->take : f->on;
            for (k = 0; name[k]; k++) if (name[k] == '/') cut = k;
            if (tl) { if (k > 30) name += k - 30; }      /* the tape's: with its folders */
            else name += cut + 1;
            if (tl && f->state == ST_PART) A->win_text(2, y, "   ", A_GREY, bg);   /* not whole: nothing to tick */
            else A->win_text(2, y, on ? "[x]" : "[ ]", on ? A_ON : A_GREY, bg);
            A->win_text(6, y, name, on || (tl && f->state != ST_SAME) ? A_CREAM : A_GREY, bg);
            file_state(f, r - 1000, m, sizeof m, &c);
            A->win_text(W - 2 - len_of(m), y, m, c, bg);
        }
    }

    /* a list, open over the settings: the jobs, the ciphers, or where to restore */
    if (s_menu) {
        int n = s_menu == 1 ? 2 : s_menu == 2 ? 3 : dest_count();
        int row0 = ROW_SET + (s_menu == 1 ? 2 : s_menu == 2 ? 6 : 10), shown = n > 9 ? 9 : n;
        int top = s_pick < shown ? 0 : s_pick - shown + 1;
        for (i = 0; i < shown; i++) {
            int e = top + i;
            u8 bg = s_pick == e ? A_EMBER : A_GREEN_D;
            int cur = s_menu == 1 ? s_job == e : s_menu == 2 ? s_alg == e : s_dest + 1 == e;
            A->win_band(row0 + i, 8, W - 3, bg);
            if (s_menu == 3) dest_name(e, m, sizeof m);
            else A->format(m, sizeof m, "%s", s_menu == 1 ? s_job_name[e] : s_alg_name[e]);
            A->win_text(9, row0 + i, m, s_pick == e ? A_CREAM : cur ? A_ON : A_GOLD, bg);
            if (s_menu != 3) A->win_text(28, row0 + i, s_menu == 1 ? s_job_what[e] : s_alg_what[e], A_CREAM, bg);
            else if (e == n - 1) A->win_text(48, row0 + i, "(made at the top)", A_CREAM, bg);
        }
    }

    /* how it is going */
    {
        u8 c;
        const char *t = mood_line(&c);
        char line[80];
        int row = H - 6, wide = W - 4 < 78 ? W - 4 : 78, n, k, cut;
        int glow = t == s_outcome && *t, from = 0, all = len_of(t);   /* the verdict shines like the title */
        while (*t && row <= H - 4) {  /* up to three lines, broken between words */
            for (n = 0; t[n] && n < wide; n++) { }
            cut = n;
            if (t[n]) { for (cut = n; cut > 0 && t[cut] != ' '; cut--) { } if (!cut) cut = n; }
            for (k = 0; k < cut; k++) line[k] = t[k];
            line[cut] = 0;
            if (glow) shiny(2, row++, line, from, all);
            else text(2, row++, line, c);
            from += cut + 1;
            t += cut;
            while (*t == ' ') t++;
        }
    }

    /* the keys */
    if (s_menu) text(2, H - 2, "\1Up\1 \1Down\1 choose   \1Enter\1 take it   \1Esc\1 close", A_OLIVE);
    else if (busy) text(2, H - 2, s_mode == M_READ ? "\1S\1 stop   \1Tab\1 the other socket" : "\1S\1 stop", A_OLIVE);
    else if (s_mode == M_ARMED) text(2, H - 2, "\1Enter\1 start   \1Esc\1 not now", A_OLIVE);
    /* (the keys shown above, by the settings and in Run 'Tingz, are not repeated here) */
    else if (tape_list()) text(2, H - 2, "\1Up\1 \1Down\1 move  \1Space\1 tick  \1Enter\1 restore the ticked  \1Esc\1 done", A_OLIVE);
    else if (s_mode == M_PICK) text(2, H - 2, "\1Up\1 \1Down\1 move  \1Space\1 tick  \1Enter\1 open  \1Backspace\1 up  \1Esc\1 back", A_OLIVE);
    else if (no_list()) text(2, H - 2, "\1Esc\1 leave", A_OLIVE);
    else text(2, H - 2, "\1Up\1 \1Down\1 move  \1Esc\1 done", A_OLIVE);
}

/* ---- asking for a line: a passphrase (shown as stars), or a folder's name -------- */
static void draw(void);

/* Draw now and put it on the screen: before a tune, which holds everything up. */
static void show_now(void)
{
    draw();
    A->pump(1);
}

static void busy_note(const char *t)
{
    draw();
    A->win_band(H - 6, 1, W - 2, A_GREEN_D);
    A->win_text(2, H - 6, t, A_CREAM, A_GREEN_D);
    A->pump(1);
}

static int ask_line(const char *prompt, char *buf, int cap, int hidden)
{
    int n = 0, key, mods, i, x;
    u32 drawn = 0;
    buf[0] = 0;
    for (;;) {
        u32 now = A->ms();
        if (now - drawn >= 100 || !drawn) {
            drawn = now ? now : 1;
            draw();
            for (i = H - 6; i <= H - 4; i++) A->win_band(i, 1, W - 2, A_GREEN_D);
            x = A->win_text(2, H - 6, prompt, A_CREAM, A_GREEN_D) + 1;
            for (i = n > W - 5 - x ? n - (W - 5 - x) : 0; i < n; i++) A->win_put(x++, H - 6, hidden ? '*' : buf[i], A_GOLD, A_GREEN_D, 0);
            A->win_put(x, H - 6, '_', A_GOLD, A_GREEN_D, 0);
            if (hidden) {
                A->win_text(2, H - 5, n < 12 ? "Short: easy to guess. Four random words is good."
                                              : "A long one is what keeps it shut. It is kept nowhere:",
                            n < 12 ? A_RED : A_ON, A_GREEN_D);
                if (n >= 12) A->win_text(2, H - 4, "forget it and the tape cannot be opened by anyone.", A_ON, A_GREEN_D);
            }
            A->win_band(H - 3, 1, W - 2, A_PAPER);
            A->win_band(H - 2, 1, W - 2, A_PAPER);
            text(2, H - 2, "\1Enter\1 done   \1Esc\1 not now", A_OLIVE);
        }
        key = A->key(&mods);
        if (key < 0) { A->pump(0); continue; }
        drawn = 0;
        if (key == K_ESC) { for (i = 0; i < cap; i++) buf[i] = 0; return 0; }
        if (key == K_ENTER) return 1;
        if (key == K_BKSP) { if (n) buf[--n] = 0; }
        else if (key >= 32 && key < 127 && n < cap - 1) { buf[n++] = (char)key; buf[n] = 0; }
    }
}

/* A folder you name, to restore into: made at once, at the top of the inventory. */
static void new_folder(void)
{
    char name[16];
    InvItem it;
    int i, d;
    if (!ask_line("New folder's name:", name, 9, 0) || !name[0]) return;
    for (i = 0; name[i]; i++) {
        int c = up(name[i]);
        name[i] = (char)c;
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) break;
    }
    if (name[i]) { say("Folder names: 8 letters or digits at most.", A_RED); return; }
    if (A->inv_stat(name, &it)) {
        if (!it.folder) { say("There is a file by that name.", A_RED); return; }
        d = dir_for(name);
        if (d >= 0) { s_dest = d; say("It is there already: chosen.", A_CREAM); }
        return;
    }
    if (!A->inv_mkdir(name)) {
        static char why[80];
        A->format(why, sizeof why, "%s", A->inv_why());
        say(why, A_RED);
        return;
    }
    d = dir_for(name);
    if (d < 0) { say("Too many folders.", A_RED); return; }
    s_dest = d;
    wire("made", name, 0, 0);
    say("Made. What is restored goes into it.", A_ON);
}

/* ---- the sleeve: G ---------------------------------------------------------------- */
static int card_file(int i, const char **name, u32 *size)
{
    int k;
    for (k = 0; k < s_nf; k++)
        if (s_f[k].card && i-- == 0) { *name = s_f[k].name; *size = s_f[k].card_size; return 1; }
    return 0;
}

/* Where the sleeve goes: with the files read (their folder), or, for a
 * tape written, at the top of the inventory. */
static void sleeve_dir(char *m, u32 cap)
{
    m[0] = 0;
    if (s_done != 2 || s_dest == -1 || s_dest == 0) return;
    if (s_dest > 0) A->format(m, cap, "%s", s_dirs[s_dest]);
    else if (s_auto >= 0) A->format(m, cap, "%s", s_dirs[s_auto]);
    else if (!auto_name(m, cap)) A->format(m, cap, "%s", "TAPE..");
}

static void sleeve_where(char *m, u32 cap)
{
    char d[INV_PATH];
    sleeve_dir(d, sizeof d);
    A->format(m, cap, "(goes to /%s%sSLEEVE.PDF)", d, d[0] ? "/" : "");
}

static void make_sleeve(void)
{
    static char why[80];
    char path[INV_PATH], d[INV_PATH];
    const u8 *pdf;
    Sleeve sl;
    u32 n;
    int i;
    if (!s_done) { say("Write or read a tape first.", A_CREAM); return; }
    if (s_done == 2 && s_dest == -2 && !ensure_auto()) {
        A->format(why, sizeof why, "%s", A->inv_why());
        say(why, A_RED);
        return;
    }
    busy_note("Drawing the sleeve and saving it...");
    for (i = 0; i < 6; i++) sl.when[i] = s_done_when[i];
    sl.have_when = s_done_clock;
    sl.did = s_done == 1 ? "Written" : s_done == 2 ? "Read" : "Written and checked";
    sl.verdict = s_done_verdict; sl.version = A->os_version();
    sl.cipher = s_done_alg >= 0 ? s_alg_name[s_done_alg] : 0;
    sl.files = s_done_files; sl.bytes = s_done_bytes;
    sl.fast = s_done_fast; sl.packed = s_done_packed;
    sl.file = card_file;
    n = sleeve_pdf(A, &sl, &pdf);
    sleeve_dir(d, sizeof d);
    A->format(path, INV_PATH, "%s%sSLEEVE.PDF", d, d[0] ? "/" : "");
    if (!A->inv_write(path, pdf, n)) {
        A->format(why, sizeof why, "%s", A->inv_why());
        say(why, A_RED);
        return;
    }
    now_here(path, n);
    wire("sleeve", path, n, 0);
    build_rows();
    say("Saved. Print it at 100% on A4.", A_ON);
}

/* ---- the app ---------------------------------------------------------------- */
int app_main(const Api *api, int argc, char **argv)
{
    u32 drawn = 0;
    int key, mods, i, a, start = 0;

    A = api;
    /* No background music while maelcum runs; and while a tape is being
     * written or read, no key sounds either (card_take, card_done). */
    A->hush(1);
    if (argc >= 2) {
        if (same_word(argv[1], "save")) start = 1;
        else if (same_word(argv[1], "load")) start = 2;
        else {
            static const char *const help[] = {
                "maelcum            the backup screen",
                "maelcum save       ready to write your files to a tape",
                "maelcum load       listen to a tape, to restore from it",
                "maelcum save NAME  only the file called NAME",
            };
            A->colour(A_CREAM);
            for (i = 0; i < 4; i++) { A->print(help[i]); A->newline(); }
            return 0;
        }
    }
    find_files();
    if (!s_nf) {
        A->colour(A_CREAM); A->print("None of your files are on this floppy."); A->newline();
        return 0;
    }
    if (argc > 2) {                              /* names: only those, which is Pick & Mix */
        s_job = 1;
        for (i = 0; i < s_nf; i++) {
            const char *name = s_f[i].name;
            int k, cut = -1;
            for (k = 0; name[k]; k++) if (name[k] == '/') cut = k;
            s_f[i].on = 0;
            for (a = 2; a < argc; a++)
                if (same_word(argv[a], name) || same_word(argv[a], name + cut + 1)) s_f[i].on = 1;
        }
    }
    W = A->screen_cols(); H = A->screen_rows();
    A->win_ember(1);                             /* this screen keeps to red, gold and green; keys yellow */
    build_rows();
    if (start == 1) write_arm();
    if (start == 2) read_go(0);

    for (;;) {
        u32 now = A->ms();
        if (s_mode == M_WRITE) write_step();
        else if (s_mode == M_READ) read_step();
        if (now - drawn >= 100 || !drawn) { drawn = now ? now : 1; draw(); }
        key = A->key(&mods);
        if (key < 0) { A->pump(0); continue; }
        drawn = 0;
        if (key >= 'a' && key <= 'z') key = key - 'a' + 'A';

        if (s_menu) {                            /* a list is open */
            int n = s_menu == 1 ? 2 : s_menu == 2 ? 3 : dest_count();
            if (key == K_UP) s_pick = (s_pick + n - 1) % n;
            else if (key == K_DOWN) s_pick = (s_pick + 1) % n;
            else if (key == K_ENTER) {
                int m = s_menu;
                s_menu = 0;
                if (m == 1) { s_job = s_pick; if (!s_job) tick_all(); }
                else if (m == 2) s_alg = s_pick;
                else if (s_pick == n - 1) new_folder();
                else s_dest = s_pick - 2;
            }
            else if (key == K_ESC || key == 'J' || key == 'K' || key == 'D') s_menu = 0;
            continue;
        }
        if (s_mode == M_WRITE) {
            if (key == 'S' || key == K_ESC) {
                card_done();
                forget_key();
                s_mode = M_IDLE;
                say("Stopped. The tape holds only part of it.", A_RED);
                wire("stopped", "saving", 0, 0);
            }
            continue;
        }
        if (s_mode == M_READ) {
            if (key == 'S' || key == K_ESC) read_end(1);
            else if (key == K_TAB) {
                s_line = !s_line;
                A->audio_close();               /* still hushed: straight on to listening */
                if (!card_take(1)) { s_mode = M_IDLE; say("No sound card that can listen answers.", A_RED); }
                else { tape_rx_begin(&s_rx, heard, 0); s_cur = -1; s_bad_was = 0; s_quiet_since = now; }
            }
            continue;
        }
        if (s_mode == M_ARMED) {
            if (key == K_ENTER) write_go();
            else if (key == K_ESC) { s_mode = M_PICK; forget_key(); }
            continue;
        }
        /* idle, choosing what to write, or looking at what a tape held */
        if (key == K_ESC && s_mode != M_IDLE) { s_mode = M_IDLE; say("", 0); build_rows(); }   /* the list empties */
        else if (key == K_ESC) break;
        else if (key == 'W' && s_mode == M_PICK) write_arm();
        else if (key == 'W') { s_mode = M_PICK; say("", 0); view(0, 0); }
        else if (key == 'R') { s_mode = M_IDLE; say("", 0); read_go(0); }
        else if (key == K_ENTER && tape_list()) put_back();
        else if (no_list() && key != 'G' && key != 'J' && key != 'K' && key != 'E' && key != 'C' && key != 'F' &&
                 key != 'V' && key != K_TAB && key != 'D') { }
        else if (tape_list() && (key == K_LEFT || key == K_RIGHT || key == K_BKSP)) { }
        else if ((key == K_ENTER || key == K_RIGHT) && s_nrows && s_rows[s_cursor] < 1000) view(s_rows[s_cursor], 0);
        else if ((key == K_BKSP || key == K_LEFT) && s_view != 0) { int was = s_view; view(parent_of(was), was); }
        else if (key == 'G') make_sleeve();
        else if (key == 'J') { s_menu = 1; s_pick = s_job; }
        else if (key == 'K') { s_menu = 2; s_pick = s_alg; }
        else if (key == 'E') s_crypt = !s_crypt;
        else if (key == 'C') s_packing = !s_packing;
        else if (key == 'F') s_fast = !s_fast;
        else if (key == 'V') { s_verify = !s_verify; s_shout = s_verify ? "If it's NICE - Spin it TWICE, SELECTA!" : ""; }
        else if (key == K_TAB) s_line = !s_line;
        else if (key == 'D') { s_menu = 3; s_pick = s_dest + 2; }
        else if (key == K_UP) { if (s_cursor > 0) s_cursor--; }
        else if (key == K_DOWN) { if (s_cursor + 1 < s_nrows) s_cursor++; }
        else if (key == K_PGUP) { s_cursor = s_cursor > 10 ? s_cursor - 10 : 0; }
        else if (key == K_PGDN) { s_cursor = s_cursor + 10 < s_nrows ? s_cursor + 10 : s_nrows - 1; }
        else if (key == ' ' && s_nrows) tick_row(s_cursor);
    }
    A->win_close();
    return 0;
}
