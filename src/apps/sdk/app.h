/* ===========================================================================
 * app.h -- what an app on the floppy can ask of FLOPOS.
 *
 * An app is a flat program linked to run at APP_BASE, with a header at the
 * front saying what it is.  `write` with no built-in of that name makes
 * the kernel look for WRITE.APP on the floppy, read it into memory above
 * everything the kernel uses, and call its entry point with a table of
 * the kernel's services.  The app uses only those; it never links against
 * the kernel itself, so the kernel can change underneath it as long as
 * this table does not.  When the table has to change, APP_API_VERSION
 * goes up and older apps are refused with a reason rather than run blind.
 *
 * This header is included by apps and by the kernel's loader.  Everything
 * here that mirrors a kernel constant is checked against it when the
 * kernel is compiled, so the two cannot drift apart unnoticed.
 * ===========================================================================
 */
#ifndef APP_H
#define APP_H

#ifndef FLOPOS_H
typedef unsigned char       u8;
typedef unsigned short      u16;
typedef unsigned int        u32;
typedef signed int          s32;
typedef unsigned long long  u64;
#endif

#define APP_API_VERSION 12
#define APP_BASE        0x00C00000u      /* 12MB: two whole 4MB pages, the app's own */
#define APP_MAX         0x00800000u      /* 8MB: code, data, buffers and the stack */
#define APP_STACK       0x00040000u      /* the top 256KB: its stack, and a page the system fills in */

typedef struct {
    char magic[8];                       /* "FLOPAPP" and a zero           */
    u32  api_version;
    u32  load_addr;                      /* APP_BASE                       */
    u32  image_end;                      /* where the file's bytes stop    */
    u32  bss_end;                        /* where its zeroed memory stops  */
    u32  entry;                          /* int app_main(const Api *, ...) */
    char name[16];
} AppHeader;

typedef struct { u32 first, size; } AppFile;

/* A thing in the inventory, as inv_list and inv_stat give it.  The date
 * and time are as FAT12 keeps them: year-1980 in the top 7 bits of date,
 * then month (4 bits) and day (5); hours in the top 5 of time, then
 * minutes (6) and seconds/2 (5). */
typedef struct { char name[13]; u8 folder; u16 date, time; u32 size; } InvItem;
#define INV_PATH 80                      /* a path's longest, with its zero */

/* Colours: palette indices, the same ones the shell uses. */
#define A_TEXT    94                     /* PAL_TEXT + 22                  */
#define A_DIM     80                     /* PAL_TEXT + 8                   */
#define A_BRIGHT  130                    /* PAL_BRIGHT + 26                */
#define A_ACCENT  158                    /* PAL_ACCENT + 22                */
#define A_GREY    242                    /* PAL_GREY + 10                  */
#define A_NEON    184                    /* 184..199: the frame's loop of colours, which turns by itself */
#define A_NEON_N  16
#define A_KEY     55                     /* PAL_KEY: a key to press, pink  */
#define A_CREAM   48                     /* fixed colours, whatever the mood: cream, for text */
#define A_OLIVE   49                     /* a dim gold, for quiet text      */
#define A_GOLD    50
#define A_EMBER   51                     /* a deep red                      */
#define A_GREEN_D 52                     /* a dark green, to lie behind text */
#define A_ON      53                     /* PAL_ON: a setting that is on, green */
#define A_RED     54                     /* PAL_RED                         */
#define A_KEYMARK "\1"                    /* in text: pink on, or off again  */
#define A_BANANA  254                    /* PAL_BANANA + 6                 */
#define A_PAPER   65                     /* a window's own background      */
#define A_HILITE  236                    /* a highlighted row              */
#define A_SELECT  140                    /* selected text, behind it       */

/* How a character in a window is drawn, beyond its colour. */
#define AT_BOLD      0x01
#define AT_ITALIC    0x02
#define AT_UNDERLINE 0x04

/* Frame pieces: a code with the arms it puts out in its low bits. */
#define A_FR_H    0x9C
#define A_FR_V    0x93
#define A_FR_TL   0x9A
#define A_FR_TR   0x96
#define A_FR_BL   0x99
#define A_FR_BR   0x95
#define A_FR_TEE_L 0x9B
#define A_FR_TEE_R 0x97

/* The Swedish letters, as the keyboard produces them and the font draws them. */
#define A_CH_ARING   0x80                /* Å */
#define A_CH_ADIA    0x81                /* Ä */
#define A_CH_ODIA    0x82                /* Ö */
#define A_CH_ARING_L 0x86                /* å */
#define A_CH_ADIA_L  0x87                /* ä */
#define A_CH_ODIA_L  0x88                /* ö */
#define A_CH_FILL    0xB3                /* a whole cell, filled          */

/* The other keyboards' letters (German, French, Spanish, Italian, the
 * pound): from 0xB4 up a letter's code is its Latin-1 number, é is 0xE9.
 * Six signs whose Latin-1 numbers fall among the frames get these: */
#define A_CH_IEXCL   0x8A                /* inverted exclamation          */
#define A_CH_POUND   0x8B
#define A_CH_EURO    0x8C
#define A_CH_SECTION 0x8D
#define A_CH_DIAER   0x8E                /* a diaeresis on its own        */
#define A_CH_DEGREE  0x8F
#define A_CH_LATIN   0xB4

/* Which Latin-1 numbers the font draws; the kernel checks these against
 * the font when it is built. */
#define A_LATIN_MASK0 0x8031019Au
#define A_LATIN_MASK1 0x9F5EFFF7u
#define A_LATIN_MASK2 0x9F5EFFF7u

/* Characters past one byte (Romanian, Cyrillic, Hebrew) are their
 * Unicode numbers, from 0x100 up.  In a string such a character is four
 * bytes: A_CH_WIDE_ESC, then the number in three bytes of seven bits, each
 * with its top bit set.  The kernel's win_text and format understand them;
 * a_ch_next and a_ch_put read and write them. */
#define A_CH_WIDE     0x100
#define A_CH_WIDE_ESC 0x1F

static inline int a_ch_next(const char **ps)
{
    const unsigned char *p = (const unsigned char *)*ps;
    int c = p[0];
    if (!c) return 0;
    if (c == A_CH_WIDE_ESC && (p[1] & 0x80) && (p[2] & 0x80) && (p[3] & 0x80)) {
        *ps += 4;
        return ((p[1] & 0x7F) << 14) | ((p[2] & 0x7F) << 7) | (p[3] & 0x7F);
    }
    *ps += 1;
    return c;
}

static inline int a_ch_put(char *d, int c)
{
    if (c < A_CH_WIDE) { d[0] = (char)c; return 1; }
    d[0] = A_CH_WIDE_ESC;
    d[1] = (char)(0x80 | ((c >> 14) & 0x7F));
    d[2] = (char)(0x80 | ((c >> 7) & 0x7F));
    d[3] = (char)(0x80 | (c & 0x7F));
    return 4;
}

/* A character as its Unicode number (the euro as 0x20AC), or 0 if it is
 * not a letter or sign of the ones above.  A wide one is its own number. */
static inline unsigned a_unicode(int c)
{
    static const unsigned short moved[6] = { 0xA1, 0xA3, 0x20AC, 0xA7, 0xA8, 0xB0 };
    if (c >= A_CH_WIDE && c < 0x10000) return (unsigned)c;
    c &= 0xFF;
    switch (c) {
    case A_CH_ARING: return 0xC5;   case A_CH_ADIA: return 0xC4;   case A_CH_ODIA: return 0xD6;
    case A_CH_ARING_L: return 0xE5; case A_CH_ADIA_L: return 0xE4; case A_CH_ODIA_L: return 0xF6;
    }
    if (c >= A_CH_IEXCL && c <= A_CH_DEGREE) return moved[c - A_CH_IEXCL];
    if (c >= A_CH_LATIN) return (unsigned)c;
    return 0;
}

/* The other way: a Unicode number as a character, or 0.  Past one byte
 * every number (up to 0xFFFF) is kept, whether the font draws it or not:
 * a letter it lacks shows as '?' and still goes back out as it came. */
static inline int a_from_unicode(unsigned cp)
{
    static const unsigned mask[3] = { A_LATIN_MASK0, A_LATIN_MASK1, A_LATIN_MASK2 };
    switch (cp) {
    case 0xC5: return A_CH_ARING;   case 0xC4: return A_CH_ADIA;   case 0xD6: return A_CH_ODIA;
    case 0xE5: return A_CH_ARING_L; case 0xE4: return A_CH_ADIA_L; case 0xF6: return A_CH_ODIA_L;
    case 0xA1: return A_CH_IEXCL;   case 0xA3: return A_CH_POUND;  case 0x20AC: return A_CH_EURO;
    case 0xA7: return A_CH_SECTION; case 0xA8: return A_CH_DIAER;  case 0xB0: return A_CH_DEGREE;
    }
    if (cp >= A_CH_WIDE && cp < 0x10000 && cp != 0x20AC) return (int)cp;
    if (cp < A_CH_LATIN || cp > 0xFF) return 0;
    return (mask[(cp - 0xA0) / 32] >> ((cp - 0xA0) % 32) & 1) ? (int)cp : 0;
}

/* Keys, as key() returns them; anything else is the character itself.
 * They sit past the last Unicode number, so no character is ever one. */
#define K_UP    0x110001
#define K_DOWN  0x110002
#define K_LEFT  0x110003
#define K_RIGHT 0x110004
#define K_ENTER 0x110005
#define K_ESC   0x110006
#define K_BKSP  0x110007
#define K_TAB   0x110008
#define K_BTAB  0x110009
#define K_PGUP  0x11000A
#define K_PGDN  0x11000B
#define K_HOME  0x11000C
#define K_END   0x11000D
#define K_DEL   0x11000E
#define M_SHIFT 0x0200
#define M_CTRL  0x0400
#define M_ALT   0x0800

/* What a connection turned out to be (conn_info). */
typedef struct {
    int  secure, pq, checked, untrusted;
    char who[48];                        /* the authority that vouched      */
    char cipher[24];
} ConnInfo;

typedef struct {
    u32 version;

    /* A window over the terminal, in character cells. */
    int  (*win_open)(int w, int h);
    void (*win_close)(void);
    int  (*win_width)(void);
    int  (*win_height)(void);
    void (*win_put)(int x, int y, int ch, u8 fg, u8 bg, u8 attr);
    int  (*win_text)(int x, int y, const char *t, u8 fg, u8 bg);
    void (*win_band)(int y, int x0, int x1, u8 bg);
    void (*win_frame)(const char *title, u8 fg);
    int  (*screen_cols)(void);
    int  (*screen_rows)(void);

    /* The scrolling terminal, for apps that only print. */
    void (*print)(const char *t);
    void (*newline)(void);
    void (*colour)(u8 c);

    /* Keys, time, and keeping the machine alive while waiting. */
    int  (*key)(int *mods);              /* -1 if nothing is waiting       */
    void (*pump)(int draw_now);
    u32  (*ms)(void);

    /* Files on the floppy.  Writing is in place only: a file keeps the
     * size it was made with, and is written exactly that long. */
    int  (*file_find)(const char *name, AppFile *f);
    int  (*file_read)(const AppFile *f, u8 *buf, u32 max);
    int  (*file_write)(const AppFile *f, const u8 *buf);
    void (*file_each)(void (*fn)(const char *name, u32 size, void *ctx), void *ctx);

    /* Text. */
    void (*format)(char *out, u32 cap, const char *fmt, ...);

    /* ---- version 2 ------------------------------------------------------ */
    const char *(*os_version)(void);     /* "1.15.0"                        */
    void (*putc)(int c);                 /* one character to the terminal   */
    void (*plain)(int on);               /* the terminal: lines left alone  */
    void (*wire)(const char *t);         /* down the serial cable, if any   */

    /* The network.  net_online brings the card up and finds an address
     * if it has to.  One connection at a time: conn_open looks the host
     * up, connects, and if secure agrees on keys and makes the server
     * prove who it is, telling note how it is going.  On failure,
     * conn_why() is a whole sentence to show.  conn_read returns bytes,
     * 0 when the other end has finished, -1 when it went quiet for
     * wait_ms, -2 when it broke.  conn_check(0) lets the next secure
     * connection through without the proof, for "go there anyway". */
    int  (*net_online)(void);
    int  (*conn_open)(const char *host, u16 port, int secure, void (*note)(const char *t));
    int  (*conn_write)(const u8 *d, int n);
    int  (*conn_read)(u8 *b, int n, int wait_ms);
    void (*conn_close)(void);
    const char *(*conn_why)(void);
    void (*conn_info)(ConnInfo *ci);
    void (*conn_check)(int on);

    /* ---- version 3 ------------------------------------------------------ */
    /* Recorded sound: 8-bit unsigned mono samples, queued and played by
     * the Sound Blaster.  audio_open asks for a rate and returns the one
     * it got (0: no card that plays samples).  The queue holds about six
     * seconds; audio_write takes what fits and says how much that was. */
    int  (*audio_open)(int rate);
    int  (*audio_space)(void);
    int  (*audio_queued)(void);
    int  (*audio_write)(const u8 *p, int n);
    void (*audio_close)(void);

    /* ---- version 4 ------------------------------------------------------ */
    /* A refill routine for the queue above, called by the system timer --
     * at interrupt time, the way the Mac's Sound Manager called its
     * players' -- whenever the queue has room, whatever the app is doing
     * meanwhile.  It may use audio_space, audio_queued, audio_write and
     * ms, and its own memory; nothing else.  It should come back within
     * about 4 ms, so no frame of the screen waits long on it; it is called
     * again, a thousand times a second, while there is room.  The floating-point unit is saved around it.  0 stops it;
     * audio_close stops it too. */
    void (*audio_feeder)(void (*fn)(void));

    /* ---- version 5 ------------------------------------------------------ */
    /* Listening: the card records instead of playing (never both at
     * once).  source is 1 for the line-in socket, 0 for the microphone;
     * audio_listen returns the rate it got, 0 if it cannot.  What comes
     * in is kept for about six seconds: audio_heard hands over up to max
     * samples of it, oldest first, and says how many.  audio_close ends
     * it. */
    int  (*audio_listen)(int rate, int source);
    int  (*audio_heard)(u8 *out, int max);

    /* ---- version 6 ------------------------------------------------------ */
    /* The inventory: the one folder on the floppy that is the user's, and
     * everything below it.  Here files and folders are really made,
     * changed, moved and removed; any other computer sees them as plain
     * files in a folder called INVENTORY.  A path is names with / between
     * them, down from that folder ("" is the folder itself, "NOTES/A.MD"
     * a file in a folder); a name is up to eight letters or digits, a dot
     * and up to three more; folders go seven deep.  No path can leave the
     * inventory.  Each call that can fail returns 0 (or -1 where it
     * returns a count), and inv_why() is then a sentence to show.
     *
     * inv_list calls fn for each thing in a folder and returns how many;
     * fn must not call the inventory itself.  inv_write makes the file or
     * replaces it, at exactly n bytes.  inv_remove takes a file, or a
     * folder with nothing in it.  inv_move renames, moves, or both: `to`
     * is the whole new path.  inv_room is the bytes free on the floppy. */
    int  (*inv_list)(const char *dir, void (*fn)(const InvItem *it, void *ctx), void *ctx);
    int  (*inv_stat)(const char *path, InvItem *it);
    int  (*inv_read)(const char *path, u8 *buf, u32 max);
    int  (*inv_write)(const char *path, const u8 *buf, u32 n);
    int  (*inv_mkdir)(const char *path);
    int  (*inv_remove)(const char *path);
    int  (*inv_move)(const char *from, const char *to);
    u32  (*inv_room)(void);
    const char *(*inv_why)(void);

    /* What to run when this app has left: an app's name and its words,
     * as they would be typed ("scribe NOTES/A.MD").  That is how one app
     * hands over to another; only apps can be named. */
    void (*then)(const char *command);

    /* ---- version 7 ---- */
    /* The frames' turning colours instead of neon, until the app leaves or
     * turns it off: 1 red, gold and green, with keys in yellow instead of
     * pink; 2 ice: cyan, teal and blue, a glint of silver, black. */
    void (*win_ember)(int on);

    /* ---- version 8: sealing and opening ---- */
    /* A passphrase made into a 32-byte key: PBKDF2-HMAC-SHA256 over the
     * salt, iters times (the more, the dearer each guess).  Then sealing:
     * out gets n bytes and tag 16; anything altered afterwards, or the
     * wrong key, makes crypt_open return 0 and give nothing.  A nonce
     * must never be used twice with one key.  alg is CRYPT_*. */
    void (*crypt_key)(const char *pass, const u8 *salt, u32 slen, u32 iters, u8 key[32]);
    void (*crypt_seal)(int alg, const u8 key[32], const u8 nonce[12], const u8 *aad, u32 alen,
                       const u8 *in, u32 n, u8 *out, u8 tag[16]);
    int  (*crypt_open)(int alg, const u8 key[32], const u8 nonce[12], const u8 *aad, u32 alen,
                       const u8 *in, u32 n, const u8 tag[16], u8 *out);
    void (*random)(u8 *out, u32 n);          /* bytes nobody can guess */
    /* ---- version 9: the clock, and the font's letters ---- */
    /* The date and time: year less 2000, month, day, hour, minute, second.
     * 0 if the clock cannot be read. */
    int  (*clock)(u8 out[6]);
    /* A letter's shape, for a program that makes a picture: five columns,
     * bit 0 at the top, eight rows.  0 if the font has no such letter. */
    int  (*glyph)(int ch, u8 cols[5]);
    /* ---- version 10: the script room ---- */
    /* A room of its own for a page's JavaScript (room.h): room_open loads
     * it; room_task hands it a task and waits for the answer (its length,
     * or -1 if the room had to be stopped, and room_why says why; it must
     * then be opened again).  While it works, the room's requests come to
     * door(buf, len, cap): the request is in buf, the answer goes there
     * too, and door returns its length, or -1. */
    int  (*room_open)(void);
    int  (*room_task)(const u8 *task, u32 len, u8 *answer, u32 cap, u32 ms,
                      int (*door)(u8 *buf, u32 len, u32 cap), u8 *door_buf, u32 door_cap);
    const char *(*room_why)(void);

    /* ---- version 11: the sound to itself ---- */
    /* hush(1): no background music while this app runs; hush(3): no key
     * sounds either, for sound that must not be disturbed (a tape being
     * written or read).  hush(0) lets them back; leaving the app does too. */
    void (*hush)(int on);

    /* ---- version 12: sealed media, and the floppy's size ---- */
    /* AES-128 in CBC mode, undone: n bytes (a multiple of 16) from in to
     * out, which may be the same.  For radio sealed that way (HLS).  It
     * cannot tell a wrong key; the caller judges what comes out. */
    void (*crypt_cbc)(const u8 key[16], const u8 iv[16], const u8 *in, u32 n, u8 *out);
    /* The bytes the floppy holds for files, in all (inv_room is what is free). */
    u32  (*inv_size)(void);
} Api;

#define CRYPT_CHACHA20_POLY1305 0        /* 256-bit key                     */
#define CRYPT_AES256_GCM        1        /* 256-bit key                     */
#define CRYPT_AES128_GCM        2        /* the first 16 bytes of the key   */

typedef int (*AppMain)(const Api *api, int argc, char **argv);

#endif
