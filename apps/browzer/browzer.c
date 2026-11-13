/* ===========================================================================
 * browzer.c -- a small web browser, and the app it is.
 *
 * BROWZER.APP on the floppy.  Typed as `browzer`, it is the browser; typed
 * as `browzer fetch address`, it reads one page out into the terminal as
 * text and stops.
 *
 * The whole screen, framed: an address along the top, the page in the
 * middle, a status line along the bottom.  Pages come through the same
 * reader `browzer fetch` uses, but into a Page that remembers which characters
 * belong to which link, so they can be scrolled through and followed.
 *
 *   up / down        scroll a line         pgup / pgdn / space   a screen
 *   tab / shift-tab  next / previous link  enter                 follow it
 *   backspace, left  back                  g                     go somewhere
 *   r                again                 h                     home
 *   esc              leave
 *
 * Forms: tab reaches the boxes and buttons as it does links.  Enter on a
 * box starts typing into it; enter again sends the form (in a big box,
 * enter is a new line and tab finishes).  Enter on a button sends the
 * form with that button.  Esc stops typing.
 *
 * Typing words rather than an address searches, through FrogFind -- a
 * search engine made for old computers that answers in plain HTML over
 * plain http, which is exactly what this one can best read.
 * ===========================================================================
 */
#include "web.h"
#include "tracker.h"
#include "decode.h"

#define MAX_LINES  1500
#define MAX_WIDTH  98
#define HISTORY    20

const Api *A;

/* Everything a page needs, kept here in the app's own memory. */
static Page      s_pg;
static PageCell  s_cells[MAX_LINES * MAX_WIDTH];
static char      s_hrefs[PAGE_LINKS][PAGE_HREF_MAX];
static PageForm  s_forms[PAGE_FORMS];
static PageField s_fields[PAGE_FIELDS];
static char      s_pool[PAGE_POOL];
static u16       s_link_field[PAGE_LINKS + 1];
static PageOption s_opts[PAGE_OPTIONS];
static u8        s_body[PAGE_POOL * 3];  /* a form, ready to send          */
static int  s_ready;
static int  s_typing = -1;               /* the field being typed into     */
static int  s_choosing = -1, s_chose_was;   /* a list being chosen from   */

/* ---- music ------------------------------------------------------------------
 * A link that is not a page comes here instead.  Two ways:
 *
 *   A file of 2 MB or less is downloaded whole, then looked at.  A song
 *   (.mod, .xm) or a recording (mp3, AAC, Ogg, FLAC, wav) plays in the
 *   background: the sound card's queue is topped up whenever browzer
 *   waits for anything, so it goes on while you browse, until it ends or
 *   s stops it.  A playlist (.m3u, .pls) is followed to the stream it
 *   names.
 *
 *   Radio, and recordings too big to keep, are played as they arrive:
 *   the player screen shows the station and what it says is on, and esc
 *   stops it.  The bytes come into a buffer of up to a megabyte, and
 *   playing starts once about three seconds are in hand.  A station that
 *   hangs up is called again.
 *
 * The decoding is not done here in browzer's own time.  feed() is handed
 * to the system, whose timer calls it at interrupt time whenever the
 * sound card's queue has room -- the way the Mac's Sound Manager asked
 * its players for more -- so the music goes on whatever browzer is busy
 * with: drawing, waiting on the network, reading a page.  browzer only
 * brings the bytes in.  The two share s_in and the decoder; browzer
 * changes them only with s_lock set, and feed() leaves them alone then.
 * (feed() can interrupt browzer; browzer never interrupts feed().) */
#define DL_MAX (2u << 20)
#define ST_MAX (1u << 20)
static u8   s_dl[DL_MAX];                /* a whole file, or a stream's bytes */
static u32  s_dl_len;
enum { DL_NONE, DL_COMING, DL_CAME, DL_BROKE, DL_BIG, DL_STREAM, DL_STREAM_END, DL_UNPLAYABLE, DL_NOCARD };
static int  s_dl_state;
static int  s_esc;                       /* esc stopped a stream            */
static char s_dl_type[48], s_dl_url[PAGE_HREF_MAX + 16], s_station[48], s_now[100], s_title[64];
static int  s_music;                     /* 0 quiet, 1 playing, 2 the end is queued */
static int  s_src;                       /* 1 a song, 2 a recording         */
static int  s_rate;                      /* the card's                      */
static u32  s_written;                   /* samples given to the card       */
static Decoder   s_dec;
static AudioIn   s_in;
static Resampler s_rs;
static u32  s_metaint, s_meta_count;     /* radio: song titles in the stream */
static int  s_meta_len, s_meta_got;
static char s_meta[512];
static u32  s_drawn;
static int  s_buffering;                 /* gathering a cushion before playing */
static int  s_rate_in;                   /* the recording's rate, as playing */
static int  s_live, s_redial, s_redials; /* radio: called again if it hangs up */
static u32  s_conn_ms;                   /* when this stream's call began   */
static volatile int s_lock;              /* browzer is changing what feed() reads */
static int  s_answered;                  /* a server answered with sound    */
static volatile int s_feeding_ok;        /* a decoder is ready for feed()   */
static volatile u32 s_ran_dry;           /* times the sound ran out mid-stream */
static volatile u32 s_cushion_x = 1;     /* cushions of three seconds to gather */

static void music_stop(void)
{
    s_lock = 1;
    A->audio_feeder(0);
    if (s_music) A->audio_close();
    s_music = 0;
    s_feeding_ok = 0;
    s_lock = 0;
}

static u32 heard_seconds(void)
{
    u32 behind = (u32)A->audio_queued() + 16384;
    return s_rate ? (s_written > behind ? s_written - behind : 0) / (u32)s_rate : 0;
}

/* How much of the recording to have in hand, in bytes, before playing:
 * three seconds at what it says its rate is, times x. */
static u32 cushion(u32 x)
{
    u32 kbps = s_dec.kbps > 8 ? (u32)s_dec.kbps : 128, c = kbps * 375 * x;
    return c > ST_MAX / 4 ? ST_MAX / 4 : c;
}

/* The refill, at interrupt time (see above): decode into the card's queue
 * while it has room, for at most about 4 ms at a go, so that no frame of
 * the screen waits on it for longer than that. */
static void feed(void)
{
    static s16 pcm[1152];
    static u8 out[3400];
    u32 t0;
    int want;

    if (s_lock || s_music != 1) return;
    t0 = A->ms();
    if (s_src == 1) {                    /* a song: the tracker plays it   */
        while (A->audio_space() >= (int)sizeof out && A->ms() - t0 < 4) {
            int n = tracker_render(out, (int)sizeof out);
            A->audio_write(out, n);
            s_written += (u32)n;
            if (n < (int)sizeof out) { s_music = 2; break; }
        }
        return;
    }
    if (!s_feeding_ok) return;
    if (s_buffering) {                   /* gathering a cushion first      */
        if (!s_in.done && s_in.len - s_in.pos < cushion(s_cushion_x)) return;
        s_buffering = 0;
    }
    /* The resampler makes up to in * card rate / recording rate + 2: ask
     * for no more than out has room for, however slow the recording. */
    want = (int)((sizeof out - 4) * (u32)s_dec.rate / (u32)s_rate);
    if (want > 1152) want = 1152;
    while (A->audio_space() >= (int)sizeof out && A->ms() - t0 < 4) {
        int n = s_dec.decode(&s_dec, &s_in, pcm, want), k;
        if (n == AUDIO_MORE) {
            /* Nothing more yet.  If the card has run out as well, the
             * network has fallen behind: gather a cushion again, twice
             * as big each time, rather than play each scrap as it comes. */
            if (A->audio_queued() == 0 && !s_in.done) {
                s_buffering = 1;
                s_ran_dry++;
                if (s_cushion_x < 8) s_cushion_x *= 2;
            }
            return;
        }
        if (n <= 0) { s_music = 2; return; }
        k = rs_run(&s_rs, pcm, n, out);
        A->audio_write(out, k);
        s_written += (u32)k;
    }
}

/* Called whenever browzer waits: once a song or recording has ended and
 * the last of it has played, the card is closed. */
static void music_feed(void)
{
    if (s_music == 2 && A->audio_queued() == 0) music_stop();
}

/* ", 128 kbps", or nothing when the recording does not say. */
static const char *kbps_text(void)
{
    static char t[24];
    t[0] = 0;
    if (s_dec.kbps > 0) fmt_str(t, sizeof t, ", %u kbps", (u32)s_dec.kbps);
    return t;
}

/* The card opened and feed() handed to the system.  For a recording, s_in
 * and s_dec are ready, and s_buffering says whether to gather first. */
static int music_start(int src, int rate_in)
{
    s_lock = 1;
    s_rate = A->audio_open(22050);
    if (!s_rate) { s_lock = 0; return 0; }
    s_src = src;
    s_written = 0;
    if (src == 2) rs_init(&s_rs, rate_in, s_rate);
    s_rate_in = rate_in;
    s_music = 1;
    s_feeding_ok = src == 2;
    A->audio_feeder(feed);
    s_lock = 0;
    return 1;
}

static int ends_with(const char *u, const char *ext)
{
    int a = 0, b = 0, i;
    while (u[a] && u[a] != '?' && u[a] != '#') a++;
    while (ext[b]) b++;
    if (a < b) return 0;
    for (i = 0; i < b; i++) {
        char c = u[a - b + i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if (c != ext[i]) return 0;
    }
    return 1;
}

static int type_is(const char *t, const char *p)
{
    while (*p) {
        char c = *t++;
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if (c != *p++) return 0;
    }
    return 1;
}

static int is_playlist(void)
{
    return type_is(s_dl_type, "audio/x-mpegurl") || type_is(s_dl_type, "audio/mpegurl") ||
           type_is(s_dl_type, "audio/x-scpls") || type_is(s_dl_type, "application/pls") ||
           ends_with(s_dl_url, ".m3u") || ends_with(s_dl_url, ".pls");
}

static int media_start(const char *ctype, const char *url, u32 length, u32 metaint, const char *station)
{
    int i, audio;
    /* Its memory is about to be used -- except when a station is called
     * again, when what is still queued plays on while it answers. */
    if (!s_redial) { music_stop(); s_redials = 0; }
    s_lock = 1;
    s_feeding_ok = 0;                    /* until the new stream's decoder is ready */
    for (i = 0; ctype[i] && i < (int)sizeof s_dl_type - 1; i++) s_dl_type[i] = ctype[i];
    s_dl_type[i] = 0;
    for (i = 0; url[i] && i < (int)sizeof s_dl_url - 1; i++) s_dl_url[i] = url[i];
    s_dl_url[i] = 0;
    for (i = 0; station[i] && i < (int)sizeof s_station - 1; i++) s_station[i] = station[i];
    s_station[i] = 0;
    s_now[0] = 0;
    s_title[0] = 0;
    s_dl_len = 0;
    s_esc = 0;
    audio = type_is(ctype, "audio/") || type_is(ctype, "application/ogg");
    if (audio && !is_playlist()) s_answered = 1;
    if (!is_playlist() && audio && (metaint || !length || length > DL_MAX)) {
        s_dl_state = DL_STREAM;
        s_metaint = metaint; s_meta_count = metaint; s_meta_len = -1;
        s_in.buf = s_dl; s_in.len = 0; s_in.pos = 0; s_in.done = 0;
        s_dec.decode = 0;
        s_drawn = 0;
        s_buffering = 0;
        s_live = metaint || !length;
        s_conn_ms = A->ms();
        s_lock = 0;
        return 2;                        /* a stream: no "loading" counts */
    }
    s_dl_state = length > DL_MAX ? DL_BIG : DL_COMING;
    s_lock = 0;
    return 1;
}

static void draw(void);

/* Radio sends what is playing between the audio: every metaint bytes, a
 * length byte, then that many times 16 bytes of StreamTitle='...';. */
static void meta_done(void)
{
    const char *k = "StreamTitle='";
    int i = 0, j, n = 0;
    s_meta[s_meta_got] = 0;
    for (i = 0; s_meta[i]; i++) {
        for (j = 0; k[j] && s_meta[i + j] == k[j]; j++) { }
        if (!k[j]) {
            i += j;
            while (s_meta[i] && !(s_meta[i] == '\'' && (s_meta[i + 1] == ';' || !s_meta[i + 1])) && n < (int)sizeof s_now - 1)
                s_now[n++] = s_meta[i++];
            s_now[n] = 0;
            return;
        }
    }
}

/* Seconds of the recording in hand: the card's queue, and what is still
 * to decode. */
static u32 in_hand(void)
{
    u32 kbps = s_dec.kbps > 8 ? (u32)s_dec.kbps : 128;
    return (u32)A->audio_queued() / (u32)(s_rate ? s_rate : 22050) + (s_in.len - s_in.pos) / (kbps * 125);
}

/* The player screen, now and then; and down the serial wire every ten
 * seconds, and whenever the sound ran out, so a listener with the cable
 * in can see how the stream is keeping up. */
static void stream_draw(void)
{
    static u32 logged, dry_seen;
    char t[96];
    if (A->ms() - s_drawn > 400) { s_drawn = A->ms(); draw(); }
    if (s_ran_dry != dry_seen) {
        dry_seen = s_ran_dry;
        fmt_str(t, sizeof t, "[browzer] the sound ran out (%u times); gathering more\n", (u32)dry_seen);
        A->wire(t);
    }
    if (s_music == 1 && A->ms() - logged >= 10000) {
        logged = A->ms();
        fmt_str(t, sizeof t, "[browzer] %u s played, %u s in hand%s\n", heard_seconds(), in_hand(),
                s_buffering ? ", gathering" : "");
        A->wire(t);
    }
}

static int stream_play(void)
{
    if (!s_feeding_ok) {
        if (!s_dec.decode) {
            int r = audio_detect(&s_in, &s_dec, s_dl_type);
            if (r == AUDIO_MORE && s_in.len < 65536) return 1;
            if (r != 1) { s_dl_state = DL_UNPLAYABLE; return 0; }
        }
        if (s_music && s_rate_in != s_dec.rate) music_stop();   /* called again, and changed */
        s_buffering = 1;
        s_cushion_x = 1;
        if (!s_music) {
            if (!music_start(2, s_dec.rate)) { s_dl_state = DL_NOCARD; return 0; }
        } else {
            s_lock = 1;
            s_feeding_ok = 1;
            s_lock = 0;
        }
    }
    stream_draw();
    return 1;
}

/* Into the stream's buffer, which feed() decodes from; when it is full,
 * wait for feed() to make room.  0 to stop. */
static int stream_take(const u8 *p, int n)
{
    while (n > 0) {
        u32 room, take;
        if (s_in.pos > ST_MAX / 2) {                    /* keep the unread part */
            u32 left;
            s_lock = 1;
            left = s_in.len - s_in.pos;
            memmove(s_dl, s_dl + s_in.pos, left);
            s_in.len = left; s_in.pos = 0;
            s_lock = 0;
        }
        room = ST_MAX - s_in.len;
        if (!room) {
            if (!stream_play()) return 0;
            if (ui_key() == UK_ESC) { s_esc = 1; return 0; }
            A->pump(0);
            continue;
        }
        take = (u32)n < room ? (u32)n : room;
        memcpy(s_dl + s_in.len, p, take);
        __asm__ __volatile__("" ::: "memory");          /* the bytes, then the length */
        s_in.len += take; p += take; n -= (int)take;
        if (!stream_play()) return 0;
    }
    return 1;
}

static int media_bytes(const u8 *p, int n)
{
    if (s_dl_state == DL_STREAM) {
        /* Take the song titles out, if the station puts them in. */
        while (n > 0 && s_metaint) {
            if (s_meta_len >= 0) {                      /* inside a title block */
                while (n > 0 && s_meta_got < s_meta_len) {
                    if (s_meta_got < (int)sizeof s_meta - 1) s_meta[s_meta_got] = (char)*p;
                    s_meta_got++; p++; n--;
                }
                if (s_meta_got >= s_meta_len) {
                    if (s_meta_got > (int)sizeof s_meta - 1) s_meta_got = (int)sizeof s_meta - 1;
                    if (s_meta_len) meta_done();
                    s_meta_len = -1; s_meta_count = s_metaint;
                }
                continue;
            }
            if (!s_meta_count) { s_meta_len = *p * 16; s_meta_got = 0; p++; n--; continue; }
            {
                u32 take = (u32)n < s_meta_count ? (u32)n : s_meta_count;
                if (!stream_take(p, (int)take)) return 0;
                p += take; n -= (int)take; s_meta_count -= take;
            }
        }
        if (n > 0 && !s_metaint) return stream_take(p, n);
        return 1;
    }
    if (s_dl_state != DL_COMING) return 0;
    if (s_dl_len + (u32)n > DL_MAX) { s_dl_state = DL_BIG; return 0; }
    memcpy(s_dl + s_dl_len, p, (u32)n);
    s_dl_len += (u32)n;
    music_feed();
    return 1;
}

static void media_end(int whole)
{
    if (whole < 0) { s_esc = 1; whole = 0; }          /* esc, while reading */
    if (s_dl_state == DL_COMING) s_dl_state = whole ? DL_CAME : DL_BROKE;
    else if (s_dl_state == DL_STREAM) {
        /* What is left is played out by feed(); a station about to be
         * called again plays on from the card's queue meanwhile. */
        s_dl_state = DL_STREAM_END;
        s_in.done = 1;                   /* feed() plays out what is left    */
        if (s_esc || (!whole && !s_live)) music_stop();
        else if (s_dec.decode && !s_music && s_in.len > s_in.pos) {
            s_buffering = 0;             /* too short to have started: play it */
            (void)music_start(2, s_dec.rate);
        }
    }
}

/* The nth address in a playlist (from 0): .pls says File1=..., File2=...,
 * .m3u just lists them.  Stations name a few servers, in case one is down. */
static int playlist_url(char *out, int cap, int nth)
{
    u32 i = 0;
    while (i < s_dl_len) {
        u32 e = i, a;
        int k = 0;
        while (e < s_dl_len && s_dl[e] != '\n' && s_dl[e] != '\r') e++;
        a = i;
        if (s_dl[a] == 'F' && s_dl[a + 1] == 'i' && s_dl[a + 2] == 'l' && s_dl[a + 3] == 'e') {
            while (a < e && s_dl[a] != '=') a++;
            if (a < e) a++;
        }
        while (a < e && (s_dl[a] == ' ' || s_dl[a] == '\t')) a++;
        if (e - a > 7 && s_dl[a] == 'h' && s_dl[a + 1] == 't' && s_dl[a + 2] == 't' && s_dl[a + 3] == 'p') {
            if (nth-- > 0) { i = e + 1; continue; }
            while (a < e && k < cap - 1 && s_dl[a] > ' ') out[k++] = (char)s_dl[a++];
            out[k] = 0;
            return 1;
        }
        i = e + 1;
    }
    return 0;
}

#define UK_SELALL 1                      /* ctrl-a                         */

int ui_key(void)
{
    int mods, k = A->key(&mods);
    if (k >= 0 && (mods & M_CTRL) && (k == 'a' || k == 'A')) return UK_SELALL;
    if (k >= 0 && (mods & (M_CTRL | M_ALT))) return -1;
    return k;
}

static PageField *field_of(int link)
{
    int f = link > 0 && link <= PAGE_LINKS ? s_pg.link_field[link] : 0;
    return f ? &s_pg.fields[f - 1] : 0;
}
static int  s_w, s_h, s_vh;              /* window, and page rows shown    */
static int  s_top;                       /* first page line shown          */
static int  s_sel;                       /* selected link, 0 for none      */
static char s_hist[HISTORY][PAGE_HREF_MAX + 16];
static int  s_hist_n;
static int  s_editing;
static char s_edit[PAGE_HREF_MAX];
static int  s_edit_len, s_edit_cur, s_edit_all;   /* the caret; ctrl-a picked all */
static int  s_leave_armed;               /* asked whether to leave          */
static int  s_loading;

/* The home page: a few words, your bookmarks, and how browzer works. */
static const char s_home_top[] =
    "<title>home</title>"
    "<h1>browzer</h1>"
    "<p>A very small web browser, on a floppy disk, on a Toshiba from 1996.</p>"
    "<p>Press <kbd>g</kbd> and type an address, or just some words to search for them.</p>";
static const char s_home_help[] =
    "<p><kbd>tab</kbd> picks a link, <kbd>enter</kbd> follows it, <kbd>backspace</kbd> goes back, <kbd>esc</kbd> leaves. "
    "Boxes on a page are typed into the same way: <kbd>tab</kbd> to one, <kbd>enter</kbd>, type, <kbd>enter</kbd> sends.</p>"
    "<p><kbd>b</kbd> adds the page you are on to your bookmarks, and <kbd>m</kbd> lists them, to change them. "
    "On that list, <kbd>p</kbd> puts one first here.</p>"
    "<p>Secure pages are encrypted, and the site has to prove who it is.</p>";

/* ---- drawing ------------------------------------------------------------ */
static u32 s_loads;                      /* pages loaded: a new one restarts the refresh wait */

/* Which line each link starts on, found in one pass over the page and
 * kept until the page changes, rather than the whole page searched for
 * every link asked about (a page of 1200 links took twenty million looks
 * for one press of shift-tab). */
static s16 s_link_line[PAGE_LINKS + 1];
static u32 s_link_lines_for;            /* the page they were found for   */
static int s_link_lines_n;

static int link_line(int id)
{
    if (s_link_lines_for != s_pg.gen || s_link_lines_n != s_pg.lines) {
        int y, x, i;
        for (i = 0; i <= PAGE_LINKS; i++) s_link_line[i] = -1;
        for (y = 0; y < s_pg.lines; y++)
            for (x = 0; x < s_pg.width; x++) {
                u16 l = s_pg.cells[y * s_pg.width + x].link;
                if (l && l <= PAGE_LINKS && s_link_line[l] < 0) s_link_line[l] = (s16)y;
            }
        s_link_lines_for = s_pg.gen;
        s_link_lines_n = s_pg.lines;
    }
    return id > 0 && id <= PAGE_LINKS ? s_link_line[id] : -1;
}

static void put_text_clipped(int x, int y, const char *t, u8 fg, u8 bg, int maxx)
{
    int key = 0;
    for (; *t && x < maxx; t++) {                /* \1: a key, in pink */
        if (*t == 1) { key = !key; continue; }
        term_win_put(x++, y, (u8)*t, key ? C_KEY : fg, bg);
    }
}

static void draw(void)
{
    char line[96];
    int x, y, i, play_x = s_w - 2;         /* where the playing label starts */
    u8 pp = WIN_PAPER;

    term_win_open(s_w, s_h);
    term_win_frame(0, C_GREY);

    /* What is playing, at the right of the top edge. */
    if (s_music) {
        char m[60];
        /* What has been heard: made, less what is still queued, less
         * the sound card's own loop. */
        u32 t = heard_seconds();
        int ml;
        const char *what = s_src == 1 ? (tracker_title()[0] ? tracker_title() : "a song")
                         : s_now[0] ? s_now : s_station[0] ? s_station : s_title[0] ? s_title : s_dec.kind;
        fmt_str(m, sizeof m, " playing %s %u:%02u ", what, t / 60, t % 60);
        for (ml = 0; m[ml]; ml++) { }
        if (ml < s_w - 30) { play_x = s_w - 2 - ml; term_win_text(play_x, 0, m, C_ACCENT, pp); }
    }

    /* The name, with the z in banana. */
    term_win_put(2, 0, ' ', C_GREY, pp);
    term_win_text(3, 0, "brow", C_BRIGHT, pp);
    term_win_put(7, 0, 'z', PAL_BANANA + 6, pp);
    term_win_text(8, 0, "er", C_BRIGHT, pp);
    x = term_win_text(11, 0, A->os_version(), C_DIM, pp);   /* which FLOPOS it came with */
    term_win_put(10, 0, ' ', C_GREY, pp);
    term_win_put(x++, 0, ' ', C_GREY, pp);
    if (s_pg.title[0]) {
        term_win_put(x++, 0, FR_H, C_GREY, pp);
        term_win_put(x++, 0, ' ', C_GREY, pp);
        for (i = 0; s_pg.title[i] && x < play_x - 2; i++) term_win_put(x++, 0, (u8)s_pg.title[i], C_TEXT, pp);
        term_win_put(x, 0, ' ', C_GREY, pp);         /* the title stops short of it */
    }

    /* The address. */
    if (s_editing) {
        /* The address being edited: the caret underlines the letter it is
         * at, or sits after the end; all of it picked shows reversed. */
        int vis = s_w - 10;
        term_win_band(1, 1, s_w - 2, WIN_HILITE);
        term_win_text(2, 1, "go:", C_ACCENT, WIN_HILITE);
        i = s_edit_cur > vis - 1 ? s_edit_cur - (vis - 1) : 0;
        for (x = 6; i <= s_edit_len && x < 6 + vis; i++, x++) {
            if (i == s_edit_len)
                term_win_put(x, 1, '_', C_ACCENT, WIN_HILITE);
            else if (s_edit_all)
                term_win_put(x, 1, (u8)s_edit[i], WIN_HILITE, C_ACCENT);
            else
                term_win_put_at(x, 1, (u8)s_edit[i], i == s_edit_cur ? C_ACCENT : C_BRIGHT, WIN_HILITE,
                                i == s_edit_cur ? AT_UNDERLINE : 0);
        }
    } else {
        term_win_text(2, 1, s_pg.secure ? "*" : ">", s_pg.secure ? C_ACCENT : C_DIM, pp);
        put_text_clipped(4, 1, s_pg.url, C_TEXT, pp, s_w - 2);
    }

    /* Rules above and below the page, joined to the frame. */
    for (x = 1; x < s_w - 1; x++) {
        term_win_put(x, 2, FR_H, C_GREY, pp);
        term_win_put(x, s_h - 3, FR_H, C_GREY, pp);
    }
    term_win_put(0, 2, FR_TEE_L, C_GREY, pp);       term_win_put(s_w - 1, 2, FR_TEE_R, C_GREY, pp);
    term_win_put(0, s_h - 3, FR_TEE_L, C_GREY, pp); term_win_put(s_w - 1, s_h - 3, FR_TEE_R, C_GREY, pp);

    /* A stream playing: its own screen instead of the page. */
    if (s_dl_state == DL_STREAM) {
        char t[120];
        u32 secs = heard_seconds();
        term_win_text(4, 4, s_station[0] ? s_station : "A stream", C_BRIGHT, pp);
        if (s_now[0]) { term_win_text(4, 6, "now:", C_DIM, pp); put_text_clipped(9, 6, s_now, C_TEXT, pp, s_w - 3); }
        if (s_dec.decode) {
            fmt_str(t, sizeof t, "%s%s, mono, %u Hz", s_dec.kind, kbps_text(), (u32)s_dec.rate);
            term_win_text(4, 8, t, C_DIM, pp);
            if (s_buffering)
                term_win_text(4, 10, s_ran_dry ? "the network fell behind; gathering more first..."
                                               : "gathering a few seconds before it plays...", C_ACCENT, pp);
            else {
                /* In hand: what the card has queued, and what is still to
                 * decode, at the recording's rate. */
                fmt_str(t, sizeof t, "listening for %u:%02u", secs / 60, secs % 60);
                term_win_text(4, 10, t, C_ACCENT, pp);
                fmt_str(t, sizeof t, "%u seconds in hand", in_hand());
                term_win_text(4, 12, t, C_DIM, pp);
            }
        } else
            term_win_text(4, 8, "listening for the first of it...", C_DIM, pp);
        term_win_text(4, 14, "\1esc\1 stops it", C_DIM, pp);
    } else
    /* The page. */
    for (y = 0; y < s_vh; y++) {
        int ln = s_top + y;
        if (ln >= s_pg.lines) break;
        for (x = 0; x < s_pg.width; x++) {
            PageCell *c = &s_pg.cells[ln * s_pg.width + x];
            int sel = s_sel && c->link == s_sel;
            if (c->ch == ' ' && !sel) continue;
            term_win_put(2 + x, 3 + y, c->ch, sel ? C_BRIGHT : c->col, sel ? WIN_HILITE : pp);
        }
    }

    /* A thumb down the right edge, for where in the page this is. */
    if (s_pg.lines > s_vh) {
        int pos = (s_vh - 1) * s_top / (s_pg.lines - s_vh);
        term_win_put(s_w - 2, 3 + pos, CH_FILL, C_GREY, pp);
    }

    /* The caret, in the box being typed into. */
    if (s_typing >= 0) {
        PageField *f = &s_pg.fields[s_typing];
        u32 k;
        int n = 0, cy = f->line - s_top;
        for (k = 0; k < f->vlen; k++) if ((s_pg.pool[f->val + k] & 0xC0) != 0x80) n++;
        if (n > f->width - 1) n = f->width - 1;
        if (cy >= 0 && cy < s_vh) term_win_put(2 + f->col + n, 3 + cy, ' ', C_TEXT, C_BRIGHT);
    }

    /* Status: what is happening, or where the picked link goes. */
    if (s_leave_armed) {
        put_text_clipped(2, s_h - 2, "Leave browzer? \1y\1 leaves; any other key stays.", C_ACCENT, pp, s_w - 12);
    } else if (s_choosing >= 0) {
        PageField *f = &s_pg.fields[s_choosing];
        fmt_str(line, sizeof line, "choosing, %u of %u: \1up\1 and \1down\1 change it, \1enter\1 keeps it",
                (u32)f->sel + 1, (u32)f->nopt);
        put_text_clipped(2, s_h - 2, line, C_ACCENT, pp, s_w - 12);
    } else if (s_typing >= 0) {
        put_text_clipped(2, s_h - 2, s_pg.fields[s_typing].kind == FK_AREA
                         ? "typing: \1enter\1 is a new line, \1tab\1 finishes"
                         : "typing: \1enter\1 sends it, \1esc\1 stops", C_ACCENT, pp, s_w - 12);
    } else if (s_sel && !s_loading && field_of(s_sel)) {
        PageField *f = field_of(s_sel);
        char where[PAGE_HREF_MAX + 16];
        if (f->kind == FK_SUBMIT) {
            const char *act = s_pg.forms[f->form].action;
            if (!url_resolve(s_pg.url, act[0] ? act : s_pg.url, where, sizeof where)) where[0] = 0;
            term_win_text(2, s_h - 2, "sends to", C_ACCENT, pp);
            put_text_clipped(11, s_h - 2, where, C_TEXT, pp, s_w - 12);
        } else
            put_text_clipped(2, s_h - 2,
                             f->kind == FK_CHECK  ? "a tick box: \1enter\1 ticks or unticks it" :
                             f->kind == FK_RADIO  ? "one of a set: \1enter\1 picks this one" :
                             f->kind == FK_SELECT ? "a list: \1enter\1 to choose from it" :
                                                    "a box: \1enter\1 to type into it", C_ACCENT, pp, s_w - 12);
    } else if (s_sel && !s_loading) {
        char where[PAGE_HREF_MAX + 16];
        if (!url_resolve(s_pg.url, s_pg.href[s_sel - 1], where, sizeof where))
            fmt_str(where, sizeof where, "(not a page: %s)", s_pg.href[s_sel - 1]);
        term_win_text(2, s_h - 2, "->", C_ACCENT, pp);
        put_text_clipped(5, s_h - 2, where, C_TEXT, pp, s_w - 12);
    } else if (s_pg.note[0]) {
        put_text_clipped(2, s_h - 2, s_pg.note, s_pg.note_col, pp, s_w - 12);
    }
    if (s_pg.lines > 1) {
        fmt_str(line, sizeof line, "%u/%u", (u32)(s_top + 1), (u32)s_pg.lines);
        for (i = 0; line[i]; i++) { }
        term_win_text(s_w - 2 - i, s_h - 2, line, C_DIM, pp);
    }

    /* Keys, let into the bottom of the frame. */
    {
        const char *keys = s_choosing >= 0 ? " \1up down\1 choose  \1enter\1 keep  \1esc\1 undo "
                         : s_editing ? " type  \1arrows\1 move  \1ctrl-a\1 all  \1enter\1 go  \1esc\1 cancel "
                         : s_typing >= 0 ? " type  \1enter\1 send  \1tab\1 done  \1esc\1 stop "
                         : s_w >= 66 ? " \1tab\1 link  \1enter\1 go  \1bksp\1 back  \1g\1 go to  \1b\1 mark  \1m\1 marks  \1esc\1 "
                         : s_w >= 60 ? " \1tab\1 link  \1enter\1 go  \1bksp\1 back  \1g\1 go to  \1esc\1 leave "
                                     : " \1tab\1 \1enter\1 \1bksp\1 \1g\1 \1esc\1 ";
        int kl = 0, i;
        for (i = 0; keys[i]; i++) if (keys[i] != 1) kl++;   /* the pink marks take no room */
        if (kl < s_w - 4) term_win_text((s_w - kl) / 2, s_h - 1, keys, C_DIM, pp);
    }
}

static void progress(void)
{
    music_feed();
    draw();
    main_pump(1);
}

/* ---- going places ------------------------------------------------------- */
static void push_history(void)
{
    int i;
    if (!s_pg.url[0]) return;
    if (s_hist_n == HISTORY) {
        for (i = 1; i < HISTORY; i++) {
            int k;
            for (k = 0; k < (int)sizeof s_hist[0]; k++) s_hist[i - 1][k] = s_hist[i][k];
        }
        s_hist_n--;
    }
    for (i = 0; s_pg.url[i] && i < (int)sizeof s_hist[0] - 1; i++) s_hist[s_hist_n][i] = s_pg.url[i];
    s_hist[s_hist_n][i] = 0;
    s_hist_n++;
}

/* ---- bookmarks -------------------------------------------------------------
 * BOOKMARK.TXT on the floppy, plain text, one a line: the address, a
 * space, the page's title.  A line that starts with "* " is also on the
 * home page.  Written whole, in place, every time one changes. */
#define MARKS 48
typedef struct { char url[PAGE_HREF_MAX]; char title[64]; u8 home; } Mark;
static Mark s_mark[MARKS];
static int  s_nmarks, s_marks_read, s_del_armed;
static u8   s_mfile[8192];

static void marks_read(void)
{
    AppFile f;
    int n, i = 0;
    if (s_marks_read) return;
    s_marks_read = 1;
    s_nmarks = 0;
    if (!A->file_find("BOOKMARK.TXT", &f)) return;
    n = A->file_read(&f, s_mfile, sizeof s_mfile);
    while (i < n && s_mfile[i] != 0x1A && s_mfile[i] && s_nmarks < MARKS) {
        Mark *m = &s_mark[s_nmarks];
        int k = 0;
        m->home = 0;
        if (s_mfile[i] == '*' && i + 1 < n && s_mfile[i + 1] == ' ') { m->home = 1; i += 2; }
        while (i < n && s_mfile[i] > ' ' && k < PAGE_HREF_MAX - 1) m->url[k++] = (char)s_mfile[i++];
        m->url[k] = 0;
        while (i < n && s_mfile[i] == ' ') i++;
        k = 0;
        while (i < n && s_mfile[i] != '\n' && s_mfile[i] != '\r' && s_mfile[i] != 0x1A && s_mfile[i]) {
            if (k < 63) m->title[k++] = (char)s_mfile[i];
            i++;
        }
        m->title[k] = 0;
        while (i < n && (s_mfile[i] == '\n' || s_mfile[i] == '\r')) i++;
        if (m->url[0]) s_nmarks++;
    }
}

static int marks_write(void)
{
    AppFile f;
    int n = 0, i, k;
    if (!A->file_find("BOOKMARK.TXT", &f) || f.size > sizeof s_mfile) return 0;
    for (i = 0; i < s_nmarks; i++) {
        Mark *m = &s_mark[i];
        if (n + PAGE_HREF_MAX + 70 > (int)f.size - 1) break;
        if (m->home) { s_mfile[n++] = '*'; s_mfile[n++] = ' '; }
        for (k = 0; m->url[k]; k++) s_mfile[n++] = (u8)m->url[k];
        s_mfile[n++] = ' ';
        for (k = 0; m->title[k]; k++) s_mfile[n++] = (u8)m->title[k];
        s_mfile[n++] = '\r'; s_mfile[n++] = '\n';
    }
    s_mfile[n++] = 0x1A;                         /* where DOS stops reading */
    while (n < (int)f.size) s_mfile[n++] = 0;
    return A->file_write(&f, s_mfile);
}

static int mark_find(const char *url)
{
    int i, k;
    for (i = 0; i < s_nmarks; i++) {
        for (k = 0; url[k] && url[k] == s_mark[i].url[k]; k++) { }
        if (!url[k] && !s_mark[i].url[k]) return i;
    }
    return -1;
}

static void say(const char *t, u8 col)
{
    fmt_str(s_pg.note, sizeof s_pg.note, "%s", t);
    s_pg.note_col = col;
}

/* Text safe to put into a page we write ourselves. */
static int html_text(char *out, int n, int cap, const char *t)
{
    for (; *t && n < cap - 8; t++) {
        if (*t == '<') { out[n++] = '&'; out[n++] = 'l'; out[n++] = 't'; out[n++] = ';'; }
        else if (*t == '&') { out[n++] = '&'; out[n++] = 'a'; out[n++] = 'm'; out[n++] = 'p'; out[n++] = ';'; }
        else if (*t == '"') { out[n++] = '&'; out[n++] = '#'; out[n++] = '3'; out[n++] = '4'; out[n++] = ';'; }
        else out[n++] = *t;
    }
    return n;
}

static int put(char *out, int n, int cap, const char *t)
{
    while (*t && n < cap - 1) out[n++] = *t++;
    return n;
}

static void home(void)
{
    static char page[sizeof s_home_top + sizeof s_home_help + MARKS * (PAGE_HREF_MAX * 5 + 140) + 200];
    int n = 0, i, pass, cap = (int)sizeof page;

    marks_read();
    n = put(page, n, cap, s_home_top);
    n = put(page, n, cap, "<h2>Your bookmarks</h2>");
    if (!s_nmarks)
        n = put(page, n, cap, "<p>None yet. On any page, <kbd>b</kbd> keeps it here.</p>");
    else {
        n = put(page, n, cap, "<ul>");
        for (pass = 0; pass < 2; pass++)              /* the ones put first, then the rest */
            for (i = 0; i < s_nmarks; i++) {
                if ((s_mark[i].home ? 0 : 1) != pass) continue;
                n = put(page, n, cap, "<li><a href=\"");
                n = html_text(page, n, cap, s_mark[i].url);
                n = put(page, n, cap, "\">");
                n = html_text(page, n, cap, s_mark[i].title[0] ? s_mark[i].title : s_mark[i].url);
                n = put(page, n, cap, "</a></li>");
            }
        n = put(page, n, cap, "</ul>");
    }
    n = put(page, n, cap, s_home_help);
    page[n] = 0;
    {
        char t[64];
        fmt_str(t, sizeof t, "[page] home: %u bookmarks\n", (u32)s_nmarks);
        A->wire(t);
    }
    http_page_html(page, &s_pg);
    fmt_str(s_pg.url, sizeof s_pg.url, "flopos:home");
    s_top = 0;
    s_sel = 0;
}

/* The list of them, as a page: pick one with tab, then p or d. */
static void marks_page(void)
{
    static char page[MARKS * (PAGE_HREF_MAX * 2 + 140) + 600];
    int n = 0, i, cap = (int)sizeof page;

    marks_read();
    n = put(page, n, cap, "<title>bookmarks</title><h1>Bookmarks</h1>");
    if (!s_nmarks)
        n = put(page, n, cap, "<p>None yet. On any page, <kbd>b</kbd> keeps it here.</p>");
    else {
        n = put(page, n, cap, "<p><kbd>tab</kbd> picks one, <kbd>enter</kbd> goes there, <kbd>p</kbd> puts it first on the home page "
                              "or back among the rest, <kbd>d</kbd> deletes it.</p><ul>");
        for (i = 0; i < s_nmarks; i++) {
            n = put(page, n, cap, "<li>");
            if (s_mark[i].home) n = put(page, n, cap, "<b>first</b> ");
            n = put(page, n, cap, "<a href=\"");
            n = html_text(page, n, cap, s_mark[i].url);
            n = put(page, n, cap, "\">");
            n = html_text(page, n, cap, s_mark[i].title[0] ? s_mark[i].title : s_mark[i].url);
            n = put(page, n, cap, "</a> <i>");
            n = html_text(page, n, cap, s_mark[i].url);
            n = put(page, n, cap, "</i></li>");
        }
        n = put(page, n, cap, "</ul>");
    }
    page[n] = 0;
    http_page_html(page, &s_pg);
    fmt_str(s_pg.url, sizeof s_pg.url, "flopos:bookmarks");
}

static int on_marks_page(void)
{
    return s_pg.url[0] == 'f' && s_pg.url[7] == 'b';            /* flopos:bookmarks */
}

static void mark_this(void)
{
    Mark *m;
    int i;
    marks_read();
    if (s_pg.url[0] == 'f' && s_pg.url[1] == 'l') { say("That is one of mine; there is nothing to keep.", C_ACCENT); return; }
    if (mark_find(s_pg.url) >= 0) { say("Already in your bookmarks. \1m\1 shows them.", C_DIM); return; }
    if (s_nmarks >= MARKS) { say("The bookmarks are full. d on one in m makes room.", C_ACCENT); return; }
    m = &s_mark[s_nmarks];
    for (i = 0; s_pg.url[i] && i < PAGE_HREF_MAX - 1; i++) m->url[i] = s_pg.url[i];
    m->url[i] = 0;
    for (i = 0; s_pg.title[i] && i < 63; i++) m->title[i] = s_pg.title[i] == '\n' ? ' ' : s_pg.title[i];
    m->title[i] = 0;
    m->home = 0;
    s_nmarks++;
    say(marks_write() ? "Added to your bookmarks, which the home page lists. \1m\1 lists them too, to change them."
                      : "Could not write BOOKMARK.TXT to the floppy.", C_ACCENT);
}

/* p or d on the bookmarks page, for the bookmark picked. */
static void mark_change(int key)
{
    char url[PAGE_HREF_MAX + 16];
    int i, top = s_top;
    if (!s_sel || field_of(s_sel)) { say("Pick one with tab first.", C_ACCENT); return; }
    for (i = 0; s_pg.href[s_sel - 1][i] && i < (int)sizeof url - 1; i++) url[i] = s_pg.href[s_sel - 1][i];
    url[i] = 0;
    i = mark_find(url);
    if (i < 0) return;
    if (key == 'd') {
        if (!s_del_armed) { s_del_armed = 1; say("d again deletes it.", C_ACCENT); return; }
        s_del_armed = 0;
        for (; i < s_nmarks - 1; i++) s_mark[i] = s_mark[i + 1];
        s_nmarks--;
    } else {
        s_mark[i].home = !s_mark[i].home;
    }
    if (!marks_write()) { say("Could not write BOOKMARK.TXT to the floppy.", C_ACCENT); return; }
    marks_page();
    s_top = top;
    say(key == 'd' ? "Deleted." : "Changed. \1h\1 shows the home page.", C_DIM);
}


/* A page, or the answer to a form sent to url (post, of postlen bytes). */
static void go_with(const char *url, const u8 *post, int postlen)
{
    int loaded;
    s_loads++;
    s_top = 0;
    s_sel = 0;
    s_typing = -1;
    s_choosing = -1;
    if (url[0] == 'f' && url[1] == 'l' && url[2] == 'o' && url[3] == 'p') { home(); return; }
    s_loading = 1;
    s_pg.title[0] = 0;
    s_pg.lines = 1;
    progress();
    if (!A->net_online()) {
        page_clear(&s_pg);
        fmt_str(s_pg.note, sizeof s_pg.note, "No network. Try the network command.");
        s_pg.note_col = C_ACCENT;
        s_loading = 0;
        return;
    }
    s_dl_state = 0;
    loaded = http_load(url, &s_pg, post, postlen);
    if (s_dl_state) {
        /* Not a page: what came instead. */
        static char html[900];
        char name[64];
        const char *what = 0, *u;
        int i, k = 0, st = s_dl_state;
        s_dl_state = DL_NONE;
        for (u = s_dl_url, i = 0; s_dl_url[i]; i++) if (s_dl_url[i] == '/') u = s_dl_url + i + 1;
        while (u[k] && u[k] != '?' && u[k] != '#' && k < 63) { name[k] = u[k]; k++; }
        name[k] = 0;
        if (!name[0]) fmt_str(name, sizeof name, "%s", s_station[0] ? s_station : "this");

        /* A station that hung up is called again, five times at most in a
         * row; a minute of listening starts the count again. */
        if (st == DL_STREAM_END && !s_esc && s_live) {
            if (A->ms() - s_conn_ms > 60000) s_redials = 0;
            if (s_redials < 5) {
                char again[PAGE_HREF_MAX + 16];
                for (i = 0; s_dl_url[i] && i < (int)sizeof again - 1; i++) again[i] = s_dl_url[i];
                again[i] = 0;
                s_redials++;
                s_redial = 1;
                go_with(again, 0, 0);
                s_redial = 0;
                return;
            }
        }

        if (st == DL_CAME && is_playlist()) {
            /* Each of its servers in turn, until one answers with sound. */
            char cand[4][PAGE_HREF_MAX + 16];
            int nc = 0, c;
            while (nc < 4 && playlist_url(cand[nc], (int)sizeof cand[0], nc)) nc++;
            for (c = 0; c < nc; c++) {
                s_answered = 0;
                go_with(cand[c], 0, 0);
                if (s_answered || s_esc) return;
            }
            if (nc) return;
            what = "<p>A playlist, but with no address in it that browzer can follow.</p>";
        }
        if (what) {
            fmt_str(html, sizeof html, "<title>%s</title><h1>%s</h1>%s", name, name, what);
        } else if (st == DL_CAME && tracker_load(s_dl, s_dl_len, DL_MAX, 22050)) {
            /* Loaded once only: loading changes the file where it lies. */
            if (music_start(1, 0)) {
                s_lock = 1;
                tracker_rate(s_rate);
                s_lock = 0;
                music_feed();
                fmt_str(html, sizeof html, "<title>%s</title><h1>%s</h1><p>A %u-channel %s song, playing.</p>"
                        "<p>It goes on playing while you browse. <kbd>s</kbd> stops it.</p>",
                        tracker_title()[0] ? tracker_title() : name, tracker_title()[0] ? tracker_title() : name,
                        (u32)tracker_channels(), tracker_kind());
            } else
                fmt_str(html, sizeof html, "<title>no sound</title><h1>%s</h1><p>A song -- but this machine has "
                        "no sound card that plays recordings.</p>", name);
        } else if (st == DL_CAME) {
            s_in.buf = s_dl; s_in.len = s_dl_len; s_in.pos = 0; s_in.done = 1;
            s_buffering = 0;
            if (audio_detect(&s_in, &s_dec, s_dl_type) == 1) {
                if (music_start(2, s_dec.rate)) {
                    fmt_str(s_title, sizeof s_title, "%s", name);
                    music_feed();
                    fmt_str(html, sizeof html, "<title>%s</title><h1>%s</h1><p>%s%s, playing.</p>"
                            "<p>It goes on playing while you browse. <kbd>s</kbd> stops it.</p>",
                            name, name, s_dec.kind, kbps_text());
                } else
                    fmt_str(html, sizeof html, "<title>no sound</title><h1>%s</h1><p>A recording -- but this "
                            "machine has no sound card that plays recordings.</p>", name);
            } else
                fmt_str(html, sizeof html, "<title>a file</title><h1>Not a page</h1><p>That is %s, %u bytes, "
                        "and browzer cannot show it or play it.</p>", s_dl_type, s_dl_len);
        } else if (st == DL_STREAM_END) {
            fmt_str(html, sizeof html, "<title>%s</title><h1>%s</h1><p>%s</p>%s%s%s",
                    s_station[0] ? s_station : name, s_station[0] ? s_station : name,
                    s_esc ? "Stopped." : s_live ? "The station stopped answering, five times running." :
                    s_music ? "That was all of it; the last of it is still playing." : "The stream ended.",
                    s_now[0] ? "<p>Last playing: " : "", s_now[0] ? s_now : "", s_now[0] ? "</p>" : "");
        } else if (st == DL_UNPLAYABLE) {
            fmt_str(html, sizeof html, "<title>%s</title><h1>%s</h1><p>That is %s, which browzer cannot play "
                    "(mp3, AAC, Ogg, FLAC, wav, .mod, .xm, .s3m and .it it can).</p>", name, name, s_dl_type);
        } else if (st == DL_NOCARD) {
            fmt_str(html, sizeof html, "<title>no sound</title><h1>%s</h1><p>This machine has no sound card "
                    "that plays recordings.</p>", name);
        } else if (st == DL_BIG) {
            fmt_str(html, sizeof html, "<title>a file</title><h1>Not a page</h1><p>That is %s, too big to keep "
                    "here: 2 MB at most.</p>", s_dl_type);
        } else {
            fmt_str(html, sizeof html, "<title>a file</title><h1>Not a page</h1><p>It stopped coming before "
                    "it was all here.</p>");
        }
        http_page_html(html, &s_pg);
        for (i = 0; s_dl_url[i]; i++) s_pg.url[i] = s_dl_url[i];
        s_pg.url[i] = 0;
        s_loading = 0;
        return;
    }
    if (!loaded && s_pg.lines <= 1 && s_pg.col == 0) {
        /* Nothing came: say why on the page itself, not only in the
         * status line, and keep the address so r can try again. */
        static char html[600];
        char why[128], where[PAGE_HREF_MAX + 16];
        int i;
        for (i = 0; s_pg.note[i]; i++) why[i] = s_pg.note[i];
        why[i] = 0;
        for (i = 0; s_pg.url[i]; i++) where[i] = s_pg.url[i];
        where[i] = 0;
        if (s_pg.untrusted)
            fmt_str(html, sizeof html,
                    "<title>who is this?</title><h2>This site could not prove who it is.</h2>"
                    "<p>%s</p><p>That can mean somebody is pretending to be it, or that the "
                    "site is set up wrong, or that this laptop's clock is.</p>"
                    "<p><kbd>a</kbd> goes there anyway, unchecked -- type nothing private. "
                    "backspace goes back.</p>", why);
        else
            fmt_str(html, sizeof html,
                    "<title>could not open it</title><h2>That page did not come.</h2>"
                    "<p>%s</p><p>r tries again, backspace goes back, g goes somewhere else.</p>",
                    why[0] ? why : "Nothing answered.");
        {
            int untrusted = s_pg.untrusted;
            http_page_html(html, &s_pg);
            s_pg.untrusted = untrusted;  /* so a can still go anyway        */
        }
        for (i = 0; where[i]; i++) s_pg.url[i] = where[i];
        s_pg.url[i] = 0;
        fmt_str(s_pg.note, sizeof s_pg.note, "%s", why);
        s_pg.note_col = C_ACCENT;
    }
    s_loading = 0;
    if (s_pg.status >= 200 && s_pg.status < 300) {
        if (s_pg.secure && s_pg.checked)
            fmt_str(s_pg.note, sizeof s_pg.note, "%s, vouched for by %s",
                    s_pg.pq ? "encrypted, post-quantum" : "encrypted", s_pg.who);
        else if (s_pg.secure)
            fmt_str(s_pg.note, sizeof s_pg.note, "%s -- NOT checked, could be anybody",
                    s_pg.pq ? "encrypted, post-quantum" : "encrypted");
        else
            fmt_str(s_pg.note, sizeof s_pg.note, "%u bytes, not encrypted", s_pg.bytes);
        s_pg.note_col = s_pg.secure && !s_pg.checked ? C_ACCENT : C_DIM;
    }
}

static void go(const char *url) { go_with(url, 0, 0); }

/* A page that asked to be loaded again (<meta refresh>): chat pages without
 * JavaScript show new messages this way.  Only short waits are kept --
 * thirty seconds or less, or ten to go somewhere else -- so a page that
 * merely refreshes its news now and then is left alone.  The same page
 * comes back where it was scrolled to. */
static int refresh_wanted(void)
{
    if (!s_pg.refresh) return 0;
    return s_pg.refresh_url[0] ? s_pg.refresh <= 10 : s_pg.refresh <= 30;
}

static void refresh_page(void)
{
    char u[PAGE_HREF_MAX + 16];
    int top = s_top, sel = s_sel, same = 1, i;
    if (s_pg.refresh_url[0]) {
        if (!url_resolve(s_pg.url, s_pg.refresh_url, u, sizeof u)) { s_pg.refresh = 0; return; }
    } else {
        for (i = 0; s_pg.url[i] && i < (int)sizeof u - 1; i++) u[i] = s_pg.url[i];
        u[i] = 0;
    }
    for (i = 0; u[i] || s_pg.url[i]; i++) if (u[i] != s_pg.url[i]) { same = 0; break; }
    go(u);
    if (same) {
        s_top = top < s_pg.lines ? top : 0;
        s_sel = sel <= s_pg.nlinks ? sel : 0;
    }
}

/* ---- forms ----------------------------------------------------------------- */
/* Bytes as a form sends them: letters and digits as they are, a space as
 * +, anything else as %XX. */
static int urlenc(u8 *out, int n, int cap, const char *p, u32 len)
{
    static const char hexd[] = "0123456789ABCDEF";
    u32 i;
    for (i = 0; i < len && n < cap - 3; i++) {
        u8 c = (u8)p[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '*') out[n++] = c;
        else if (c == ' ') out[n++] = '+';
        else if (c == '\n') { out[n++] = '%'; out[n++] = '0'; out[n++] = 'D';
                              out[n++] = '%'; out[n++] = '0'; out[n++] = 'A'; }
        else { out[n++] = '%'; out[n++] = (u8)hexd[c >> 4]; out[n++] = (u8)hexd[c & 15]; }
    }
    return n;
}

/* Send the form that field belongs to: every named box and hidden value
 * in it, and the button, if a button was what sent it. */
static void submit(PageField *by)
{
    PageForm *form = &s_pg.forms[by->form];
    char url[PAGE_HREF_MAX + 16];
    int n = 0, i, cap = (int)sizeof s_body;

    for (i = 0; i < s_pg.nfields; i++) {
        PageField *f = &s_pg.fields[i];
        if (f->form != by->form || !f->nlen) continue;
        if (f->kind == FK_SUBMIT && f != by) continue;
        if ((f->kind == FK_CHECK || f->kind == FK_RADIO) && !f->on) continue;
        if (f->kind == FK_SELECT && !f->nopt) continue;
        if (n) s_body[n++] = '&';
        n = urlenc(s_body, n, cap, s_pg.pool + f->name, f->nlen);
        s_body[n++] = '=';
        n = urlenc(s_body, n, cap, s_pg.pool + f->val, f->vlen);
    }
    if (n >= cap - 4) {
        fmt_str(s_pg.note, sizeof s_pg.note, "That form is too big to send.");
        s_pg.note_col = C_ACCENT;
        return;
    }
    if (!url_resolve(s_pg.url, form->action[0] ? form->action : s_pg.url, url, sizeof url)) {
        fmt_str(s_pg.note, sizeof s_pg.note, "The form says to send it nowhere I can find.");
        s_pg.note_col = C_ACCENT;
        return;
    }
    push_history();
    if (form->post) {
        go_with(url, s_body, n);
    } else {
        /* A GET form: what it says goes after a ? in the address. */
        int k = 0;
        while (url[k] && url[k] != '?' && url[k] != '#') k++;
        if (k + 1 + n >= (int)sizeof url) {
            fmt_str(s_pg.note, sizeof s_pg.note, "Too much to send in an address.");
            s_pg.note_col = C_ACCENT;
            return;
        }
        url[k++] = '?';
        for (i = 0; i < n; i++) url[k++] = (char)s_body[i];
        url[k] = 0;
        go(url);
    }
}

/* One typed character into the box being typed into, as UTF-8. */
static void type_char(PageField *f, int k)
{
    char b[2];
    int n = 0, i;
    switch (k) {
    case CH_ARING:   b[0] = (char)0xC3; b[1] = (char)0x85; n = 2; break;
    case CH_ADIA:    b[0] = (char)0xC3; b[1] = (char)0x84; n = 2; break;
    case CH_ODIA:    b[0] = (char)0xC3; b[1] = (char)0x96; n = 2; break;
    case CH_ARING_L: b[0] = (char)0xC3; b[1] = (char)0xA5; n = 2; break;
    case CH_ADIA_L:  b[0] = (char)0xC3; b[1] = (char)0xA4; n = 2; break;
    case CH_ODIA_L:  b[0] = (char)0xC3; b[1] = (char)0xB6; n = 2; break;
    default:
        if (k == '\n' || (k >= 32 && k < 127)) { b[0] = (char)k; n = 1; }
    }
    if (!n || f->vlen + (u32)n > f->vcap) return;
    for (i = 0; i < n; i++) s_pg.pool[f->val + f->vlen++] = b[i];
    page_field_show(&s_pg, (int)(f - s_pg.fields));
}

static void unchar(PageField *f)
{
    if (!f->vlen) return;
    f->vlen--;
    while (f->vlen && (s_pg.pool[f->val + f->vlen] & 0xC0) == 0x80) f->vlen--;  /* the whole letter */
    page_field_show(&s_pg, (int)(f - s_pg.fields));
}

/* What was typed into the address bar: an address, or words to look for. */
static void go_typed(const char *t)
{
    char url[PAGE_HREF_MAX + 16];
    int i, n = 0, dot = 0, space = 0, scheme = 0;

    for (i = 0; t[i]; i++) {
        if (t[i] == '.') dot = 1;
        if (t[i] == ' ') space = 1;
        if (t[i] == ':' && t[i + 1] == '/' && t[i + 2] == '/') scheme = 1;
    }
    if (scheme) {
        for (i = 0; t[i] && n < (int)sizeof url - 1; i++) url[n++] = t[i];
    } else if (dot && !space) {
        fmt_str(url, sizeof url, "http://%s", t);
        n = -1;
    } else {
        /* Words: ask FrogFind, spelled the way a query is spelled. */
        static const char hexd[] = "0123456789ABCDEF";
        const char *pre = "http://frogfind.com/?q=";
        for (i = 0; pre[i]; i++) url[n++] = pre[i];
        for (i = 0; t[i] && n < (int)sizeof url - 4; i++) {
            u8 c = (u8)t[i];
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                c == '-' || c == '_' || c == '.' || c == '~') url[n++] = (char)c;
            else if (c == ' ') url[n++] = '+';
            else { url[n++] = '%'; url[n++] = hexd[c >> 4]; url[n++] = hexd[c & 15]; }
        }
    }
    if (n >= 0) url[n] = 0;
    push_history();
    go(url);
}

/* g: the address, ready to change, the caret at its end. */
static void edit_start(void)
{
    int i = 0;
    s_editing = 1;
    s_edit_all = 0;
    if (!(s_pg.url[0] == 'f' && s_pg.url[1] == 'l' && s_pg.url[2] == 'o' && s_pg.url[3] == 'p'))
        for (; s_pg.url[i] && i < PAGE_HREF_MAX - 1; i++) s_edit[i] = s_pg.url[i];
    s_edit_len = s_edit_cur = i;
}

static void edit_cut(int at)
{
    memmove(s_edit + at, s_edit + at + 1, (u32)(s_edit_len - at - 1));
    s_edit_len--;
}

/* One of a set of choices picked: the others with its name let go. */
static void radio_pick(PageField *f)
{
    int i;
    for (i = 0; i < s_pg.nfields; i++) {
        PageField *g = &s_pg.fields[i];
        u32 k;
        if (g->kind != FK_RADIO || g->form != f->form || g->nlen != f->nlen) continue;
        for (k = 0; k < f->nlen && s_pg.pool[g->name + k] == s_pg.pool[f->name + k]; k++) { }
        if (k < f->nlen) continue;
        g->on = g == f;
        page_field_show(&s_pg, i);
    }
}

/* A list's choice changed to number i (kept in range). */
static void choose(PageField *f, int i)
{
    const PageOption *o;
    if (i < 0) i = 0;
    if (i >= f->nopt) i = f->nopt - 1;
    f->sel = (u16)i;
    o = &s_pg.opts[f->opt0 + i];
    f->val = o->val;
    f->vlen = o->vlen;
    page_field_show(&s_pg, (int)(f - s_pg.fields));
}

/* The next choice after the current one whose words start with ch. */
static void choose_letter(PageField *f, int ch)
{
    int i;
    if (ch >= 'A' && ch <= 'Z') ch += 32;
    for (i = 1; i <= f->nopt; i++) {
        int at = (f->sel + i) % f->nopt;
        const PageOption *o = &s_pg.opts[f->opt0 + at];
        int c = o->llen ? (u8)s_pg.pool[o->lab] : 0;
        if (c >= 'A' && c <= 'Z') c += 32;
        if (c == ch) { choose(f, at); return; }
    }
}

static void follow(void)
{
    char url[PAGE_HREF_MAX + 16];
    PageField *f;
    if (!s_sel) return;
    if ((f = field_of(s_sel)) != 0) {
        if (f->kind == FK_SUBMIT) submit(f);
        else if (f->kind == FK_CHECK) { f->on = !f->on; page_field_show(&s_pg, (int)(f - s_pg.fields)); }
        else if (f->kind == FK_RADIO) radio_pick(f);
        else if (f->kind == FK_SELECT) { s_choosing = (int)(f - s_pg.fields); s_chose_was = f->sel; }
        else s_typing = (int)(f - s_pg.fields);
        return;
    }
    if (!url_resolve(s_pg.url, s_pg.href[s_sel - 1], url, sizeof url)) return;
    push_history();
    go(url);
}

static void back(void)
{
    char url[PAGE_HREF_MAX + 16];
    int i;
    if (!s_hist_n) return;
    s_hist_n--;
    for (i = 0; s_hist[s_hist_n][i]; i++) url[i] = s_hist[s_hist_n][i];
    url[i] = 0;
    go(url);
}

/* Next or previous link from the one picked, or from the top of what is
 * on screen, and scroll so it can be seen. */
static void pick(int dir)
{
    int id, ln;
    if (!s_pg.nlinks) return;
    if (!s_sel) {
        /* The first link on screen, or the last one going backwards. */
        int best = 0;
        for (id = 1; id <= s_pg.nlinks; id++) {
            ln = link_line(id);
            if (ln < 0) continue;
            if (dir > 0 && ln >= s_top) { best = id; break; }
            if (dir < 0 && ln < s_top + s_vh) best = id;
        }
        s_sel = best ? best : (dir > 0 ? 1 : s_pg.nlinks);
    } else {
        s_sel += dir;
        if (s_sel < 1) s_sel = s_pg.nlinks;
        if (s_sel > s_pg.nlinks) s_sel = 1;
    }
    {
        char t[24];
        fmt_str(t, sizeof t, "[pick] %u\n", (u32)s_sel);
        A->wire(t);                       /* for whoever is testing */
    }
    ln = link_line(s_sel);
    if (ln >= 0 && (ln < s_top || ln >= s_top + s_vh)) {
        s_top = ln - s_vh / 3;
        if (s_top < 0) s_top = 0;
    }
}

static void scroll(int by)
{
    int max = s_pg.lines - s_vh;
    s_top += by;
    if (s_top > max) s_top = max;
    if (s_top < 0) s_top = 0;
    /* A picked link that has scrolled out of sight is let go. */
    if (s_sel) {
        int ln = link_line(s_sel);
        if (ln < s_top || ln >= s_top + s_vh) s_sel = 0;
    }
}

void browzer_run(const char *start)
{
    int k;

    term_win_open(term_cols(), term_rows());
    s_w = term_win_width();
    s_h = term_win_height();
    s_vh = s_h - 6;

    if (!s_ready) {
        s_pg.cells = s_cells;
        s_pg.href = s_hrefs;
        s_pg.forms = s_forms;
        s_pg.fields = s_fields;
        s_pg.pool = s_pool;
        s_pg.link_field = s_link_field;
        s_pg.opts = s_opts;
        s_pg.max_lines = MAX_LINES;
        s_pg.progress = progress;
        web_media_start = media_start;
        web_media_bytes = media_bytes;
        web_media_end = media_end;
        s_ready = 1;
        s_pg.width = 0;
    }

    /* Pages are laid out for a width; if the screen changed since the
     * last visit, the old one is laid out wrong, so it is fetched again. */
    if (s_pg.width != s_w - 4) {
        s_pg.width = s_w - 4;
        if (!start || !start[0]) {
            if (s_pg.url[0] && s_pg.url[0] != 'f') start = s_pg.url;
            else start = 0;
        }
        if (!start) home();
    }
    if (start && start[0]) go_typed(start);
    s_editing = 0;

    for (;;) {
        /* Drawn when something has happened, and otherwise ten times a
         * second for what changes on its own (a stream's seconds in
         * hand).  Not on every pass: a loop with nothing to do should
         * look idle, and the HUD's 'Thunking counts it so. */
        static u32 drawn_at;
        static int stale = 1;
        if (stale || A->ms() - drawn_at >= 100) { draw(); drawn_at = A->ms(); stale = 0; }
        k = ui_key();
        if (k < 0) {
            static u32 loads_seen, due;
            music_feed();
            if (s_loads != loads_seen) { loads_seen = s_loads; due = A->ms() + (u32)s_pg.refresh * 1000u; stale = 1; }
            if (refresh_wanted() && !s_editing && s_typing < 0 && s_choosing < 0 && !s_leave_armed &&
                (s32)(A->ms() - due) >= 0)
                refresh_page();
            main_pump(0);
            continue;
        }
        stale = 1;

        if (s_choosing >= 0) {           /* up and down through a list      */
            PageField *f = &s_pg.fields[s_choosing];
            if (k == UK_UP) choose(f, f->sel - 1);
            else if (k == UK_DOWN) choose(f, f->sel + 1);
            else if (k == UK_PGUP) choose(f, f->sel - 10);
            else if (k == UK_PGDN) choose(f, f->sel + 10);
            else if (k == UK_HOME) choose(f, 0);
            else if (k == UK_END) choose(f, f->nopt - 1);
            else if (k == UK_ESC) { choose(f, s_chose_was); s_choosing = -1; }
            else if (k == UK_ENTER) s_choosing = -1;
            else if (k == UK_TAB) { s_choosing = -1; pick(+1); }
            else if (k > 32 && k < 127) choose_letter(f, k);
            continue;
        }

        if (s_typing >= 0) {             /* into a box on the page          */
            PageField *f = &s_pg.fields[s_typing];
            if (k == UK_ESC) s_typing = -1;
            else if (k == UK_TAB) { s_typing = -1; pick(+1); }
            else if (k == UK_BKSP) unchar(f);
            else if (k == UK_ENTER) {
                if (f->kind == FK_AREA) type_char(f, '\n');
                else { s_typing = -1; submit(f); }
            } else type_char(f, k);
            continue;
        }

        if (s_editing) {
            if (k == UK_ENTER) {
                s_editing = 0;
                s_edit[s_edit_len] = 0;
                if (s_edit_len) go_typed(s_edit);
            } else if (k == UK_ESC) {
                s_editing = 0;
            } else if (k == UK_SELALL) {
                s_edit_all = s_edit_len > 0;
            } else if (k == UK_LEFT || k == UK_HOME) {
                s_edit_cur = (s_edit_all || k == UK_HOME) ? 0 : s_edit_cur > 0 ? s_edit_cur - 1 : 0;
                s_edit_all = 0;
            } else if (k == UK_RIGHT || k == UK_END) {
                s_edit_cur = (s_edit_all || k == UK_END) ? s_edit_len
                           : s_edit_cur < s_edit_len ? s_edit_cur + 1 : s_edit_len;
                s_edit_all = 0;
            } else if (k == UK_BKSP || k == UK_DEL) {
                if (s_edit_all) { s_edit_len = s_edit_cur = 0; s_edit_all = 0; }
                else if (k == UK_BKSP && s_edit_cur > 0) { edit_cut(s_edit_cur - 1); s_edit_cur--; }
                else if (k == UK_DEL && s_edit_cur < s_edit_len) edit_cut(s_edit_cur);
            } else if (k >= 32 && k < 127) {
                if (s_edit_all) { s_edit_len = s_edit_cur = 0; s_edit_all = 0; }
                if (s_edit_len < PAGE_HREF_MAX - 1) {
                    memmove(s_edit + s_edit_cur + 1, s_edit + s_edit_cur, (u32)(s_edit_len - s_edit_cur));
                    s_edit[s_edit_cur++] = (char)k;
                    s_edit_len++;
                }
            }
            continue;
        }

        /* Esc, or q, asked whether to leave: only y does.  The laptop's
         * esc key can bounce, so a second esc must not be the answer. */
        if (s_leave_armed) {
            s_leave_armed = 0;
            if (k == 'y' || k == 'Y') { music_stop(); term_win_close(); return; }
            say("Staying.", C_DIM);
            continue;
        }

        if (k != 'd') s_del_armed = 0;
        switch (k) {
        case UK_ESC: case 'q':
            s_leave_armed = 1;
            say("Leave browzer? \1y\1 leaves; any other key stays.", C_ACCENT);
            break;
        case UK_UP:    scroll(-1); break;
        case UK_DOWN:  scroll(+1); break;
        case UK_PGUP:  scroll(-(s_vh - 1)); break;
        case UK_PGDN: case ' ': scroll(s_vh - 1); break;
        case UK_HOME:  s_top = 0; s_sel = 0; break;
        case UK_END:   scroll(s_pg.lines); break;
        case UK_TAB:   pick(+1); break;
        case UK_BTAB:  pick(-1); break;
        case UK_ENTER: follow(); break;
        case UK_BKSP: case UK_LEFT: back(); break;
        case 'g': case '/': edit_start(); break;
        case 'r': if (s_pg.url[0]) { char u[PAGE_HREF_MAX + 16]; int i;
                      for (i = 0; s_pg.url[i]; i++) u[i] = s_pg.url[i];
                      u[i] = 0; go(u); } break;
        case 'h': push_history(); home(); break;
        case 's':
            if (s_music) { music_stop(); say("The music has stopped.", C_DIM); }
            break;
        case 'b': mark_this(); break;
        case 'm': push_history(); marks_page(); s_top = 0; s_sel = 0; break;
        case 'p': case 'd':
            if (on_marks_page()) mark_change(k);
            break;
        case 'a':                        /* anyway: past a failed proof     */
            if (s_pg.untrusted && s_pg.url[0]) {
                char u[PAGE_HREF_MAX + 16]; int i;
                for (i = 0; s_pg.url[i]; i++) u[i] = s_pg.url[i];
                u[i] = 0;
                A->conn_check(0);
                go(u);
                A->conn_check(1);
            }
            break;
        default: break;
        }
    }
}

/* ---- the app ---------------------------------------------------------------- */
/* browzer [address or words], or browzer fetch address: the words after
 * it are where to go. */
int app_main(const Api *api, int argc, char **argv)
{
    static char start[PAGE_HREF_MAX];
    int a, i, n = 0, fetch = 0, first = 1;

    A = api;
    __asm__ __volatile__("fninit");      /* the songs use the FPU; nothing else here does */
    /* `browzer fetch address`: the page read out into the terminal, not
     * shown in a window. */
    if (argc >= 2 && argv[1][0] == 'f' && argv[1][1] == 'e' && argv[1][2] == 't' &&
        argv[1][3] == 'c' && argv[1][4] == 'h' && !argv[1][5]) {
        fetch = 1;
        first = 2;
    }
    for (a = first; a < argc; a++) {
        if (a > first && n < PAGE_HREF_MAX - 1) start[n++] = ' ';
        for (i = 0; argv[a][i] && n < PAGE_HREF_MAX - 1; i++) start[n++] = argv[a][i];
    }
    start[n] = 0;

    if (fetch) {
        if (n) http_fetch(start);
        else { A->colour(A_TEXT); A->print("Fetch what?  browzer fetch example.com"); A->newline(); }
        return 0;
    }
    browzer_run(start);
    return 0;
}
