/* ===========================================================================
 * maelcum.c -- backup: your files, kept on an audio cassette.
 *
 * Named for the pilot in William Gibson's Neuromancer (1984), who got
 * people home.
 *
 * MAELCUM.APP on the floppy.  One full screen: the settings at the top, a
 * cassette and the progress under them, then your files with a tick by
 * each.  Writing plays the ticked files out of the headphone socket as
 * two tones, for a cassette recorder to record; reading listens to the
 * recorder playing them back, compares what it hears with the floppy, and
 * puts it back if asked.  The sound itself is apps/lib/tape.
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
 * left is sent as LZSS (apps/lib/tape/lz.c) when that is shorter.  A block
 * that is spoiled is simply missing: the tape can be played again and
 * only the gaps are filled.
 * ===========================================================================
 */
#include "app.h"
#include "tape.h"

#define PIECES   255                     /* a file's most blocks: 65280 bytes */
#define FILE_MAX (PIECES * TAPE_PIECE)
#define MAXF     200
#define MAXD     64
#define HEAD_OLD (INV_PATH + 16)         /* a header before packing came    */
#define HEAD_LEN (INV_PATH + 20)
#define ARENA    (800 * 1024)            /* more than a floppy has free    */

enum { ST_NONE, ST_PART, ST_SAME, ST_DIFF, ST_BACK, ST_BAD };
enum { M_IDLE, M_ARMED, M_WRITE, M_WROTE, M_READ, M_RESULT };

typedef struct {
    char name[INV_PATH];                 /* in the inventory: its path     */
    int  sys;                            /* one of the system's three      */
    int  dir;                            /* which folder; -1 the system's, -2 only the tape has it */
    AppFile f;                           /* where a system one is          */
    int  here;                           /* it is on the floppy            */
    int  on;                             /* ticked                         */
    u32  size, used;                     /* its length on the floppy; bytes in use, before the filling */
    u32  plen;                           /* packed length, or 0            */
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
static int s_dest = -1;                  /* restore into: -1 where each came from, or a folder */
static int s_mode, s_verifying, s_menu;  /* s_menu: the job list is open, on which line + 1 */
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

static const char *const s_job_name[2] = { "Standard Backup", "Pick & Mix" };
static const char *const s_job_what[2] = { "(everything of yours on this floppy)", "(only what you tick below)" };

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
    f->on = 1;
    f->here = 0; f->state = ST_NONE; f->got = f->bad = 0; f->plen = 0;
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
    n = f->plen ? f->plen : f->used;
    f->pieces = (int)((n + TAPE_PIECE - 1) / TAPE_PIECE);
    return 1;
}

static u32 sent_len(const File *f) { return f->plen ? f->plen : f->used; }

/* ---- the list on the screen: folders and files, with ticks ---------------------
 * A row is a folder (>= 0: its number; -1 the system's records; -2 what
 * only the tape has) or a file (its number + 1000). */
static int s_rows[MAXF + MAXD + 4], s_nrows;

static void build_rows(void)
{
    int d, i, any;
    s_nrows = 0;
    for (d = 0; d < s_ndirs; d++) {
        if (d) s_rows[s_nrows++] = d;
        for (i = 0; i < s_nf; i++) if (s_f[i].dir == d) s_rows[s_nrows++] = 1000 + i;
    }
    for (d = -1; d >= -2; d--) {
        any = 0;
        for (i = 0; i < s_nf; i++) any |= s_f[i].dir == d;
        if (!any) continue;
        s_rows[s_nrows++] = d;
        for (i = 0; i < s_nf; i++) if (s_f[i].dir == d) s_rows[s_nrows++] = 1000 + i;
    }
    if (s_cursor >= s_nrows) s_cursor = s_nrows ? s_nrows - 1 : 0;
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
#define CW 21
#define CH 10
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
    box(x + 3, y + 2, CW - 6, 5, c);             /* the window with the two wheels */
    for (i = 0; i < 2; i++) {
        int wx = x + 6 + i * 6;
        A->win_put(wx, y + 4, '(', c, A_PAPER, 0);
        A->win_put(wx + 1, y + 4, spoke[s], turning ? A_ON : A_OLIVE, A_PAPER, 0);
        A->win_put(wx + 2, y + 4, ')', c, A_PAPER, 0);
    }
    /* the opening at the bottom, where the heads go */
    A->win_put(x + 4, y + 8, '/', c, A_PAPER, 0);
    A->win_put(x + CW - 5, y + 8, '\\', c, A_PAPER, 0);
    for (i = 5; i < CW - 5; i++) A->win_put(x + i, y + 8, A_FR_H, c, A_PAPER, 0);
    A->win_put(x + 7, y + 8, 'o', c, A_PAPER, 0);
    A->win_put(x + CW - 8, y + 8, 'o', c, A_PAPER, 0);
}

/* ---- the trudelutt ------------------------------------------------------------- */
static void tune(void)
{
    /* pitch in Hz and length in ms; a pitch of 0 rests; a negative one slides up to the next */
    static const short notes[][2] = {
        { 523, 70 }, { 659, 70 }, { 784, 70 }, { 1047, 70 }, { 784, 70 }, { 1047, 140 }, { 0, 60 },
        { -300, 260 }, { 1800, 40 }, { 0, 70 },
        { 988, 60 }, { 0, 40 }, { 988, 60 }, { 0, 40 }, { 1319, 110 }, { 0, 50 },
        { 196, 90 }, { 185, 90 }, { 131, 260 }, { 0, 80 }, { 2093, 50 }, { 0, 30 }, { 2637, 120 },
    };
    static u8 buf[512];
    u32 phase = 0;
    int i, n, k, rate = A->audio_open(TAPE_RATE);
    if (!rate) return;
    for (i = 0; i < (int)(sizeof notes / sizeof notes[0]); i++) {
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
    A->audio_close();
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
        s_samples += block_samples(HEAD_LEN, s_count ? 1000 : 8000) + block_samples(HEAD_LEN, 100) + 2 * block_samples(0, 100);
        for (n = 0; n < f->pieces; n++) {
            u32 left = sent_len(f) - (u32)n * TAPE_PIECE;
            s_samples += block_samples(left > TAPE_PIECE ? TAPE_PIECE : (int)left, 100);
        }
        s_bytes += f->used;
        s_count++;
    }
    return s_count;
}

static void write_arm(void)
{
    if (!write_ready()) { say("Nothing ticked that the floppy can give.", A_RED); return; }
    s_mode = M_ARMED;
    say("", 0);
}

static void write_go(void)
{
    if (!A->audio_open(TAPE_RATE)) { s_mode = M_IDLE; say("No sound card that plays answers.", A_RED); return; }
    s_at = s_stage = s_ord = 0;
    s_save_id = (u16)(A->ms() * 2654435761u >> 16);
    tape_tx_begin(&s_tx, s_fast);
    s_sent = 0; s_last = 0;
    s_began = A->ms(); s_shout = "";
    s_mode = M_WRITE;
    wire("saving", "files", (u32)s_count, s_samples / TAPE_RATE + 1);
}

static void read_go(int verifying);

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
        A->audio_close();
        wire("saved", "files", (u32)s_count, s_bytes);
        s_mode = M_WROTE;
        if (s_verify) read_go(1);                /* not done yet: the tune waits for the check */
        else { tune(); say("All on the tape. Stop the recorder.", A_ON); }
    }
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
    if (f->plen) sound = lz_unpack(f->pk, f->plen, f->tape, f->used) == f->used;
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
        int i, k, ord, count, set;
        u16 crc;
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
        if (set != s_set || count != s_set_n) { s_others = 1; return; }
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
        /* Not ticked, or one of the system's at a size this floppy does not keep it at. */
        if (!f->on || (f->sys && size != f->size)) { s_seen[ord] = -1; return; }
        if (f->state == ST_NONE || f->used != used || f->crc != crc || f->pad != d[k + 8] || f->tsize != size || f->plen != plen) {
            if (s_tape_n + size + plen > sizeof s_tape) { s_seen[ord] = -1; return; }
            f->tape = s_tape + s_tape_n;
            f->pk = f->tape + size;
            s_tape_n += size + plen;
            f->tsize = size; f->used = used; f->crc = crc; f->pad = d[k + 8]; f->plen = plen;
            n = plen ? plen : used;
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
        to = f->plen ? f->pk : f->tape;
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
    s_disk_n = s_pack_n = s_tape_n = 0;
    for (i = 0; i < s_nf; i++) {
        File *f = &s_f[i];
        f->state = ST_NONE; f->disk = f->send = 0; f->got = f->bad = 0;
        if (f->here) from_floppy(f, 0);
    }
    s_set = -1; s_others = 0;
    for (i = 0; i < 256; i++) s_seen[i] = 0;
    if (!A->audio_listen(TAPE_RATE, s_line)) { s_mode = M_IDLE; say("No sound card that can listen answers.", A_RED); return; }
    tape_rx_begin(&s_rx, heard, 0);
    s_cur = -1; s_bad_was = 0;
    s_quiet_since = A->ms();
    s_verifying = verifying;
    s_began = A->ms(); s_nbumps = 0; s_shout = "";
    s_mode = M_READ;
    say("", 0);
}

static void read_end(int stopped)
{
    int i, same = 0, diff = 0, part = 0;
    A->audio_close();
    for (i = 0; i < s_nf; i++) {
        same += s_f[i].state == ST_SAME;
        diff += s_f[i].state == ST_DIFF;
        part += s_f[i].state == ST_PART;
    }
    s_mode = M_RESULT;
    wire("read", "same/different", (u32)same, (u32)diff);
    {   /* the summing-up: one of five, by how the tape came.  Stopping it by
         * hand says nothing about the tape, unless pieces were spoiled. */
        int whole = (same || diff) && !part, pick = (int)((A->ms() >> 3) % 5);
        s_outcome = whole ? s_good[pick] : (stopped && !s_rx.bad) ? "" : s_poor[pick];
        s_outcome_c = whole ? A_ON : A_RED;
        s_shout = "";
    }
    if (stopped && !same && !diff) say(part ? "Stopped part way. Nothing was judged." : "Stopped before anything was read.", A_CREAM);
    else if (!same && !diff && !part) say("Nothing of yours was heard.", A_RED);
    else if (part) say("Pieces missing: rewind, play, press R.", A_RED);
    else {
        say(diff ? "All read. Some differs from the floppy." : "All read: the same as the floppy.", A_ON);
        tune();
    }
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
    if (all_in()) read_end(0);
    else if (s_set >= 0 && now - s_quiet_since > 10000) read_end(0);   /* ten seconds of nothing: the tape is over */
}

/* Put what the tape has and the floppy has not, or has differently, on the floppy. */
static void put_back(void)
{
    static const char sig[] = "FLOPOS journal";
    static char why[80];
    int i, k, back = 0, failed = 0;
    for (i = 0; i < s_nf && !failed; i++) {
        File *f = &s_f[i];
        if (f->state != ST_DIFF) continue;
        /* The journal has to be a journal: FLOPOS writes to wherever it is. */
        if (f->sys && same_word(f->name, "JOURNAL.TXT")) {
            for (k = 0; sig[k] && f->tape[k] == (u8)sig[k]; k++) { }
            if (sig[k]) { f->state = ST_BAD; continue; }
        }
        if (f->sys) {
            if (!A->file_write(&f->f, f->tape)) { failed = 1; s_why = "The floppy would not take it."; break; }
        } else {
            /* Where it goes: where it came from, or the chosen folder.  Its
             * folders first, from the top down, then the file. */
            char path[INV_PATH], dir[INV_PATH];
            InvItem it;
            int bad = 0, cut = -1;
            if (s_dest < 0) A->format(path, INV_PATH, "%s", f->name);
            else {
                for (k = 0; f->name[k]; k++) if (f->name[k] == '/') cut = k;
                if (len_of(s_dirs[s_dest]) + len_of(f->name + cut + 1) + 2 > INV_PATH) { f->state = ST_BAD; continue; }
                A->format(path, INV_PATH, "%s%s%s", s_dirs[s_dest], s_dirs[s_dest][0] ? "/" : "", f->name + cut + 1);
            }
            for (k = 0; path[k] && !bad; k++) {
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
            if (s_dest < 0) { f->here = 1; f->size = f->tsize; }
        }
        f->state = ST_BACK;
        back++;
        wire("put back", f->name, f->used, 0);
    }
    if (failed) say(s_why, A_RED);
    else say(back ? "Put back on the floppy." : "Nothing needed putting back.", A_ON);
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
#define ROW_SET  3                       /* the settings start here         */
#define ROW_CAS  8                       /* the cassette                    */
#define ROW_LIST 20                      /* the files                       */

/* A setting with two values: the one in force in green. */
static int two(int x, int y, const char *key, const char *label, const char *a, const char *b, int second)
{
    char m[40];
    A->format(m, sizeof m, "\1%s\1 %s  ", key, label);
    x = text(x, y, m, A_CREAM);
    x = text(x, y, a, second ? A_OLIVE : A_ON) + 2;
    return text(x, y, b, second ? A_ON : A_OLIVE);
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
            case ST_SAME: A->format(m, cap, "same as the floppy's, accuracy %u%%", acc); *c = A_CREAM; break;
            case ST_DIFF: A->format(m, cap, "%s, accuracy %u%%", f->here ? "different" : "not on the floppy", acc); *c = A_GOLD; break;
            case ST_BACK: A->format(m, cap, "%s", "put back on the floppy"); *c = A_ON; break;
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
    text(2, 1, "Backup to cassette", A_RED);

    /* the settings */
    y = ROW_SET;
    x = text(2, y, "\1J\1 Job  ", A_CREAM);
    x = text(x, y, s_job_name[s_job], A_ON) + 1;
    text(x, y, s_job_what[s_job], A_OLIVE);
    x = two(2, y + 1, "C", "Compress (LZSS)", "on", "off", !s_packing);
    two(x + 4, y + 1, "F", "Speed", "normal", "fast", s_fast);
    x = two(2, y + 2, "V", "Verify after writing", "on", "off", !s_verify);
    x = text(x + 4, y + 2, "\1Tab\1 ", A_CREAM);
    text(x, y + 2, s_line ? "line-in socket active" : "microphone active", A_ON);
    x = text(2, y + 3, "\1D\1 Restore into  ", A_CREAM);
    if (s_dest < 0) text(x, y + 3, "where each file came from", A_ON);
    else { A->format(m, sizeof m, "%s/", s_dirs[s_dest]); text(x, y + 3, s_dest ? m : "the top of the inventory", A_ON); }

    /* the cassette, and how it is going */
    cassette(2, ROW_CAS, busy);
    y = ROW_CAS;
    if (s_mode == M_ARMED) {
        u32 secs = s_samples / TAPE_RATE + 1;
        A->format(m, sizeof m, "%u file%s, %u bytes: %u min %u s of tape.", (u32)s_count, s_count == 1 ? "" : "s", s_bytes, secs / 60, secs % 60);
        text(px, y, m, A_CREAM);
        text(px, y + 2, "Plug a cable from the headphone socket", A_CREAM);
        text(px, y + 3, "(front) into the recorder's MIC socket.", A_CREAM);
        text(px, y + 4, "Set it recording, then press \1Enter\1.", A_CREAM);
        text(px, y + 6, "Cassette Storage Legend (per tape side):", A_OLIVE);
        text(px, y + 7, "C30 = 15 min of data", A_OLIVE);
        text(px, y + 8, "C60 = 30 min of data", A_OLIVE);
        text(px, y + 9, "C90 = 45 min of data", A_OLIVE);
    } else if (s_mode == M_WRITE) {
        u32 heard_n = s_sent - (u32)A->audio_queued();
        u32 left = s_samples > heard_n ? (s_samples - heard_n) / TAPE_RATE : 0;
        u32 pc = heard_n >= s_samples ? 100 : heard_n / (s_samples / 100 + 1);
        text(px, y, "Writing: these tones are your files.", A_CREAM);
        bar(px, y + 2, pw, (int)(pc * (u32)pw / 100), A_RED);
        A->format(m, sizeof m, "%u%%", pc);
        text(px + pw + 2, y + 2, m, A_GOLD);
        A->format(m, sizeof m, "file %u of %u    %u:%02u left", (u32)(s_ord + 1 > s_count ? s_count : s_ord + 1), (u32)s_count, left / 60, left % 60);
        text(px, y + 3, m, A_CREAM);
        text(px, y + 6, "Cassette Storage Legend (per tape side):", A_OLIVE);
        text(px, y + 7, "C30 = 15 min of data", A_OLIVE);
        text(px, y + 8, "C60 = 30 min of data", A_OLIVE);
        text(px, y + 9, "C90 = 45 min of data", A_OLIVE);
    } else if (s_mode == M_READ) {
        int pc = read_percent();
        u32 all = s_rx.good + s_rx.bad;
        text(px, y, s_verifying ? "Written. Now the check, on the recorder:" : "Reading a tape. On the recorder:", A_CREAM);
        text(px, y + 1, s_verifying ? "press STOP, rewind, then press PLAY." : "rewind, then press PLAY.", A_GOLD);
        bar(px, y + 2, pw, pc * pw / 100, A_RED);
        A->format(m, sizeof m, "%u%%", (u32)pc);
        text(px + pw + 2, y + 2, m, A_GOLD);
        if (s_set >= 0) A->format(m, sizeof m, "accuracy %u%%   %u file%s   %s speed", all ? s_rx.good * 100 / all : 100, (u32)s_set_n, s_set_n == 1 ? "" : "s", s_rx.fast ? "fast" : "normal");
        else A->format(m, sizeof m, "%s", "Plug a cable from the recorder's EAR");
        text(px, y + 3, m, A_CREAM);
        if (s_set < 0) text(px, y + 4, s_line ? "socket into line-in (left: note, arrow)." : "socket into the microphone socket.", A_CREAM);
        bar(px, y + 8, pw, (s_rx.loud * pw + 63) / 127, A_ON);
        if (s_rx.loud < 4) A->format(m, sizeof m, "%s", "silence");
        else if (s_rx.tone) A->format(m, sizeof m, "%s", "a tape's tones");
        else A->format(m, sizeof m, "sound, not a tape's (%u waves a second)", s_rx.pitch);
        text(px, y + 9, m, s_rx.tone ? A_GOLD : A_OLIVE);
    } else {
        text(px, y, "\1W\1  write the ticked files to a tape", A_CREAM);
        text(px, y + 1, "\1R\1  read a tape, compare with the floppy", A_CREAM);
        if (s_mode == M_RESULT) {
            int diff = 0;
            for (i = 0; i < s_nf; i++) diff += s_f[i].state == ST_DIFF;
            if (diff) {
                text(px, y + 8, "\1Enter\1 puts the tape's copy of each", A_CREAM);
                text(px, y + 9, "different file on the floppy.", A_CREAM);
            } else if (s_others) text(px, y + 9, "(The tape holds another save as well.)", A_OLIVE);
        }
        if (s_big) { A->format(m, sizeof m, "(%u too long for a tape, left out.)", (u32)s_big); text(px, y + 6, m, A_OLIVE); }
    }
    if (*s_note && s_mode != M_READ && s_mode != M_WRITE && s_mode != M_ARMED) text(px, y + 3, s_note, s_note_c);

    /* the files */
    rows = H - 7 - ROW_LIST;
    if (s_cursor < s_top) s_top = s_cursor;
    if (s_cursor >= s_top + rows) s_top = s_cursor - rows + 1;
    if (s_mode == M_WRITE && s_at < s_nf) {       /* follow the file in hand */
        for (i = 0; i < s_nrows; i++) if (s_rows[i] == 1000 + s_at) { s_top = i < rows / 2 ? 0 : i - rows / 2; break; }
    } else if (s_mode == M_READ && s_cur >= 0) {
        for (i = 0; i < s_nrows; i++) if (s_rows[i] == 1000 + s_cur) { s_top = i < rows / 2 ? 0 : i - rows / 2; break; }
    }
    for (i = 1; i < W - 1; i++) A->win_put(i, ROW_LIST - 1, A_FR_H, A_EMBER, A_PAPER, 0);
    for (i = 0; i < rows && s_top + i < s_nrows; i++) {
        int r = s_rows[s_top + i], here = s_top + i == s_cursor && !busy;
        u8 bg = here ? A_GREEN_D : A_PAPER, c;
        y = ROW_LIST + i;
        if (here) A->win_band(y, 1, W - 2, A_GREEN_D);
        if (r < 1000) {
            int t = folder_tick(r);
            A->win_text(2, y, t == 1 ? "[x]" : t == 2 ? "[-]" : "[ ]", t ? A_ON : A_OLIVE, bg);
            if (r == -1) A->format(m, sizeof m, "%s", "FLOPOS's own records: bookmarks, journal, settings");
            else if (r == -2) A->format(m, sizeof m, "%s", "On the tape, not on this floppy");
            else A->format(m, sizeof m, "%s/", s_dirs[r]);
            A->win_text(6, y, m, A_GOLD, bg);
        } else {
            const File *f = &s_f[r - 1000];
            const char *name = f->name;
            int k, cut = -1, indent = f->dir != 0 ? 2 : 0;
            for (k = 0; name[k]; k++) if (name[k] == '/') cut = k;
            if (f->dir != -2) name += cut + 1;
            else if (k > 24) name += k - 24;
            A->win_text(2 + indent, y, f->on ? "[x]" : "[ ]", f->on ? A_ON : A_OLIVE, bg);
            A->win_text(6 + indent, y, name, f->on ? A_CREAM : A_OLIVE, bg);
            file_state(f, r - 1000, m, sizeof m, &c);
            A->win_text(W - 2 - len_of(m), y, m, c, bg);
        }
    }

    /* the job list, open over the settings */
    if (s_menu) {
        for (i = 0; i < 2; i++) {
            u8 bg = s_menu - 1 == i ? A_EMBER : A_GREEN_D;
            A->win_band(ROW_SET + 1 + i, 8, W - 3, bg);
            A->win_text(9, ROW_SET + 1 + i, s_job_name[i], s_menu - 1 == i ? A_CREAM : s_job == i ? A_ON : A_GOLD, bg);
            A->win_text(27, ROW_SET + 1 + i, s_job_what[i], A_CREAM, bg);
        }
    }

    /* how it is going */
    {
        u8 c;
        const char *t = mood_line(&c);
        char line[80];
        int row = H - 6, wide = W - 4 < 78 ? W - 4 : 78, n, k, cut;
        while (*t && row <= H - 4 && !s_menu) {  /* up to three lines, broken between words */
            for (n = 0; t[n] && n < wide; n++) { }
            cut = n;
            if (t[n]) { for (cut = n; cut > 0 && t[cut] != ' '; cut--) { } if (!cut) cut = n; }
            for (k = 0; k < cut; k++) line[k] = t[k];
            line[cut] = 0;
            text(2, row++, line, c);
            t += cut;
            while (*t == ' ') t++;
        }
    }

    /* the keys */
    if (s_menu) text(2, H - 2, "\1Up\1 \1Down\1 choose   \1Enter\1 take it   \1Esc\1 close", A_OLIVE);
    else if (busy) text(2, H - 2, s_mode == M_READ ? "\1S\1 stop   \1Tab\1 the other socket" : "\1S\1 stop", A_OLIVE);
    else if (s_mode == M_ARMED) text(2, H - 2, "\1Enter\1 start   \1Esc\1 not now", A_OLIVE);
    else text(2, H - 2, "\1W\1 write  \1R\1 read  \1Space\1 tick  \1Up\1 \1Down\1 move  \1Esc\1 leave", A_OLIVE);
}

/* ---- the app ---------------------------------------------------------------- */
int app_main(const Api *api, int argc, char **argv)
{
    u32 drawn = 0;
    int key, mods, i, a, start = 0;

    A = api;
    if (argc >= 2) {
        if (same_word(argv[1], "save")) start = 1;
        else if (same_word(argv[1], "load")) start = 2;
        else {
            static const char *const help[] = {
                "maelcum            the backup screen",
                "maelcum save       ready to write your files to a tape",
                "maelcum load       listen to a tape and compare it",
                "maelcum save NAME  only the file called NAME",
                "maelcum load NAME  only the file called NAME",
            };
            A->colour(A_CREAM);
            for (i = 0; i < 5; i++) { A->print(help[i]); A->newline(); }
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

        if (s_menu) {                            /* the job list is open */
            if (key == K_UP || key == K_DOWN) s_menu = 3 - s_menu;
            else if (key == K_ENTER) { s_job = s_menu - 1; if (!s_job) tick_all(); s_menu = 0; }
            else if (key == K_ESC || key == 'J') s_menu = 0;
            continue;
        }
        if (s_mode == M_WRITE) {
            if (key == 'S' || key == K_ESC) {
                A->audio_close();
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
                A->audio_close();
                if (!A->audio_listen(TAPE_RATE, s_line)) { s_mode = M_IDLE; say("No sound card that can listen answers.", A_RED); }
                else { tape_rx_begin(&s_rx, heard, 0); s_cur = -1; s_bad_was = 0; s_quiet_since = now; }
            }
            continue;
        }
        if (s_mode == M_ARMED) {
            if (key == K_ENTER) write_go();
            else if (key == K_ESC) s_mode = M_IDLE;
            continue;
        }
        /* idle, or looking at what a tape held */
        if (key == K_ESC) break;
        else if (key == 'W') write_arm();
        else if (key == 'R') read_go(0);
        else if (key == K_ENTER && s_mode == M_RESULT) put_back();
        else if (key == 'J') s_menu = 1 + s_job;
        else if (key == 'C') s_packing = !s_packing;
        else if (key == 'F') s_fast = !s_fast;
        else if (key == 'V') { s_verify = !s_verify; s_shout = s_verify ? "If it's NICE - Spin it TWICE, SELECTA! :-D" : ""; }
        else if (key == K_TAB) s_line = !s_line;
        else if (key == 'D') { if (++s_dest >= s_ndirs) s_dest = -1; }
        else if (key == K_UP) { if (s_cursor > 0) s_cursor--; }
        else if (key == K_DOWN) { if (s_cursor + 1 < s_nrows) s_cursor++; }
        else if (key == K_PGUP) { s_cursor = s_cursor > 10 ? s_cursor - 10 : 0; }
        else if (key == K_PGDN) { s_cursor = s_cursor + 10 < s_nrows ? s_cursor + 10 : s_nrows - 1; }
        else if (key == ' ' && s_nrows) tick_row(s_cursor);
    }
    A->win_close();
    return 0;
}
