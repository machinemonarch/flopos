/* ===========================================================================
 * tape.c -- your files, kept on an audio cassette.
 *
 * TAPE.APP on the floppy.  `tape save` plays the files out of the headphone
 * socket as two tones, for a cassette recorder to record; `tape load`
 * listens to the recorder playing them back, compares what it hears with
 * the floppy, and puts it back if asked.  The sound itself is apps/lib/tape.
 *
 * Only the files that are yours go to tape or come back from it: the eight
 * documents, the bookmarks, the journal and what FLOPOS has been taught.
 * Nothing a tape says can touch any other file.
 *
 * On the tape, a save is a long leader and then each file in turn:
 *
 *      HEAD (twice)   its name, full size, how much of it is in use, the
 *                     byte the rest is filled with, which file of how many
 *                     this is, a check of the whole, and this save's number
 *      DATA ...       the part in use, 256 bytes a block; the block's
 *                     number carries which file it belongs to
 *      END  (twice)
 *
 * Files keep their size on the floppy and are mostly empty filling, so
 * only the part in use is sent.  A block that is spoiled is simply missing:
 * the tape can be played again and only the gaps are filled.
 * ===========================================================================
 */
#include "app.h"
#include "tape.h"

#define FILE_MAX 16384                   /* the largest file kept          */
#define PIECES   (FILE_MAX / TAPE_PIECE)
#define MAXF     11
#define HEAD_LEN 28
#define WW       53                      /* the window, in characters      */
#define HH       23
#define ROW0     9                       /* where the list of files starts */

enum { ST_NONE, ST_PART, ST_SAME, ST_DIFF, ST_BACK, ST_BAD };

typedef struct {
    char name[TAPE_NAME];
    AppFile f;
    int  on;                             /* asked for                      */
    u32  used;                           /* bytes before the filling       */
    u8   pad;                            /* the filling                    */
    u16  crc;                            /* of the part in use             */
    int  pieces, got;
    int  state;
    u8   have[PIECES];
    u8   disk[FILE_MAX];                 /* as it is on the floppy         */
    u8   tape[FILE_MAX];                 /* as the tape gave it            */
} File;

static const Api *A;
static File s_f[MAXF];
static int  s_nf;

static const char *const s_names[MAXF] = {
    "WRITE1.MD", "WRITE2.MD", "WRITE3.MD", "WRITE4.MD", "WRITE5.MD", "WRITE6.MD",
    "WRITE7.MD", "WRITE8.MD", "BOOKMARK.TXT", "JOURNAL.TXT", "STATE.BIN"
};

/* ---- small things ----------------------------------------------------------- */
static int up(int c) { return c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c; }

static int same_word(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) if (up(*a) != up(*b)) return 0;
    return !*a && !*b;
}

static void put(int y, const char *t, u8 fg) { A->win_text(2, y, t, fg, A_PAPER); }

static void frame(const char *head)
{
    A->win_open(WW, HH);
    A->win_frame("tape", A_GREY);
    put(1, head, A_BRIGHT);
}

static void bar(int x, int y, int w, int fill)
{
    int i;
    for (i = 0; i < w; i++)
        A->win_put(x + i, y, i < fill ? A_CH_FILL : '.', i < fill ? A_BRIGHT : A_DIM, A_PAPER, 0);
}

static int wait_key(void)
{
    int key, mods;
    do { key = A->key(&mods); if (key < 0) A->pump(0); } while (key < 0);
    return key;
}

/* The files that are yours and on this floppy. */
static void find_files(void)
{
    int i, k;
    s_nf = 0;
    for (i = 0; i < MAXF; i++) {
        File *f = &s_f[s_nf];
        if (!A->file_find(s_names[i], &f->f) || f->f.size == 0 || f->f.size > FILE_MAX) continue;
        for (k = 0; s_names[i][k]; k++) f->name[k] = s_names[i][k];
        f->name[k] = 0;
        s_nf++;
    }
}

/* The names after `tape save` or `tape load`: only those, or all with none. */
static int choose(int argc, char **argv)
{
    int a, i, n = 0;
    char m[80];
    for (i = 0; i < s_nf; i++) s_f[i].on = argc <= 2;
    for (a = 2; a < argc; a++) {
        for (i = 0; i < s_nf && !same_word(argv[a], s_f[i].name); i++) { }
        if (i == s_nf) {
            A->format(m, sizeof m, "tape does not keep %s. It keeps:", argv[a]);
            A->colour(A_TEXT); A->print(m); A->newline();
            A->colour(A_DIM);
            for (i = 0; i < s_nf; i++) { A->print(i ? " " : "  "); A->print(s_f[i].name); }
            A->newline();
            return 0;
        }
        s_f[i].on = 1;
    }
    for (i = 0; i < s_nf; i++) n += s_f[i].on;
    return n;
}

/* A file from the floppy, and how much of it is in use: everything before
 * the run of one repeated byte it ends with. */
static int from_floppy(File *f)
{
    u32 n = f->f.size;
    if (A->file_read(&f->f, f->disk, FILE_MAX) != (int)n) return 0;
    f->pad = f->disk[n - 1];
    while (n > 0 && f->disk[n - 1] == f->pad) n--;
    f->used = n;
    f->crc = tape_crc(f->disk, (int)n);
    f->pieces = (int)((n + TAPE_PIECE - 1) / TAPE_PIECE);
    return 1;
}

static void list(int load, int now)
{
    int i;
    char m[48];
    for (i = 0; i < s_nf; i++) {
        File *f = &s_f[i];
        u8 c = A_DIM;
        m[0] = 0;
        if (!load) {
            if (!f->on) { }
            else if (now > i) { A->format(m, sizeof m, "%s", "on the tape"); c = A_TEXT; }
            else if (now == i) { A->format(m, sizeof m, "%u of %u pieces", (u32)f->got, (u32)f->pieces); c = A_BRIGHT; }
            else A->format(m, sizeof m, "%u bytes", f->used);
        } else switch (f->state) {
            case ST_PART: A->format(m, sizeof m, "%u of %u pieces", (u32)f->got, (u32)f->pieces); c = A_BRIGHT; break;
            case ST_SAME: A->format(m, sizeof m, "%s", "whole: the same as on the floppy"); c = A_TEXT; break;
            case ST_DIFF: A->format(m, sizeof m, "%s", "whole: different from the floppy"); c = A_ACCENT; break;
            case ST_BACK: A->format(m, sizeof m, "%s", "put back on the floppy"); c = A_BRIGHT; break;
            case ST_BAD:  A->format(m, sizeof m, "%s", "not a journal: left alone"); c = A_ACCENT; break;
            default: break;
        }
        A->win_text(2, ROW0 + i, f->name, f->on && (!load || f->state) ? A_TEXT : A_DIM, A_PAPER);
        A->win_text(16, ROW0 + i, m, c, A_PAPER);
    }
}

/* Down the serial cable, for whoever is testing: a word and two numbers. */
static void wire(const char *what, const char *name, u32 a, u32 b)
{
    char m[100];
    A->format(m, sizeof m, "[tape] %s %s %u %u\n", what, name, a, b);
    A->wire(m);
}

/* ---- saving ----------------------------------------------------------------- */
static int s_at, s_stage, s_ord, s_count;    /* the file, where in it, which of how many */
static u16 s_save_id;

/* How long a block lasts on the tape, in samples. */
static u32 block_samples(int len, int lead_ms)
{
    u32 bits = (u32)lead_ms * 1200 / 1000 + 8 + (u32)(9 + len) * 10;
    return bits * (TAPE_RATE / 150) / 8;
}

/* Queue the next block of the save, or say there is none. */
static int next_block(TapeTx *t)
{
    u8 h[HEAD_LEN];
    File *f;
    int k;

    while (s_at < s_nf && !s_f[s_at].on) s_at++;
    if (s_at >= s_nf) return 0;
    f = &s_f[s_at];
    if (s_stage < 2) {                           /* its name and size, twice */
        for (k = 0; k < TAPE_NAME; k++) h[k] = (u8)f->name[k];
        h[13] = (u8)f->f.size; h[14] = (u8)(f->f.size >> 8); h[15] = (u8)(f->f.size >> 16); h[16] = (u8)(f->f.size >> 24);
        h[17] = (u8)f->used;   h[18] = (u8)(f->used >> 8);   h[19] = (u8)(f->used >> 16);   h[20] = (u8)(f->used >> 24);
        h[21] = f->pad; h[22] = (u8)s_ord; h[23] = (u8)s_count;
        h[24] = (u8)f->crc; h[25] = (u8)(f->crc >> 8);
        h[26] = (u8)s_save_id; h[27] = (u8)(s_save_id >> 8);
        tape_tx_block(t, TAPE_HEAD, s_ord << 8, h, HEAD_LEN, s_stage ? 100 : (s_ord ? 1000 : 8000));
        s_stage++;
        f->got = 0;
    } else if (f->got < f->pieces) {             /* the part in use        */
        u32 off = (u32)f->got * TAPE_PIECE;
        int len = f->used - off > TAPE_PIECE ? TAPE_PIECE : (int)(f->used - off);
        tape_tx_block(t, TAPE_DATA, (s_ord << 8) | f->got, f->disk + off, len, 100);
        f->got++;
    } else {                                     /* and its end, twice     */
        tape_tx_block(t, TAPE_END, (s_ord << 8) | f->pieces, h, 0, 100);
        if (++s_stage == 4) { s_stage = 0; s_ord++; s_at++; }
    }
    return 1;
}

static void save(void)
{
    static u8 buf[1024];
    TapeTx tx;
    u32 bytes = 0, samples = 0, sent = 0, drawn = 0, secs, ended = 0;
    int i, n, key, mods, playing = 0, done = 0, last = 0;
    char m[64];

    frame("Saving to a cassette");
    put(3, "Reading your files from the floppy...", A_DIM);
    A->pump(1);
    s_count = 0;
    for (i = 0; i < s_nf; i++) {
        File *f = &s_f[i];
        if (!f->on) continue;
        if (!from_floppy(f)) { f->on = 0; continue; }
        samples += block_samples(HEAD_LEN, s_count ? 1000 : 8000) + block_samples(HEAD_LEN, 100) + 2 * block_samples(0, 100);
        for (n = 0; n < f->pieces; n++) {
            u32 left = f->used - (u32)n * TAPE_PIECE;
            samples += block_samples(left > TAPE_PIECE ? TAPE_PIECE : (int)left, 100);
        }
        bytes += f->used;
        s_count++;
    }
    if (!s_count) {
        frame("Saving to a cassette");
        put(3, "The floppy would not give up the files.", A_ACCENT);
        put(HH - 2, "\1Esc\1 leaves", A_DIM);
        wait_key();
        return;
    }
    secs = samples / TAPE_RATE + 1;
    s_at = s_stage = s_ord = 0;
    s_save_id = (u16)(A->ms() * 2654435761u >> 16);

    for (;;) {
        u32 now = A->ms();
        if (now - drawn >= 200 || !drawn) {
            drawn = now ? now : 1;
            frame("Saving to a cassette");
            if (!playing && !done) {
                A->format(m, sizeof m, "%u file%s, %u bytes: %u min %u s of tape.", (u32)s_count, s_count == 1 ? "" : "s",
                          bytes, secs / 60, secs % 60);
                put(3, m, A_TEXT);
                put(5, "A cable from the headphone socket (the front)", A_TEXT);
                put(6, "to the recorder's MIC socket. Set the recorder", A_TEXT);
                put(7, "recording, then press \1Enter\1.", A_TEXT);
                put(HH - 2, "\1Enter\1 start   \1Esc\1 leave", A_DIM);
            } else if (playing) {
                u32 heard = sent - (u32)A->audio_queued();
                u32 left = samples > heard ? (samples - heard) / TAPE_RATE : 0;
                put(3, "The tones you hear are your files.", A_TEXT);
                bar(2, 5, 34, (int)(heard / (samples / 34 + 1)));
                A->format(m, sizeof m, "%u:%02u left", left / 60, left % 60);
                A->win_text(38, 5, m, A_TEXT, A_PAPER);
                put(HH - 2, "\1Esc\1 stop", A_DIM);
            } else {
                put(3, done == 1 ? "All of it is on the tape. Stop the recorder." : "Stopped. The tape holds only part of it.", done == 1 ? A_BRIGHT : A_ACCENT);
                if (done == 1) {
                    put(5, "To be sure of it: rewind, and type", A_TEXT);
                    put(6, "tape load", A_BRIGHT);
                }
                put(HH - 2, "\1Esc\1 leave", A_DIM);
            }
            list(0, playing ? s_at : done == 1 ? s_nf : -1);
        }

        if (playing) {
            /* A second in hand, so a slow moment never leaves a gap in a block. */
            while (A->audio_queued() < TAPE_RATE) {
                if (!tape_tx_busy(&tx) && !next_block(&tx)) { if (!last) last = 1; break; }
                n = tape_tx_samples(&tx, buf, (int)sizeof buf);
                sent += (u32)A->audio_write(buf, n);
            }
            if (last == 1 && A->audio_queued() == 0) { last = 2; ended = now; }
            else if (last == 2 && now - ended > 1200) {      /* the card's own loop has emptied too */
                A->audio_close();
                playing = 0; done = 1; drawn = 0;
                wire("saved", "files", (u32)s_count, bytes);
            }
        }

        key = A->key(&mods);
        if (key < 0) { A->pump(0); continue; }
        drawn = 0;
        if (key == K_ESC) {
            if (playing) { A->audio_close(); playing = 0; done = 2; wire("stopped", "saving", 0, 0); }
            else return;
        } else if (key == K_ENTER && !playing && !done) {
            if (!A->audio_open(TAPE_RATE)) {
                frame("Saving to a cassette");
                put(3, "No sound card that plays samples answers.", A_ACCENT);
                put(HH - 2, "\1Esc\1 leaves", A_DIM);
                wait_key();
                return;
            }
            tape_tx_begin(&tx);
            playing = 1; sent = 0; last = 0;
            wire("saving", "files", (u32)s_count, secs);
        }
    }
}

/* ---- loading ---------------------------------------------------------------- */
static TapeRx s_rx;
static int s_cur, s_cur_ord;             /* the file the tape is in, or -1  */
static int s_set, s_set_n;               /* which save this is; how many files it has */
static int s_seen[256];                  /* by place in the save: 0 not heard, 1 + the file, or -1 not wanted */
static int s_others;                     /* another save was heard too      */

/* Every piece of a file has come: is it right, and is it what the floppy has? */
static void settle(File *f)
{
    u32 i;
    if (tape_crc(f->tape, (int)f->used) != f->crc) {     /* pieces of two saves, mixed */
        f->got = 0;
        for (i = 0; i < PIECES; i++) f->have[i] = 0;
        f->state = ST_PART;
        return;
    }
    for (i = f->used; i < f->f.size; i++) f->tape[i] = f->pad;
    for (i = 0; i < f->f.size && f->tape[i] == f->disk[i]; i++) { }
    f->state = i == f->f.size ? ST_SAME : ST_DIFF;
    wire(f->state == ST_SAME ? "same" : "different", f->name, f->used, 0);
}

static void heard(void *ctx, int type, int seq, const u8 *d, int len)
{
    File *f;
    (void)ctx;
    if (type == TAPE_HEAD) {
        u32 size, used;
        int i, k, ord, count, set;
        u16 crc;
        s_cur = -1;
        if (len != HEAD_LEN || d[TAPE_NAME - 1]) return;
        size = (u32)d[13] | (u32)d[14] << 8 | (u32)d[15] << 16 | (u32)d[16] << 24;
        used = (u32)d[17] | (u32)d[18] << 8 | (u32)d[19] << 16 | (u32)d[20] << 24;
        ord = d[22]; count = d[23];
        crc = (u16)(d[24] | d[25] << 8);
        set = d[26] | d[27] << 8;
        if (!count || ord >= count || (seq >> 8) != ord) return;
        if (s_set < 0) { s_set = set; s_set_n = count; }
        if (set != s_set || count != s_set_n) { s_others = 1; return; }
        for (i = 0; i < s_nf; i++) {
            for (k = 0; k < TAPE_NAME && s_f[i].name[k] == (char)d[k] && d[k]; k++) { }
            if (k < TAPE_NAME && !s_f[i].name[k] && !d[k]) break;
        }
        /* Not one of yours, not wanted, or not the size this floppy keeps it at. */
        if (i == s_nf || !s_f[i].on || size != s_f[i].f.size || used > size) { s_seen[ord] = -1; return; }
        f = &s_f[i];
        if (f->state == ST_NONE || f->used != used || f->crc != crc || f->pad != d[21]) {
            f->used = used; f->crc = crc; f->pad = d[21];
            f->pieces = (int)((used + TAPE_PIECE - 1) / TAPE_PIECE);
            f->got = 0;
            for (k = 0; k < PIECES; k++) f->have[k] = 0;
            f->state = ST_PART;
            if (!f->pieces) settle(f);
        }
        s_seen[ord] = 1 + i;
        s_cur = i; s_cur_ord = ord;
    } else if (type == TAPE_DATA) {
        int p = seq & 0xFF, want, k;
        if (s_cur < 0 || (seq >> 8) != s_cur_ord) return;
        f = &s_f[s_cur];
        if (f->state != ST_PART || p >= f->pieces || f->have[p]) return;
        want = p == f->pieces - 1 ? (int)(f->used - (u32)p * TAPE_PIECE) : TAPE_PIECE;
        if (len != want) return;
        for (k = 0; k < len; k++) f->tape[p * TAPE_PIECE + k] = d[k];
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

/* Listen until everything is in or esc.  0: no card to listen with. */
static int listen(void)
{
    static u8 buf[2048];
    u32 drawn = 0, told = 0;
    int n, key, mods, line = 1, rate;
    char m[64];

    rate = A->audio_listen(TAPE_RATE, line);
    if (!rate) return 0;
    tape_rx_begin(&s_rx, heard, 0);
    s_cur = -1;
    for (;;) {
        u32 now = A->ms();
        while ((n = A->audio_heard(buf, (int)sizeof buf)) > 0) tape_rx_samples(&s_rx, buf, n);
        if (all_in()) break;
        if (now - told >= 5000) {
            told = now;
            wire(s_rx.tone ? "tones" : "listening", "level", (u32)s_rx.loud, 0);
            wire("pieces", "good/spoiled", s_rx.good, s_rx.bad);
        }
        if (now - drawn >= 100) {
            drawn = now;
            frame("Reading a cassette");
            put(3, "A cable from the recorder's EAR socket to the", A_TEXT);
            put(4, line ? "line-in socket (left side, the note and arrow)." : "microphone socket (left side).", A_TEXT);
            put(5, "Rewind the tape, and set the recorder playing.", A_TEXT);
            bar(2, 7, 24, s_rx.loud * 24 / 127);
            A->win_text(28, 7, s_rx.loud < 6 ? "silence" : s_rx.tone ? "a tape's tones" : "sound, not a tape's",
                        s_rx.tone ? A_BRIGHT : A_DIM, A_PAPER);
            if (s_rx.bad) {
                A->format(m, sizeof m, "%u spoiled", s_rx.bad);
                A->win_text(40, 8, m, A_DIM, A_PAPER);
            }
            list(1, 0);
            put(HH - 2, line ? "\1Tab\1 the microphone socket   \1Esc\1 enough" : "\1Tab\1 the line-in socket   \1Esc\1 enough", A_DIM);
        }
        key = A->key(&mods);
        if (key < 0) { A->pump(0); continue; }
        drawn = 0;
        if (key == K_ESC) break;
        if (key == K_TAB) {
            line = !line;
            A->audio_close();
            if (!A->audio_listen(TAPE_RATE, line)) return 0;
            tape_rx_begin(&s_rx, heard, 0);
            s_cur = -1;
        }
    }
    A->audio_close();
    return 1;
}

static void load(void)
{
    int i, key, k, same, diff, part, back = 0, failed = 0;
    char m[64];

    frame("Reading a cassette");
    put(3, "Reading your files from the floppy first...", A_DIM);
    A->pump(1);
    for (i = 0; i < s_nf; i++) {
        File *f = &s_f[i];
        f->state = ST_NONE;
        if (f->on && !from_floppy(f)) f->on = 0;
    }
    s_set = -1; s_others = 0;
    for (i = 0; i < 256; i++) s_seen[i] = 0;

    for (;;) {
        if (!listen()) {
            frame("Reading a cassette");
            put(3, "No sound card that can listen answers.", A_ACCENT);
            put(HH - 2, "\1Esc\1 leaves", A_DIM);
            wait_key();
            return;
        }
        for (;;) {
            same = diff = part = 0;
            for (i = 0; i < s_nf; i++) {
                same += s_f[i].state == ST_SAME;
                diff += s_f[i].state == ST_DIFF;
                part += s_f[i].state == ST_PART;
            }
            frame("What the tape holds");
            if (failed) put(3, "The floppy would not take it.", A_ACCENT);
            else if (back) {
                A->format(m, sizeof m, "%u put back on the floppy.", (u32)back);
                put(3, m, A_BRIGHT);
            } else if (!same && !diff && !part) put(3, "Nothing of yours was heard.", A_TEXT);
            else {
                A->format(m, sizeof m, "%u the same as the floppy, %u different.", (u32)same, (u32)diff);
                put(3, m, A_TEXT);
            }
            if (part) {
                A->format(m, sizeof m, "%u with pieces missing: rewind, play it again", (u32)part);
                put(4, m, A_TEXT);
                put(5, "and \1Space\1 fills in the gaps.", A_TEXT);
            } else if (s_others) put(4, "(The tape has another save on it as well.)", A_DIM);
            if (diff) {
                put(6, "\1Enter\1 puts the tape's copy of each different", A_TEXT);
                put(7, "file on the floppy, over what is there now.", A_TEXT);
            }
            list(1, 0);
            put(HH - 2, diff ? "\1Enter\1 put back  \1Space\1 listen again  \1Esc\1 leave" : "\1Space\1 listen again   \1Esc\1 leave", A_DIM);
            key = wait_key();
            if (key == K_ESC) return;
            if (key == ' ') break;
            if (key == K_ENTER && diff) {
                put(3, "Writing to the floppy...                    ", A_DIM);
                A->pump(1);
                for (i = 0; i < s_nf; i++) {
                    File *f = &s_f[i];
                    static const char sig[] = "FLOPOS journal";
                    if (f->state != ST_DIFF) continue;
                    /* The journal has to be a journal: FLOPOS writes to wherever it is. */
                    if (same_word(f->name, "JOURNAL.TXT")) {
                        for (k = 0; sig[k] && f->tape[k] == (u8)sig[k]; k++) { }
                        if (sig[k]) { f->state = ST_BAD; continue; }
                    }
                    if (!A->file_write(&f->f, f->tape)) { failed = 1; break; }
                    for (k = 0; k < (int)f->f.size; k++) f->disk[k] = f->tape[k];
                    f->state = ST_BACK;
                    back++;
                    wire("put back", f->name, f->used, 0);
                }
            }
        }
    }
}

/* ---- the app ---------------------------------------------------------------- */
int app_main(const Api *api, int argc, char **argv)
{
    int what = 0, key;

    A = api;
    find_files();
    if (!s_nf) {
        A->colour(A_TEXT); A->print("None of your files are on this floppy."); A->newline();
        return 0;
    }
    if (argc >= 2) {
        if (same_word(argv[1], "save")) what = 1;
        else if (same_word(argv[1], "load")) what = 2;
        else {
            A->colour(A_TEXT); A->print("tape save, or tape load. A file's name after it keeps to that file."); A->newline();
            return 0;
        }
    }
    if (!choose(argc, argv)) return 0;
    while (!what) {
        frame("Your files, kept on an audio cassette");
        put(3, "The documents, the bookmarks, the journal and", A_TEXT);
        put(4, "the words you have taught.", A_TEXT);
        put(7, "\1S\1    save them to a tape", A_TEXT);
        put(9, "\1L\1    load a tape, and compare it with the floppy", A_TEXT);
        put(11, "\1Esc\1  leave", A_TEXT);
        key = wait_key();
        if (key == K_ESC) { A->win_close(); return 0; }
        if (key == 's' || key == 'S') what = 1;
        if (key == 'l' || key == 'L') what = 2;
    }
    if (what == 1) save(); else load();
    A->win_close();
    return 0;
}
