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

#define APP_API_VERSION 1
#define APP_BASE        0x00A00000u      /* 10MB, clear of the kernel heap */
#define APP_MAX         0x00400000u      /* 4MB: code, data and buffers    */

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

/* Colours: palette indices, the same ones the shell uses. */
#define A_TEXT    94                     /* PAL_TEXT + 22                  */
#define A_DIM     80                     /* PAL_TEXT + 8                   */
#define A_BRIGHT  130                    /* PAL_BRIGHT + 26                */
#define A_ACCENT  158                    /* PAL_ACCENT + 22                */
#define A_GREY    242                    /* PAL_GREY + 10                  */
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

/* Keys, as key() returns them; anything else is the character itself. */
#define K_UP    0x101
#define K_DOWN  0x102
#define K_LEFT  0x103
#define K_RIGHT 0x104
#define K_ENTER 0x105
#define K_ESC   0x106
#define K_BKSP  0x107
#define K_TAB   0x108
#define K_BTAB  0x109
#define K_PGUP  0x10A
#define K_PGDN  0x10B
#define K_HOME  0x10C
#define K_END   0x10D
#define K_DEL   0x10E
#define M_SHIFT 0x0200
#define M_CTRL  0x0400
#define M_ALT   0x0800

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
} Api;

typedef int (*AppMain)(const Api *api, int argc, char **argv);

#endif
