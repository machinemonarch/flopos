/* ===========================================================================
 * FLOPOS -- shared types and the interfaces every module publishes.
 * Freestanding: no libc, no floating point, nothing but what is in here.
 * ===========================================================================
 */
#ifndef FLOPOS_H
#define FLOPOS_H

typedef unsigned char       u8;
typedef signed char         s8;
typedef unsigned short      u16;
typedef signed short        s16;
typedef unsigned int        u32;
typedef signed int          s32;
typedef unsigned long long  u64;
typedef signed long long    s64;
typedef unsigned int        usize;

#define NULL ((void *)0)

#define SCR_W   320
#define SCR_H   200
#define SCR_PIX (SCR_W * SCR_H)

/* Where the boot sector and the 16-bit stub left their notes. */
#define BOOTINFO_DRIVE  (*(volatile u8  *)0x502)
#define BOOTINFO_EXTKB  (*(volatile u32 *)0x504)
#define BOOTINFO_A20    (*(volatile u8  *)0x508)

#define VGA_FB  ((volatile u8 *)0xA0000)

/* ---- font ------------------------------------------------------------- *
 * Slot numbers come from fontmap.h, which tools/build/mkfont.py generates from
 * the glyph art.  What this file owns is the character codes: ASCII for
 * everything ASCII has, and these for what it does not.
 */
#include "gen/fontmap.h"

#define CH_ARING    0x80                 /* A-ring, capital                 */
#define CH_ADIA     0x81                 /* A-diaeresis, capital            */
#define CH_ODIA     0x82                 /* O-diaeresis, capital            */
#define CH_BLOCK    0x83                 /* solid block                     */
#define CH_SOFT     0x84                 /* 50% chequer block               */
#define CH_CARET    0x85                 /* a small left-pointing arrow     */
#define CH_ARING_L  0x86                 /* a-ring, lowercase               */
#define CH_ADIA_L   0x87                 /* a-diaeresis, lowercase          */
#define CH_ODIA_L   0x88                 /* o-diaeresis, lowercase          */
#define CH_NOODLER  0x89                 /* a small figure, arms separate   */

/* The other keyboards' letters use their Latin-1 numbers from 0xB4 up
 * (é is 0xE9).  Below that the frames and shades live, so the six signs
 * whose Latin-1 numbers fall there get codes of their own: */
#define CH_IEXCL    0x8A                 /* inverted exclamation, Latin-1 A1 */
#define CH_POUND    0x8B                 /* pound sign, A3                  */
#define CH_EURO     0x8C                 /* euro sign (Latin-9 puts it at A4) */
#define CH_SECTION  0x8D                 /* section sign, A7                */
#define CH_DIAER    0x8E                 /* a diaeresis on its own, A8      */
#define CH_DEGREE   0x8F                 /* degree sign, B0                 */
#define CH_LATIN    0xB4                 /* Latin-1 from here on            */

/* ---- characters past one byte -------------------------------------------
 * A character is an int.  Below 0x100 the codes are FLOPOS's own, as
 * above: ASCII, the Nordic letters, the frames and shades, Latin-1.  From
 * 0x100 up a code is its Unicode number: Romanian, Cyrillic, Hebrew.
 *
 * In a string (char *), such a character is four bytes: CH_WIDE_ESC and
 * then the number in three bytes of seven bits each, every one with its
 * top bit set, so no byte of it is ever 0, a space or a slash.  ch_next()
 * reads a string a character at a time; ch_put() writes one. */
#define CH_WIDE_ESC 0x1F
#define CH_WIDE     0x100                /* the first Unicode-numbered code */

static inline int ch_next(const char **ps)
{
    const u8 *p = (const u8 *)*ps;
    int c = p[0];
    if (!c) return 0;
    if (c == CH_WIDE_ESC && (p[1] & 0x80) && (p[2] & 0x80) && (p[3] & 0x80)) {
        *ps += 4;
        return ((p[1] & 0x7F) << 14) | ((p[2] & 0x7F) << 7) | (p[3] & 0x7F);
    }
    *ps += 1;
    return c;
}

/* Writes c at d: one byte, or four for a wide one.  Returns how many. */
static inline int ch_put(char *d, int c)
{
    if (c < CH_WIDE) { d[0] = (char)c; return 1; }
    d[0] = CH_WIDE_ESC;
    d[1] = (char)(0x80 | ((c >> 14) & 0x7F));
    d[2] = (char)(0x80 | ((c >> 7) & 0x7F));
    d[3] = (char)(0x80 | (c & 0x7F));
    return 4;
}

/* Characters in a string, which is not its bytes once wide ones are in it. */
static inline int ch_count(const char *s)
{
    int n = 0;
    while (ch_next(&s)) n++;
    return n;
}

/* A character code as its Latin-1 number (the euro as 0xA4), or 0 if it
 * is not one of the letters or signs above. */
static inline int ch_latin1(int c)
{
    static const u8 moved[6] = { 0xA1, 0xA3, 0xA4, 0xA7, 0xA8, 0xB0 };
    c &= 0xFF;
    switch (c) {
    case CH_ARING: return 0xC5;  case CH_ADIA: return 0xC4;  case CH_ODIA: return 0xD6;
    case CH_ARING_L: return 0xE5; case CH_ADIA_L: return 0xE4; case CH_ODIA_L: return 0xF6;
    }
    if (c >= CH_IEXCL && c <= CH_DEGREE) return moved[c - CH_IEXCL];
    if (c >= CH_LATIN) return c;
    return 0;
}

/* The other way: a Latin-1 number (0xA4 for the euro) as the character
 * code the font draws, or 0 if the font has no such letter. */
static inline int ch_from_latin1(unsigned cp)
{
    static const unsigned mask[3] = { LATIN_MASK0, LATIN_MASK1, LATIN_MASK2 };
    switch (cp) {
    case 0xC5: return CH_ARING;   case 0xC4: return CH_ADIA;   case 0xD6: return CH_ODIA;
    case 0xE5: return CH_ARING_L; case 0xE4: return CH_ADIA_L; case 0xF6: return CH_ODIA_L;
    case 0xA1: return CH_IEXCL;   case 0xA3: return CH_POUND;  case 0xA4: return CH_EURO;
    case 0xA7: return CH_SECTION; case 0xA8: return CH_DIAER;  case 0xB0: return CH_DEGREE;
    }
    if (cp < CH_LATIN || cp > 0xFF) return 0;
    return (mask[(cp - 0xA0) / 32] >> ((cp - 0xA0) % 32) & 1) ? (int)cp : 0;
}

extern const u8 g_font5x8[FONT_GLYPHS][5];
extern const u8 g_latin_slot[96];        /* Latin-1 (A0..FF) -> slot, 0 none */
extern const u16 g_wide_code[WIDE_COUNT]; /* wide characters the font has   */
extern const u16 g_wide_slot[WIDE_COUNT]; /* and where each is drawn        */
/* Right to left (draw.c): order[v] is which character of the row is drawn
 * in column v, with BIDI_MIRROR set for a bracket to turn round.  Returns
 * 0, with order left as it is, when nothing in the row is Hebrew. */
#define BIDI_MIRROR 0x8000
int    bidi_order(const u16 *ch, int n, u16 *order);
int    bidi_mirror(int c);
int    glyph_wide(int c);                /* slot, or '?''s if there is none */

/* Character code -> glyph slot, for everything the font can draw. */
int    glyph_index(int c);

/* ---- the palette, carved into ramps ---------------------------------- *
 * Index 0 is always true black.  Everything else belongs to a ramp that
 * runs dark to bright across its span, so code can pick a brightness by
 * adding an offset and can animate by walking one.  mood.c builds it.
 */
#define PAL_MOOD    1                    /*   1..63   the background field  */
#define PAL_MOOD_N  47                   /*   1..47; 48..54 fixed colours, 55 keys, 56..63 the HUD */
#define PAL_CREAM   48                   /*  48       cream, for text       */
#define PAL_OLIVE   49                   /*  49       a dim gold, for quiet text */
#define PAL_GOLD    50                   /*  50       gold                  */
#define PAL_EMBER   51                   /*  51       a deep red            */
#define PAL_GREEN_D 52                   /*  52       a dark green, to lie behind text */
#define PAL_ON      53                   /*  53       a setting that is on, in green */
#define PAL_RED     54                   /*  54       a red, for a bar       */
#define PAL_KEY     55                   /*  55       keys, in pink          */
#define PAL_HUD     56                   /*  56..63   the HUD's brain        */
#define PAL_HUD_N   8
#define PAL_SHADOW  64                   /*  64..71   text drop shadow      */
#define PAL_SHADOW_N 8
#define PAL_TEXT    72                   /*  72..103  body text             */
#define PAL_TEXT_N  32
#define PAL_BRIGHT  104                  /* 104..135  emphasis              */
#define PAL_BRIGHT_N 32
#define PAL_ACCENT  136                  /* 136..167  the complementary hue */
#define PAL_ACCENT_N 32
#define PAL_PROMPT  168                  /* 168..183  prompt, animated      */
#define PAL_PROMPT_N 16
#define PAL_NEON    184                  /* 184..199  frames: a loop of neon */
#define PAL_NEON_N  16
#define PAL_CURSOR  200                  /* 200..231  cursor, animated      */
#define PAL_CURSOR_N 32
#define PAL_GREY    232                  /* 232..247  rules and furniture   */
#define PAL_GREY_N  16
#define PAL_BANANA  248                  /* 248..255  a banana is yellow    */
#define PAL_BANANA_N 8

#define C_SHADOW (PAL_SHADOW + 2)
#define C_DIM    (PAL_TEXT + 8)
#define C_TEXT   (PAL_TEXT + 22)
#define C_BRIGHT (PAL_BRIGHT + 26)
#define C_ACCENT (PAL_ACCENT + 22)
#define C_GREY   (PAL_GREY + 10)
#define C_KEY    PAL_KEY                 /* a key to press, wherever it is named */

/* --- heap.c: a bump allocator over extended memory --------------------- */
void   heap_init(void);
void  *halloc(usize bytes);
void   hgive_back(void *p, usize bytes);   /* only the last one   */
u32    heap_used(void);
u32    heap_total(void);
int    heap_is_low(void);                /* using conventional memory?     */

/* --- fault.c ----------------------------------------------------------- */
void   fatal(const char *what, const char *detail);

/* What an interrupt leaves on the stack (isr.S).  useresp and ss are there
 * only when it came from an app's privilege (cs & 3). */
typedef struct {
    u32 edi, esi, ebp, esp_unused, ebx, edx, ecx, eax;
    u32 vector;
    u32 err;
    u32 eip, cs, eflags;
    u32 useresp, ss;
} Regs;

/* --- room.c: the script room ---------------------------------------- */
int  room_open(void);
int  room_task(const u8 *task, u32 len, u8 *answer, u32 cap, u32 ms, u32 door_fn, u8 *door_buf, u32 door_cap);
const char *room_why(void);
void room_tick(Regs *r);

/* --- wall.c: apps kept to their own memory ------------------------------- */
void   wall_init(void);
int    wall_on(void);
void   wall_protect(u32 ro_end);      /* the app's code read-only while it runs */
void   wall_unprotect(void);
void   wall_map(u32 addr, u32 len);       /* let the system reach memory that is not RAM (the screen) */
void   wall_paging(int on);              /* off and on again round a BIOS call */
int    wall_trap(Regs *r);               /* 1 if it was one of an app's doors  */
void   wall_fault(Regs *r, const char *what);
int    wall_run(u32 entry, int argc, char **argv);
u32    wall_call(u32 fn, u32 a0, u32 a1, u32 a2);     /* into the app and back */
u32    wall_scratch(void);               /* a few hundred bytes of the app's, for handing it things */
u32    wall_string(int slot, const char *s);          /* a string put where the app can read it */
void   wall_kill(const char *why);
int    app_ask(u32 n, const u32 *a, u32 *result);     /* apps.c: one request, checked */
extern int g_irq_depth;
extern const char *g_panic_note;         /* said on the crash screen, if set */

/* --- idt.c ------------------------------------------------------------- */
void   idt_init(void);
void   irq_enable(void);
void   irq_unmask(int irq);

/* --- time.c ------------------------------------------------------------ */
#define TICK_HZ 250
extern volatile u32 g_ticks;
void   mood_ember(int on);             /* frames in red, gold and green, keys in yellow */
extern u8 g_key_colour;                /* what a named key is drawn in (term.c) */
void   mood_neon(u32 t);               /* turns the frames' colours (mood.c) */
extern u32 g_irq_cycles;              /* cycles spent in interrupts (idt.c) */
void   time_init(void);
u32    ms_now(void);
int    rand_health(void);             /* the jitter, measured and said on the wire */
void   delay_ms(u32 ms);
int    spun(u32 start, u32 turn, u32 us);   /* a hardware wait's limit, by cycles */

/* --- kbd.c ------------------------------------------------------------- *
 * Events are a scancode in the low byte plus the modifier state at the
 * moment the key went down.  keymap.c turns one into a character; the line
 * editor reads the scancode directly for the keys that are not characters.
 */
#define KEV_SCAN(e)  ((e) & 0xFF)
#define KEV_EXT      0x0100              /* arrived with an E0 prefix       */
#define KEV_SHIFT    0x0200
#define KEV_CTRL     0x0400
#define KEV_ALT      0x0800
#define KEV_ALTGR    0x1000              /* right Alt on a Swedish board    */
#define KEV_CAPS     0x2000

#define SC_ESC    0x01
#define SC_BKSP   0x0E
#define SC_TAB    0x0F
#define SC_ENTER  0x1C
#define SC_SPACE  0x39
#define SC_HOME   0x47
#define SC_UP     0x48
#define SC_PGUP   0x49
#define SC_LEFT   0x4B
#define SC_RIGHT  0x4D
#define SC_END    0x4F
#define SC_DOWN   0x50
#define SC_PGDN   0x51
#define SC_DEL    0x53
#define SC_F1     0x3B
#define SC_F2     0x3C
#define SC_F3     0x3D

void   kbd_init(void);
int    kbd_event(void);                  /* next event, or -1 if none       */
extern volatile u8 g_ctrl_c;             /* Ctrl-C was pressed: stop what you are saying */
int    kbd_down(u8 scancode);
void   kbd_resync(void);  /* after anything that masked IRQ1 */

/* --- keymap.c ---------------------------------------------------------- */
int    keymap_char(int event);           /* character code, or 0            */
int    keymap_key(int event);            /* the same, accent keys ignored   */
int    keymap_set(const char *code);     /* "se", "us", "de"...: 1 if known */
void   keymap_switch(void);              /* Alt+Shift: Latin, or back       */
int    keymap_on_latin(void);            /* on a second keyboard's Latin side */
int    keymap_has_latin(void);           /* this keyboard has a Latin side  */
const char *keymap_code(void);           /* the one in use                  */
int    keymap_count(void);
const char *keymap_code_at(int i);
const char *keymap_name_at(int i);

/* --- vga.c ------------------------------------------------------------- */
extern u8 *g_back;
void   vga_init(void);
void   vga_wait_retrace(void);
void   vga_present(void);
void   vga_clear(u8 colour);
void   vga_set_palette(const u8 *pal768);
void   vga_keep_composed(void);       /* the background was not redrawn: keep it */
void   vga_present_rows(int y0, int y1);  /* send only these rows to the card */
void   vga_get_palette(u8 *out768);
void   vga_adopt(void);                     /* take over the boot screen     */
void   vga_fade_to(const u8 *pal768, int steps);
void   vga_fade_out(int steps);
int    vga_retrace_works(void);
u32    vga_retrace_polls(void);

/* --- main.c: where the time actually goes, measured on the real machine - */
u32    perf_us(int phase);               /* 0 room 1 text 2 blit 3 wait    */
u32    perf_fps(void);

/* --- draw.c ------------------------------------------------------------ */
void   draw_char(u8 *buf, int x, int y, int c, u8 colour);
void   draw_text(u8 *buf, int x, int y, const char *s, u8 colour);
void   draw_text_shadow(u8 *buf, int x, int y, const char *s, u8 fg, u8 bg);
void   draw_text_outline(u8 *buf, int x, int y, const char *s, u8 fg, u8 bg);
void   draw_char_outline(u8 *buf, int x, int y, int c, u8 fg, u8 bg);
int    text_width(const char *s);
void   draw_rect(u8 *buf, int x, int y, int w, int h, u8 colour);
void   draw_line_clipped(u8 *buf, int x0, int y0, int x1, int y1,
                         u8 colour, int bx, int by, int bw, int bh);
void   draw_frame(u8 *buf, int x, int y, int w, int h, u8 colour);
void   draw_hline(u8 *buf, int x, int y, int w, u8 colour);
void   draw_vline(u8 *buf, int x, int y, int h, u8 colour);
void   draw_shade_box(u8 *buf, int x, int y, int w, int h, u8 sub);

/* --- fmt.c ------------------------------------------------------------- */
char  *fmt_u32(char *dst, u32 v);
char  *fmt_s32(char *dst, s32 v);
char  *fmt_hex(char *dst, u32 v, int digits);
void   fmt_str(char *dst, usize cap, const char *fmt, ...);
void   fmt_vstr(char *dst, usize cap, const char *fmt,
                __builtin_va_list ap);

/* --- mathx.c ----------------------------------------------------------- */
#define SINLEN  1024
#define SINMASK (SINLEN - 1)
extern s16 g_sin[SINLEN];
#define SIN(a) (g_sin[(a) & SINMASK])
#define COS(a) (g_sin[((a) + SINLEN / 4) & SINMASK])

void   mathx_init(void);
u32    rnd(void);
void   rnd_seed(u32 seed);
int    rnd_range(int lo, int hi);
int    isqrt32(u32 v);
int    iatan2(int y, int x);

/* --- term.c ------------------------------------------------------------ *
 * The terminal's grid is not a constant.  Four faces are compiled in and
 * any can be under the terminal while it runs, because the only person who
 * can judge how small text may get is the one looking at the panel.  At
 * 320x200, and then at 800x600:
 *
 *   roomy   53 by 25, 66 by 37   -- generous, and what FLOPOS grew up in
 *   close   80 by 33, 100 by 50  -- the grid text interfaces were designed on
 *   fine    --        133 by 66  -- the large letters at the screen's own pixels
 *   easy    --        100 by 50  -- those 1.4 times the size, shaded: the default
 *
 * adv and pitch are the cell: what one character occupies, ink plus air.
 * Anything drawn to the cell rather than to the ink -- rules, frames,
 * shading -- uses those, which is what lets a frame actually join up.
 */
typedef struct {
    const u8   *glyphs;              /* FONT_GLYPHS entries, w bytes each */
    u8          w, h;                /* the ink box                       */
    u8          adv, pitch;          /* the cell                          */
    u8          cols, rows;          /* what fits in 320x200 from x0      */
    u8          x0;                  /* left margin                       */
    u8          shadow;              /* a drop shadow under every glyph    */
    u8          sharp;               /* one pixel a pixel, even at 800x600 */
    const u16  *lv;                  /* shaded letters, lh rows each, or 0 */
    u8          lh;
    const char *name;
} Face;

extern const Face g_faces[FACE_COUNT];

const Face *term_face(void);
int    term_face_index(void);
int    term_set_face(int n);             /* clamps, returns what took     */
int    term_cols(void);
int    term_rows(void);

/* Frames, rules and shading, drawn to the cell instead of the ink so they
 * meet their neighbours with no seam.  The low four bits of a frame code
 * are the arms it puts out, so a corner is just two of them ORed together
 * and there is no table to keep straight. */
#define CH_FRAME    0x90                 /* 0x90 | arms, single stroke     */
#define CH_FRAME2   0xA0                 /* 0xA0 | arms, double stroke     */
#define FR_UP       0x01
#define FR_DOWN     0x02
#define FR_LEFT     0x04
#define FR_RIGHT    0x08

#define FR_H        (CH_FRAME | FR_LEFT | FR_RIGHT)
#define FR_V        (CH_FRAME | FR_UP   | FR_DOWN)
#define FR_TL       (CH_FRAME | FR_DOWN | FR_RIGHT)
#define FR_TR       (CH_FRAME | FR_DOWN | FR_LEFT)
#define FR_BL       (CH_FRAME | FR_UP   | FR_RIGHT)
#define FR_BR       (CH_FRAME | FR_UP   | FR_LEFT)
#define FR_TEE_L    (CH_FRAME | FR_UP   | FR_DOWN | FR_RIGHT)
#define FR_TEE_R    (CH_FRAME | FR_UP   | FR_DOWN | FR_LEFT)
#define FR_TEE_D    (CH_FRAME | FR_DOWN | FR_LEFT | FR_RIGHT)
#define FR_TEE_U    (CH_FRAME | FR_UP   | FR_LEFT | FR_RIGHT)
#define FR_CROSS    (CH_FRAME | 0x0F)

#define CH_SHADE1   0xB0                 /* a quarter of the cell          */
#define CH_SHADE2   0xB1                 /* half, chequered                */
#define CH_SHADE3   0xB2                 /* three quarters                 */
#define CH_FILL     0xB3                 /* the whole cell                 */

/* True for everything the terminal draws geometrically rather than from a
 * glyph, which is also everything that must reach the edge of its cell. */
#define CH_IS_CELL(c) ((c) >= CH_FRAME && (c) <= CH_FILL)

void   term_init(void);
void   term_clear(void);
void   term_colour(u8 c);
void   term_putc(int c);
void   term_print(const char *s);
void   term_printf(const char *fmt, ...);
void   term_newline(void);
void   term_render(u8 *dst, u32 t);
void   term_mute(int on);
void   term_cursor(int on);              /* the terminal's own, on or off   */
void   term_hide(int on);                /* the shell unseen; windows still drawn */
void   term_plain(int on);
int    term_win_open(int w, int h);
void   term_win_close(void);
int    term_win_covers(void);            /* an opaque window over everything */
int    term_dirty_rows(int *y0, int *y1);  /* pixel rows drawn since last asked */
int    term_dirty_peek(int *y0, int *y1);  /* the same, without forgetting them */
int    term_win_width(void);
int    term_win_height(void);
void   term_win_put(int x, int y, int ch, u8 fg, u8 bg);
void   term_win_put_at(int x, int y, int ch, u8 fg, u8 bg, u8 attr);
#define ATTR_BOLD      0x01
#define ATTR_ITALIC    0x02
#define ATTR_UNDERLINE 0x04
#define ATTR_CLEAR     0x10              /* no paper: the screen shows through */
void   term_win_bare(void);              /* every cell clear, and no shadow */
int    term_win_text(int x, int y, const char *t, u8 fg, u8 bg);
void   term_win_band(int y, int x0, int x1, u8 bg);
void   term_win_frame(const char *title, u8 fg);
#define WIN_PAPER  (PAL_SHADOW + 1)
#define WIN_HILITE (PAL_GREY + 4)
void   term_frame_top(int w, const char *title, int dbl);
void   term_frame_row(int w, const char *text, int dbl);
void   term_frame_bottom(int w, int dbl);
void   term_scroll_view(int lines);      /* +ve looks back through history  */
void   term_scroll_home(void);
int    term_scrolled(void);
int    term_cursor_col(void);
int    term_at_origin(void);
void   term_set_cursor_col(int c);
void   term_quirks(int on);
int    term_quirks_on(void);
void   term_zalgo(int on);
int    term_zalgo_on(void);
void   term_rule(void);                  /* a horizontal rule across a row  */
void   term_input_line(int from_col, const char *s, int caret, u8 colour);

/* --- rtc.c ------------------------------------------------------------- */
int    rtc_read(u8 *hour, u8 *min, u8 *sec,
                u16 *year, u8 *month, u8 *day);
u32    rtc_seconds(void);                /* since 2000; 0 if unreadable    */
void   rtc_set_seconds(u32 s);
int    clock_from_network(void);         /* ask a time server; 1 if set    */
int    rtc_battery_ok(void);              /* the clock chip says its battery is good */
int    rtc_lost_power(void);              /* the BIOS found it had lost power */
int    clock_drift(s32 *secs);            /* how far out the network found it */
int    clock_from_goodbye(void);         /* at boot: no earlier than bye   */
void   clock_remember(void);             /* at bye                         */
int    ip_udp_ask(u32 to, u16 port, const u8 *q, int qlen, u8 *ans, int max, int wait_ms);
/* --- nts.c ---------------------------------------------------------------- */
int    conn_busy(void);
u32    nts_time(void);                   /* UTC seconds since 2000 as trusted, or 0 */
const char *nts_source(void);            /* where the last time came from  */
int    nts_trust(void);                  /* 0 signed only, 1 then the network's, 2 then any */
const char *nts_server(int i);           /* the signed servers, in order   */
int    nts_settings(int trust, const char server[3][64]);   /* kept on the floppy */
int    store_clockset_get(u32 *trust, char server[3][64]);
int    store_clockset_set(u32 trust, const char server[3][64]);
u32    ip_ntp_server(void);              /* the time server DHCP named, or 0 */
u32    store_clock_get(void);
void   store_clock_set(u32 s);

/* --- panel.c ----------------------------------------------------------- */
void   panel_init(void);
int    panel_present(void);              /* is there a flat-panel chip?    */
void   panel_tell(const char *when);        /* its registers, down the wire   */
int    panel_wake(void);                 /* its off switches as they were while lit */
int    panel_wide(void);
void   panel_set_wide(int on);

/* --- arrive.c: the boot, told as it happens --------------------------- *
 * Saffran owns the whole palette while he is up, so the entries text
 * needs are kept back from the photograph rather than taken from it.
 * tools/build/mksplash.py writes them; these name them.  The shadow is entry 0
 * because 0 is what the screen shows where he has not loaded yet.
 */
#define SPL_SHADOW 0
#define SPL_DIM    250
#define SPL_TEXT   253
#define SPL_BRIGHT 254
#define SPL_WHITE  255

void   boot_begin(void);                    /* the clock is running: start   */
void   boot_step(const char *fmt, ...);     /* a step is about to happen     */
void   leave_load(void);                 /* Ozzy, off the floppy, at boot   */
int    leave_begin(void);                /* bye/reboot: Ozzy on the screen  */
void   leave_end(void);
void   boot_result(const char *fmt, ...);   /* ...and what it found          */
void   boot_end(void);                      /* hold, then fade out           */

/* --- wizard.c, cyberdelia.c: the way into the shell -------------------- */
void   wizard_gate(int lock);               /* until a riddle is answered    */

/* --- wizsay.c: the wizard's own words, from WIZARD.WIZ ------------------- */
int    wizsay_init(void);                   /* at boot; 0: he stays silent    */
int    wizsay(const char *typed);           /* one line; 0: nothing was said  */
void   cyberdelia_jingle(void (*each)(void));  /* a secret, found          */
void   cyberdelia_enter(void (*each)(void));   /* the screen ripples you in */

/* Applied to every finished frame, after the text and before it goes to
 * the screen, while set.  w and h are the canvas's own size. */
extern void (*g_frame_fx)(u8 *canvas, int w, int h, u32 t);

/* --- banana.c ---------------------------------------------------------- */
void   banana_draw(u8 *dst, u32 t);
void   banana_draw_big(u8 *dst, int w, int h, u32 t);

/* --- vortex.c ---------------------------------------------------------- */
void   vortex_run(const u8 *pal_to);

/* --- mood.c ------------------------------------------------------------ */
void   mood_init(void);
void   mood_set(int which);
int    mood_get(void);
int    mood_count(void);
const char *mood_name(int which);
void   mood_apply_palette(int fade_steps);
void   mood_set_deep(int on);
int    mood_deep(void);
void   mood_build_palette(u8 *out768);
void   mood_draw(u8 *dst, u32 t);
void   mood_draw_big(u8 *dst, int w, int h, u32 t);   /* straight onto the 800x600 canvas */

/* --- line.c ------------------------------------------------------------ */
#define LINE_MAX 120
void   line_init(void);
void   line_reset(void);
const char *line_text(void);
int    line_len(void);
int    line_caret(void);
/* Feed it a key event.  Returns 1 when the user pressed Enter and the
 * finished line is ready to be read with line_text(). */
int    line_key(int event);
/* Same, but for a character arriving from somewhere other than the
 * keyboard -- the serial line.  Returns 1 when the line is finished. */
int    line_inject(int c);

/* --- shell.c ----------------------------------------------------------- */
void   shell_init(void);
void   shell_banner(void);
void   shell_prompt(void);
void   shell_submit(const char *line);
const char *shell_place(void);

/* --- pcic.c ------------------------------------------------------------ *
 * The PC Card sockets.  A CIS walk hands each tuple to a callback rather
 * than building a list, because the only two things anyone wants from a
 * CIS -- who made this card, and how do I switch it on -- are both single
 * tuples and neither is worth a parser or the memory one would need.
 */
#define CIS_MAX_BODY 64

typedef int (*CisFn)(u8 code, const u8 *body, int len, void *ctx);

int    pcic_probe(void);                 /* sockets found, 0 for none      */
int    pcic_scan(u16 *ports, u8 *ids, int max);
int    pcic_sockets(void);
u16    pcic_port(void);
u8     pcic_ident(int sock);
const char *pcic_kind(u8 id);
int    pcic_present(int sock);
u8     pcic_status(int sock);
int    pcic_power_up(int sock, int io_card);
void   pcic_power_down(int sock);
int    pcic_cis_walk(int sock, CisFn fn, void *ctx);
int    pcic_cis_read(int sock, u32 off, u8 *buf, int len);
void   pcic_attr_write(int sock, u32 off, u8 val);
u8     pcic_attr_read(int sock, u32 off);
int    pcic_map_io(int sock, u16 base, int len);
void   pcic_set_irq(int sock, int irq);
void   pcic_claim(int sock, int on);
int    pcic_claimed(int sock);

/* The tuples worth knowing by name. */
#define CISTPL_VERS_1       0x15         /* who made it, in words          */
#define CISTPL_CONFIG       0x1A         /* where its config registers are */
#define CISTPL_CFTABLE      0x1B         /* what resources it will take    */
#define CISTPL_MANFID       0x20         /* who made it, in numbers        */
#define CISTPL_FUNCID       0x21         /* what kind of card it is        */

/* --- voice.c ----------------------------------------------------------- */
typedef enum {
    V_WHOAMI, V_UNKNOWN, V_HELP_HEAD, V_HELP_FOOT, V_LOOK, V_HERE,
    V_EMPTY, V_BANNER_SUB, V_READY, V_ECHO_NOTHING, V_SOUND_ON,
    V_SOUND_OFF, V_MOOD, V_BYE, V_NOT_YET, V_TIME,
    /* Things said *to* it rather than commands given to it. */
    V_IDENT, V_NAME, V_WELLBEING, V_ALIVE, V_REAL, V_STILL_THERE,
    V_GREETING, V_THANKS, V_SORRY, V_LOVE, V_WHY, V_THERE,
    V_WHAT_NOW, V_QUESTION, V_MORE_OF_WHAT, V_UNKNOWN_PHRASE,
    V_YES, V_NO, V_PLEASE, V_WHAAAT_IN, V_WHAAAT_OUT,
    V_COUNT
} VoiceId;

const char *voice(VoiceId id);

/* Look up something said to FLOPOS, already squashed to lowercase letters
 * and digits.  Returns 1 on a hit.  *runs is set when the phrase should
 * simply run one of the ordinary words instead of answering in prose. */
int  voice_phrase(const char *squashed, VoiceId *says, const char **runs);

/* --- sound.c ----------------------------------------------------------- */
void   snd_init(void);
void   snd_tick(void);
void   snd_toggle(void);
int    snd_enabled(void);

/* --- audio.c: samples through the Sound Blaster ------------------------- */
int    audio_probe(void);
int    audio_open(int rate);              /* -> the rate it got, 0: none   */
int    audio_space(void);
int    audio_queued(void);
int    audio_write(const u8 *p, int n);
void   audio_close(void);
int    audio_listen(int rate, int source); /* record instead: 1 line-in, 0 mic */
int    audio_heard(u8 *out, int max);     /* what has come in since last asked */
void   audio_tick(void);
void   audio_report(void);
int    snd_has_fm(void);
void   snd_set_tempo(int bpm);
int    snd_tempo(void);
void   snd_next_pattern(void);
int    snd_pattern(void);
const char *snd_pattern_name(void);
void   snd_silence(void);
/* Take the speaker and one FM voice away from the sequencer for an effect,
 * sweep them by hand, then give them back. */
void   snd_sweep_begin(void);
void   snd_sweep_set(int hz);
void   snd_sweep_end(void);
void   snd_fx_begin(void);               /* the music steps aside           */
void   snd_fx_square(int hz);            /* a chip-tune note; 0 stops it    */
void   snd_fx_bell(int hz);              /* a bell, struck and left to ring */
void   snd_fx_end(void);
int    snd_gibson(int on);           /* key sounds: 1 on, 0 off, -1 asks */
extern volatile u8 g_tw_key;
extern volatile u8 g_chime;             /* the HUD's chime: 1 opening, 2 closing */
int    hud_shown(void);
void   audio_hit(const u8 *p, u32 n);
int    audio_gibson(int want);       /* the card for gibson, or done with it */
void   snd_key(void);
void   snd_hush(int on);             /* an app has the sound to itself (1) or not */                /* from the timer: play a key left by kbd.c */          /* kbd.c leaves a key for the timer */
extern volatile u8  g_beat;
extern volatile u8  g_bar;
extern volatile u32 g_step;

/* --- net.c -------------------------------------------------------------- */
#define NET_MTU 1514                     /* the biggest ethernet frame     */

typedef struct {
    const char *name;
    int  (*start)(void);
    void (*stop)(void);
    const u8 *(*mac)(void);
    int  (*send)(const u8 *frame, int len);
    int  (*recv)(u8 *frame, int max);
    const char *(*why)(void);            /* why it did not come up         */
    int  (*link)(void);                  /* is anybody at the other end    */
} Nic;

void audio_feeder(void (*fn)(void));     /* an app's refill, at interrupt time */
void audio_tell(u32 fps);                /* now and then, down the wire     */
void audio_deferred(void);               /* from the timer, after its EOI   */
void net_tick(void);                     /* from the timer: empty the card  */

extern const Nic g_nic_el3;
extern const Nic g_nic_ne2000;
extern const u8  g_mac_broadcast[6];

int          net_up(void);
void         net_down(void);
int          net_ready(void);
const char  *net_name(void);
const char  *net_why(void);
const u8    *net_mac(void);
int          net_send(const u8 *frame, int len);
int          net_recv(u8 *frame, int max);
int          net_frame_begin(u8 *f, const u8 *to, u16 type);
int          net_link(void);
const char  *net_state(void);         /* the queue and the card, as words */

/* --- ip.c --------------------------------------------------------------- */
void   ip_configure(u32 addr, u32 mask, u32 gw);
u32    ip_addr(void);
u32    ip_gateway(void);
u32    ip_netmask(void);
void   ip_poll(void);
const u8 *ip_route(u32 to, int wait_ms);
int    ip_ping(u32 to, int seq, int wait_ms);
int    ip_parse(const char *s, u32 *out);
u32    ip_dns(void);
void   ip_set_dns(u32 addr, u16 port);
int    ip_how(void);
int    ip_dhcp(void);
u32    ip_lookup(const char *name);
int    ip_send(u8 proto, u32 to, const u8 *payload, int len);

/* --- tcp.c --------------------------------------------------------------- */
void   tcp_input(u32 from, const u8 *seg, int len);
int    tcp_connect(u32 to, u16 port, int wait_ms);
int    tcp_write(const u8 *data, int len);
int    tcp_read(u8 *buf, int max, int wait_ms);
void   tcp_close(void);
int    tcp_open(void);
int    tcp_done(void);
const char *tcp_why(void);

/* url.c, http.c and browzer.c are BROWZER.APP now (src/apps/browzer). */

/* --- sha256.c ------------------------------------------------------------ */
typedef struct { u32 h[8]; u32 len; u8 buf[64]; int fill; } Sha256;
void   sha256_init(Sha256 *c);
void   sha256_update(Sha256 *c, const u8 *p, u32 n);
void   sha256_final(Sha256 *c, u8 out[32]);
void   sha256(const u8 *p, u32 n, u8 out[32]);
typedef struct { u32 h[5]; u32 len; u8 buf[64]; int fill; } Sha1;
void   sha1_init(Sha1 *c);
void   sha1_update(Sha1 *c, const u8 *p, u32 n);
void   sha1_final(Sha1 *c, u8 out[20]);
void   sha1_waste(int n);                /* n compressions of nothing, for even timing */

/* --- sha512.c: SHA-512 and SHA-384, for checking certificates ---------- */
typedef struct { u64 h[8]; u32 len; u8 buf[128]; int fill, out; } Sha512;
void   sha512_init(Sha512 *c, int is384);
void   sha512_update(Sha512 *c, const u8 *p, u32 n);
void   sha512_final(Sha512 *c, u8 *out);
#define HASH_SHA256 1
#define HASH_SHA384 2
#define HASH_SHA512 3
int    hash_len(int alg);
void   hash_buf(int alg, const u8 *p, u32 n, u8 *out);

/* --- bignum.c: modular arithmetic for signatures ------------------------ */
#define BN_LIMBS 130                     /* 4096 bits, and a word to spare   */
typedef struct { int n, bits; u32 m[BN_LIMBS], rr[BN_LIMBS], one[BN_LIMBS], minv; } Mont;
int    bn_from_be(u32 *a, int n, const u8 *be, int len);
void   bn_to_be(u8 *be, int len, const u32 *a, int n);
int    bn_cmp(const u32 *a, const u32 *b, int n);
int    bn_is_zero(const u32 *a, int n);
u32    bn_add(u32 *r, const u32 *a, const u32 *b, int n);
u32    bn_sub(u32 *r, const u32 *a, const u32 *b, int n);
int    bn_bits(const u32 *a, int n);
int    mont_init(Mont *M, const u8 *mod_be, int len);
void   mont_mul(const Mont *M, u32 *r, const u32 *a, const u32 *b);
void   mont_to(const Mont *M, u32 *r, const u32 *a);
void   mont_from(const Mont *M, u32 *r, const u32 *a);
void   mont_pow(const Mont *M, u32 *r, const u32 *base, const u8 *exp, int elen);
void   mod_add(const Mont *M, u32 *r, const u32 *a, const u32 *b);
void   mod_sub(const Mont *M, u32 *r, const u32 *a, const u32 *b);

/* --- rsa.c, ecdsa.c: is this signature good? ---------------------------- */
int    rsa_verify_pkcs1(const u8 *n, int nlen, const u8 *e, int elen,
                        const u8 *sig, int slen, int alg, const u8 *hash);
int    rsa_verify_pss(const u8 *n, int nlen, const u8 *e, int elen,
                      const u8 *sig, int slen, int alg, const u8 *hash, int salt);
#define CURVE_P256 0
#define CURVE_P384 1
int    ecdsa_verify(int curve, const u8 *pub, int publen,
                    const u8 *sig, int siglen, const u8 *hash, int hlen);

/* --- x509.c: is a secure site who it says it is? ------------------------ */
int    x509_check(const u8 *const *certs, const int *lens, int n, const char *host,
                  char *why, int whycap);
int    x509_check_handshake(int scheme, const u8 *content, int clen, const u8 *sig, int slen);
int    x509_check_handshake12(int scheme, const u8 *content, int clen, const u8 *sig, int slen);
const char *tls_kex(void);              /* the key exchange, as words */
int    ecdh_p256(const u8 secret[32], const u8 *peer, u8 shared[32], u8 pub[65]);
int    ecdh(int curve, const u8 *secret, const u8 *peer, u8 *shared, u8 *pub);   /* CURVE_P256 or CURVE_P384 */
const char *x509_who(void);              /* who vouched, after a good check */
int    x509_roots(void);                 /* how many authorities are trusted */
void   hmac_sha256(const u8 *key, u32 klen, const u8 *msg, u32 mlen, u8 out[32]);
void   hkdf_extract(const u8 *salt, u32 slen, const u8 *ikm, u32 ilen, u8 prk[32]);
void   hkdf_expand(const u8 prk[32], const u8 *info, u32 ilen, u8 *out, u32 olen);

/* --- chacha.c ------------------------------------------------------------ */
typedef struct { u32 r[5], h[5], pad[4]; u8 buf[16]; int fill; } Poly1305;
void   chacha20_block(const u8 key[32], u32 counter, const u8 nonce[12], u8 out[64]);
void   chacha20_xor(const u8 key[32], u32 counter, const u8 nonce[12],
                    const u8 *in, u8 *out, u32 len);
void   poly1305_init(Poly1305 *p, const u8 key[32]);
void   poly1305_update(Poly1305 *p, const u8 *m, u32 n);
void   poly1305_final(Poly1305 *p, u8 tag[16]);
void   aead_seal(const u8 key[32], const u8 nonce[12], const u8 *aad, u32 alen,
                 const u8 *pt, u32 len, u8 *ct, u8 tag[16]);
int    aead_open(const u8 key[32], const u8 nonce[12], const u8 *aad, u32 alen,
                 const u8 *ct, u32 len, const u8 tag[16], u8 *pt);

/* --- crypt.c: sealing and opening, for apps -------------------------------- */
void   pbkdf2_sha256(const u8 *pw, u32 plen, const u8 *salt, u32 slen, u32 iters, u8 out[32]);
void   crypt_key(const char *pass, const u8 *salt, u32 slen, u32 iters, u8 key[32]);
void   crypt_seal(int alg, const u8 key[32], const u8 nonce[12], const u8 *aad, u32 alen,
                  const u8 *in, u32 n, u8 *out, u8 tag[16]);
int    crypt_open(int alg, const u8 key[32], const u8 nonce[12], const u8 *aad, u32 alen,
                  const u8 *in, u32 n, const u8 tag[16], u8 *out);
void   crypt_random(u8 *out, u32 n);
void   aes256gcm_seal(const u8 key[32], const u8 iv[12], const u8 *aad, u32 alen,
                      const u8 *pt, u32 len, u8 *ct, u8 tag[16]);
int    aes256gcm_open(const u8 key[32], const u8 iv[12], const u8 *aad, u32 alen,
                      const u8 *ct, u32 len, const u8 tag[16], u8 *pt);

/* --- aes.c ---------------------------------------------------------------- */
void   aesgcm_seal(const u8 key[16], const u8 iv[12], const u8 *aad, u32 alen,
                   const u8 *pt, u32 len, u8 *ct, u8 tag[16]);
int    aesgcm_open(const u8 key[16], const u8 iv[12], const u8 *aad, u32 alen,
                   const u8 *ct, u32 len, const u8 tag[16], u8 *pt);
void   aes_cbc_encrypt(const u8 *key, int klen, const u8 iv[16], const u8 *in, u8 *out, u32 n);
void   aes_cbc_decrypt(const u8 *key, int klen, const u8 iv[16], const u8 *in, u8 *out, u32 n);
/* AES-SIV-CMAC-256 (RFC 5297), for NTS: ad[] are the associated data then
 * the nonce, in order; seal writes the 16-byte tag then n bytes. */
void   aessiv_seal(const u8 key[32], const u8 *const *ad, const u32 *adlen, int nad,
                   const u8 *p, u32 n, u8 *out);
int    aessiv_open(const u8 key[32], const u8 *const *ad, const u32 *adlen, int nad,
                   const u8 *in, u32 n, u8 *p);

/* --- x25519.c ------------------------------------------------------------ */
void   x25519(u8 out[32], const u8 scalar[32], const u8 point[32]);
void   x25519_public(u8 out[32], const u8 secret[32]);
int    crypto_selftest(void);

/* --- sha3.c --------------------------------------------------------------- */
typedef struct { u64 s[25]; int rate, pos, squeezing; u8 pad; } Keccak;
void   keccak_init(Keccak *k, int rate, u8 pad);
void   keccak_absorb(Keccak *k, const u8 *in, u32 len);
void   keccak_squeeze(Keccak *k, u8 *out, u32 len);
void   sha3_256(const u8 *in, u32 len, u8 out[32]);
void   sha3_512(const u8 *in, u32 len, u8 out[64]);
void   shake128(const u8 *in, u32 len, u8 *out, u32 olen);
void   shake256(const u8 *in, u32 len, u8 *out, u32 olen);

/* --- mlkem.c -------------------------------------------------------------- */
#define MLKEM_EK 1184
#define MLKEM_DK 2400
#define MLKEM_CT 1088
void   mlkem_keygen(const u8 d[32], const u8 z[32], u8 ek[MLKEM_EK], u8 dk[MLKEM_DK]);
void   mlkem_encaps(const u8 ek[MLKEM_EK], const u8 m[32], u8 c[MLKEM_CT], u8 key[32]);
void   mlkem_decaps(const u8 dk[MLKEM_DK], const u8 c[MLKEM_CT], u8 key[32]);

/* --- random.c ------------------------------------------------------------ */
void   rand_add(u32 v);
void   rand_bytes(u8 *out, int n);

/* --- tls.c --------------------------------------------------------------- */
int    tls_connect(const char *host);
int    tls_write(const u8 *data, int len);
int    tls_read(u8 *buf, int max, int wait_ms);
void   tls_close(void);
const char *tls_why(void);
int    tls_post_quantum(void);
void   tls_offer_pq(int on);
int    tls_pq_broke(void);
const char *tls_cipher(void);
void   tls_check(int on);                /* 0: go on without the proof      */
int    tls_untrusted(void);              /* the failure: it could not prove */
int    tls_checked(void);                /* this connection was proved      */
const char *tls_who(void);               /* the authority that vouched      */
void   tls_set_note(void (*fn)(const char *));
void   tls_alpn(const char *proto);      /* ask for this protocol (ALPN), "" none */
int    tls_export(const char *label, const u8 *ctx, int clen, u8 *out, int olen);
int    tls_checking(void);               /* whether certificates are being checked */

/* --- el3.c -------------------------------------------------------------- */
int    el3_start(int sock, u32 cor, u16 io);
void   el3_stop(void);
int    el3_up(void);
u16    el3_io(void);
int    el3_socket(void);
u16    el3_product(void);
u16    el3_maker(void);
const u8 *el3_mac(void);
int    el3_link(void);

/* --- vbe.c ------------------------------------------------------------- */
typedef struct {
    u16 mode;
    u16 attr;
    u16 width, height, pitch;
    u8  bpp, planes, model;
    u16 win_seg;
    u32 lfb;
} VbeMode;

int    vbe_probe(void);
int    vbe_present(void);
u16    vbe_version(void);
u32    vbe_vram_kb(void);
const char *vbe_oem(void);
int    vbe_mode_at(int n);
int    vbe_mode_info(u16 mode, VbeMode *out);
int    vbe_is_graphics(const VbeMode *m);
int    vbe_has_lfb(const VbeMode *m);

int    vga_go_big(void);
void   vga_go_small(void);
int    vga_redo_big(void);
void   vga_redo_small(void);             /* 320x200 set again, as it was     */
int    vga_is_big(void);
void   vga_compose(void);
void   vga_shrink(void);
u8    *vga_canvas(void);
int    vga_width(void);
int    vga_height(void);
int    vga_pitch(void);
int    vga_scale(void);
u16    vga_big_mode(void);

/* --- serial.c ---------------------------------------------------------- */
void   serial_init(void);
int    serial_present(void);
void   serial_putc(int c);
void   serial_print(const char *s);
void   serial_printf(const char *fmt, ...);
int    serial_getc(void);                /* -1 when nothing has arrived    */
u32    serial_sent(void);
u32    serial_received(void);
int    serial_echo(void);
void   serial_set_echo(int on);

/* --- bios.S ------------------------------------------------------------ *
 * Drops to real mode, performs INT 13h, and comes back.  Returns the
 * BIOS's AX, with bit 16 set if it reported failure.  Interrupts are off
 * for the duration, so this is not cheap -- but it is the machine's own
 * disk code, which is the point. */
u32    bios_int(u16 which, u16 ax, u16 bx, u16 cx, u16 dx, u16 es, u16 di);
void   bios_regs(u16 *bx, u16 *cx, u16 *dx);   /* what the last call left in them */
void   pic_to_bios(void);
void   pic_to_kernel(void);
void   clock_credit_cycles(u32 cycles);
void   clock_rebase(void);
void   clock_restart(void);
void   clock_watch(void);
u32    clock_restarts(void);
void   kbd_drain(void);
void   serial_drain(void);
void   serial_irq(void);
void   time_init(void);

/* --- disk.c ------------------------------------------------------------ */
#define DISK_BUF_SEG 0x7000              /* 0x70000: below 1MB, and clear
                                          * of a 64K boundary             */
#define DISK_BUF     ((volatile u8 *)0x70000)
#define VBE_BUF_SEG  0x7200              /* 0x72000: clear of the disk    */
#define VBE_BUF      ((volatile u8 *)0x72000)
int    disk_read(u32 lba, int count);
int    disk_read_run(u32 lba, u32 sectors, u8 *dst);
int    disk_write_run(u32 lba, u32 sectors, const u8 *src);

/* --- fs.c ----------------------------------------------------------------- */
typedef struct { u32 first, size; } FsFile;
int    fs_mount(void);
int    fs_find(const char *name, FsFile *f);
int    fs_read(const FsFile *f, u8 *buf, u32 max);
int    fs_write_inplace(const FsFile *f, const u8 *buf);
int    fs_is_file(const FsFile *f);      /* a real file's place and length  */
void   fs_each(void (*fn)(const char *name, u32 size, void *ctx), void *ctx);

/* The inventory: your own folder on the floppy, where files and folders
 * are made, changed, moved and removed.  Paths are names with / between
 * them, down from that folder; "" is the folder itself. */
typedef struct { char name[13]; u8 folder; u16 date, time; u32 size; } FsItem;
int    inv_list(const char *dir, void (*fn)(const FsItem *it, void *ctx), void *ctx);
int    inv_stat(const char *path, FsItem *it);
int    inv_read(const char *path, u8 *buf, u32 max);      /* bytes, or -1   */
int    inv_write(const char *path, const u8 *buf, u32 n); /* make or replace */
int    inv_mkdir(const char *path);
int    inv_remove(const char *path);
int    inv_move(const char *from, const char *to);
u32    inv_room(void);                    /* bytes free on the floppy       */
u32    inv_size(void);                    /* bytes it holds for files, in all */
const char *inv_why(void);                /* why the last one failed        */    /* -> 1 on success               */
int    disk_write(u32 lba, int count);
u8     disk_last_status(void);

/* --- store.c ----------------------------------------------------------- *
 * Two fixed-size files on the floppy, placed by the build.  The kernel
 * never parses FAT: it is handed a starting sector and a length for each,
 * and overwrites the data in place.
 */
#define TAUGHT_MAX 48
#define WORD_MAX   20
#define RESP_MAX   72

typedef struct {
    char word[WORD_MAX];
    char resp[RESP_MAX];
} Taught;

typedef struct {
    char magic[16];                      /* patched by tools/build/mkimage.py    */
    u32  journal_lba, journal_sectors;
    u32  state_lba,   state_sectors;
} DiskMap;

extern volatile DiskMap g_diskmap;

void   store_init(void);
int    store_ready(void);
int    store_save(void);                 /* -> 1 if everything landed      */
int    store_unsaved(void);
void   store_reload(void);               /* the files changed under us     */
int    store_net_get(u32 *how, u32 *ip, u32 *mask, u32 *gw, u32 *dns);
int    store_net_set(u32 how, u32 ip, u32 mask, u32 gw, u32 dns);
const char *store_keys_get(void);         /* chosen under this release, or 0 */
int    store_keys_set(const char *code);
void   main_pump(int now);

/* --- ui.c --------------------------------------------------------------- */
#define UK_UP    0x110001
#define UK_DOWN  0x110002
#define UK_LEFT  0x110003
#define UK_RIGHT 0x110004
#define UK_ENTER 0x110005
#define UK_ESC   0x110006
#define UK_BKSP  0x110007
#define UK_TAB   0x110008
#define UK_BTAB  0x110009
#define UK_PGUP  0x11000A
#define UK_PGDN  0x11000B
#define UK_HOME  0x11000C
#define UK_END   0x11000D
#define UK_DEL   0x11000E
int    ui_key(void);
int    ui_wait_key(void);
int    ui_key_mods(int *mods);
void   netui_run(void);
void   clockui_run(void);
int    net_online(void);                 /* the network up, with an address */
int    app_run(const char *word, int argc, char **argv);
void   apps_list(void);
int    ip_apply_saved(void);
u32    store_boots(void);

void   store_journal_line(const char *s);
void   store_journal_printf(const char *fmt, ...);
const char *store_journal_text(void);
u32    store_journal_used(void);
u32    store_journal_size(void);


/* --- cpu.c ------------------------------------------------------------- */
extern char g_cpu_vendor[16];
extern char g_cpu_name[52];
extern int  g_has_cpuid;
extern int  g_has_tsc;
extern u32  g_cpu_mhz;
void   cpu_detect(void);
void   cpu_measure_mhz(void);
void   reboot(void);
int    power_off(void);  /* returns only if it could not */

/* --- hud.c: ctrl-shift-f10 ------------------------------------------- */
extern volatile int g_hud_toggle;

/* kbpick.c: Ctrl+Shift+F1, a window to choose the keyboard from. */
extern volatile int g_kbpick_open;
extern volatile int g_screen_again;      /* Ctrl+Shift+F5: switch the screen again */
int    kbpick_takes_keys(void);
void   kbpick_key(int ev);
void   kbpick_frame(u8 *dst, u32 t);
int    kbpick_rect(int *x0, int *y0, int *x1, int *y1);
extern u32 g_idle_cycles;
void   hud_frame(u8 *canvas, u32 t);
u32    perf_us(int phase);
u32    perf_us(int phase);
int    hud_rect(int *x0, int *y0, int *x1, int *y1);
int    hud_takes_keys(void);
void   hud_key(int ev);
void   idle_wait(void);                  /* halt until an interrupt, counted as idle */
int    audio_peek(u8 *out, int n);       /* what the card is playing now    */
int    audio_rate_now(void);             /* 0 while closed                  */
int    app_exists(const char *word);
int    app_active(void);
u32    app_bytes(void);
void   term_tap(void (*fn)(int c));      /* everything printed, to fn too   */
int    term_muted(void);
void   term_refresh(void);               /* a window drawn again, all of it */
void   term_cell_size(int *w, int *h);
int    term_pitch(void);
void   term_overlay_cell(u8 *dst, int px, int py, int c, u8 fg, u8 bg);
void   term_overlay_fill(u8 *dst, int px, int py, int w, int h, u8 colour);
void   vga_set_colours(int first, int n, const u8 *rgb);
void   vga_present_rect(int x0, int y0, int x1, int y1);

#endif /* FLOPOS_H */
