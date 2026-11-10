/* ===========================================================================
 * web.h -- what browzer's parts share.
 *
 * browzer was part of the kernel until 1.15, and its code still says
 * term_win_put and C_TEXT and main_pump.  Those names are kept, and here
 * they mean the app interface's own (app.h), which lays the screen, the
 * keys and the terminal out exactly as the kernel does -- apps.c checks
 * every one of these numbers against the kernel's when it is compiled.
 *
 * The network is the kernel's: conn_open and friends give a connection,
 * plain or secure, already checked.  Everything above that -- HTTP,
 * reading HTML, laying a page out, forms -- is browzer's, here.
 * ===========================================================================
 */
#ifndef WEB_H
#define WEB_H
#include "app.h"

extern const Api *A;
typedef u32 usize;
void *memcpy(void *d, const void *s, u32 n);    /* crt.c */
void *memset(void *d, int c, u32 n);
void *memmove(void *d, const void *s, u32 n);

/* ---- the kernel's names, as the app interface has them ------------------ */
#define C_TEXT     A_TEXT
#define C_DIM      A_DIM
#define C_BRIGHT   A_BRIGHT
#define C_ACCENT   A_ACCENT
#define C_GREY     A_GREY
#define WIN_PAPER  A_PAPER
#define WIN_HILITE A_HILITE
#define PAL_BANANA (A_BANANA - 6)

#define FR_H       A_FR_H
#define FR_V       A_FR_V
#define FR_TL      A_FR_TL
#define FR_TR      A_FR_TR
#define FR_BL      A_FR_BL
#define FR_BR      A_FR_BR
#define FR_TEE_L   A_FR_TEE_L
#define FR_TEE_R   A_FR_TEE_R
#define CH_FILL    A_CH_FILL
#define CH_ARING   A_CH_ARING
#define CH_ADIA    A_CH_ADIA
#define CH_ODIA    A_CH_ODIA
#define CH_ARING_L A_CH_ARING_L
#define CH_ADIA_L  A_CH_ADIA_L
#define CH_ODIA_L  A_CH_ODIA_L

#define UK_UP    K_UP
#define UK_DOWN  K_DOWN
#define UK_LEFT  K_LEFT
#define UK_RIGHT K_RIGHT
#define UK_ENTER K_ENTER
#define UK_ESC   K_ESC
#define UK_BKSP  K_BKSP
#define UK_TAB   K_TAB
#define UK_BTAB  K_BTAB
#define UK_PGUP  K_PGUP
#define UK_PGDN  K_PGDN
#define UK_HOME  K_HOME
#define UK_END   K_END
#define UK_DEL   K_DEL

#define fmt_str(...)               A->format(__VA_ARGS__)
#define term_win_open(w, h)        A->win_open(w, h)
#define term_win_close()           A->win_close()
#define term_win_width()           A->win_width()
#define term_win_height()          A->win_height()
#define term_win_put(x, y, c, f, b) A->win_put(x, y, c, f, b, 0)
#define term_win_put_at            A->win_put
#define term_win_text(x, y, t, f, b) A->win_text(x, y, t, f, b)
#define term_win_band(y, a, b, c)  A->win_band(y, a, b, c)
#define term_win_frame(t, f)       A->win_frame(t, f)
#define term_cols()                A->screen_cols()
#define term_rows()                A->screen_rows()
#define term_print(t)              A->print(t)
#define term_newline()             A->newline()
#define term_colour(c)             A->colour(c)
#define term_putc(c)               A->putc(c)
#define term_plain(on)             A->plain(on)
#define main_pump(n)               A->pump(n)
#define ms_now()                   A->ms()

/* A key, with anything held with Ctrl or Alt let go of, as menus want. */
int ui_key(void);

/* ---- addresses (url.c) ---------------------------------------------------- */
#define URL_PATH_MAX 200
typedef struct { char host[64]; char path[URL_PATH_MAX]; u16 port; int secure; } Url;
int    url_parse(const char *u, Url *out);
void   url_text(const Url *u, char *out, int max);
int    url_resolve(const char *base, const char *href, char *out, int max);

/* ---- a page ------------------------------------------------------------------ */
#define PAGE_LINKS    240                /* links and form fields, together */
#define PAGE_HREF_MAX 200
#define PAGE_FORMS    24
#define PAGE_FIELDS   64
#define PAGE_POOL     65536              /* field names and values          */
#define PAGE_OPTIONS  512                /* the choices in drop-down lists  */

/* One character of the page: what it is, its colour, and which link or
 * field it belongs to (0 for none). */
typedef struct { u8 ch, col; u16 link; } PageCell;

/* A <form>: where it is sent, and how. */
typedef struct { char action[PAGE_HREF_MAX]; u8 post; } PageForm;

/* The kinds of field browzer understands. */
enum { FK_TEXT = 1, FK_HIDDEN, FK_SUBMIT, FK_AREA, FK_PASSWORD, FK_CHECK, FK_RADIO, FK_SELECT };

/* One choice in a drop-down list: what is sent, and what is shown. */
typedef struct { u32 val, vlen, lab, llen; } PageOption;

/* A field of a form.  Its name and value are bytes in the page's pool,
 * UTF-8, as they will be sent; a box that can be typed into has room to
 * grow up to vcap.  A field that can be picked has a link number, and its
 * box is on the page at line, col, width cells wide. */
typedef struct {
    u8  form, kind;
    u8  on;                              /* a tick box or choice: ticked    */
    u16 link;
    u16 opt0, nopt, sel;                 /* a list: its choices, the one picked */
    u16 line, col, width;
    u32 name, nlen;
    u32 val, vlen, vcap;
} PageField;

typedef struct {
    PageCell *cells;                     /* max_lines rows of width         */
    int   width, max_lines;
    int   lines, col, full;
    char  title[64];
    char  (*href)[PAGE_HREF_MAX];        /* PAGE_LINKS addresses            */
    int   nlinks;
    char  url[PAGE_HREF_MAX + 16];
    char  note[128];
    char  who[48];                       /* who vouched for a secure page   */
    int   checked, untrusted;
    u8    note_col;
    int   status, secure, pq;
    u32   bytes;
    void  (*progress)(void);

    PageForm  *forms;                    /* PAGE_FORMS                      */
    int        nforms;
    PageField *fields;                   /* PAGE_FIELDS                     */
    int        nfields;
    char      *pool;                     /* PAGE_POOL bytes                 */
    u32        pool_used;
    u16       *link_field;               /* PAGE_LINKS + 1: link -> field+1 */
    PageOption *opts;                    /* PAGE_OPTIONS                    */
    int        nopts;
} Page;

void   page_clear(Page *pg);
int    http_page(const char *url, Page *pg);
int    http_load(const char *url, Page *pg, const u8 *post, int postlen);
void   http_page_html(const char *html, Page *pg);
void   http_fetch(const char *url);      /* into the terminal, as text      */
void   page_field_show(Page *pg, int f); /* redraw a box after typing       */

void   browzer_run(const char *start);

/* What a page that is not a page is handed to.  browzer sets these; the
 * reader calls them.  start: its type, address, length if the server
 * said (else 0), and for radio the metadata interval and station name;
 * 1 to take the bytes.  bytes: 0 to stop.  end: 1 if all of it came,
 * 0 if not, -1 if esc stopped it. */
extern int  (*web_media_start)(const char *ctype, const char *url, u32 length,
                               u32 metaint, const char *station);
extern int  (*web_media_bytes)(const u8 *p, int n);
extern void (*web_media_end)(int whole);

#endif
