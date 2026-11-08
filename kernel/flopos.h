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
 * Slot numbers come from fontmap.h, which tools/mkfont.py generates from
 * the glyph art.  What this file owns is the character codes: ASCII for
 * everything ASCII has, and these for what it does not.
 */
#include "fontmap.h"

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

extern const u8 g_font5x8[FONT_GLYPHS][5];

/* Character code -> glyph slot, for everything the font can draw. */
int    glyph_index(int c);

/* ---- the palette, carved into ramps ---------------------------------- *
 * Index 0 is always true black.  Everything else belongs to a ramp that
 * runs dark to bright across its span, so code can pick a brightness by
 * adding an offset and can animate by walking one.  mood.c builds it.
 */
#define PAL_MOOD    1                    /*   1..63   the background field  */
#define PAL_MOOD_N  63
#define PAL_SHADOW  64                   /*  64..71   text drop shadow      */
#define PAL_SHADOW_N 8
#define PAL_TEXT    72                   /*  72..103  body text             */
#define PAL_TEXT_N  32
#define PAL_BRIGHT  104                  /* 104..135  emphasis              */
#define PAL_BRIGHT_N 32
#define PAL_ACCENT  136                  /* 136..167  the complementary hue */
#define PAL_ACCENT_N 32
#define PAL_PROMPT  168                  /* 168..199  prompt, animated      */
#define PAL_PROMPT_N 32
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

/* --- heap.c: a bump allocator over extended memory --------------------- */
void   heap_init(void);
void  *halloc(usize bytes);
u32    heap_used(void);
u32    heap_total(void);
int    heap_is_low(void);                /* using conventional memory?     */

/* --- fault.c ----------------------------------------------------------- */
void   fatal(const char *what, const char *detail);

/* --- idt.c ------------------------------------------------------------- */
void   idt_init(void);
void   irq_enable(void);
void   irq_unmask(int irq);

/* --- time.c ------------------------------------------------------------ */
#define TICK_HZ 250
extern volatile u32 g_ticks;
void   time_init(void);
u32    ms_now(void);
void   delay_ms(u32 ms);

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
int    kbd_down(u8 scancode);
void   kbd_resync(void);  /* after anything that masked IRQ1 */

/* --- keymap.c ---------------------------------------------------------- */
int    keymap_char(int event);           /* character code, or 0            */

/* --- vga.c ------------------------------------------------------------- */
extern u8 *g_back;
void   vga_init(void);
void   vga_wait_retrace(void);
void   vga_present(void);
void   vga_clear(u8 colour);
void   vga_set_palette(const u8 *pal768);
void   vga_get_palette(u8 *out768);
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
 * The terminal's grid is not a constant.  Two faces are compiled in and
 * either can be under the terminal while it runs, because the only person
 * who can judge how small text may get is the one looking at the panel.
 *
 *   roomy   53 columns by 25 rows   -- generous, and what FLOPOS grew up in
 *   close   80 columns by 33 rows   -- the grid every text interface
 *                                      worth copying was designed on
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

/* --- panel.c ----------------------------------------------------------- */
void   panel_init(void);
int    panel_present(void);              /* is there a flat-panel chip?    */
int    panel_wide(void);
void   panel_set_wide(int on);

/* --- splash.c / arrive.c ----------------------------------------------- *
 * The boot screen owns the whole palette while it is up, so the eight
 * entries text needs are reserved out of the photograph's 256 rather than
 * taken from it.  mksplash.py writes them; these name them.
 */
extern const u8 g_saffran_pal[768];
extern const u8 g_saffran_pix[SCR_PIX];

#define SPL_SHADOW 248
#define SPL_DIM    250
#define SPL_TEXT   253
#define SPL_BRIGHT 254
#define SPL_WHITE  255

void   splash_show(void);

/* --- banana.c ---------------------------------------------------------- */
void   banana_draw(u8 *dst, u32 t);

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
extern volatile u8  g_beat;
extern volatile u8  g_bar;
extern volatile u32 g_step;

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
u32    bios_int(u16 which, u16 ax, u16 bx, u16 cx, u16 dx, u16 es);
void   pic_to_bios(void);
void   pic_to_kernel(void);

/* --- disk.c ------------------------------------------------------------ */
#define DISK_BUF_SEG 0x7000              /* 0x70000: below 1MB, and clear
                                          * of a 64K boundary             */
#define DISK_BUF     ((volatile u8 *)0x70000)
int    disk_read(u32 lba, int count);    /* -> 1 on success               */
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
    char magic[16];                      /* patched by tools/mkimage.py    */
    u32  journal_lba, journal_sectors;
    u32  state_lba,   state_sectors;
} DiskMap;

extern volatile DiskMap g_diskmap;

void   store_init(void);
int    store_ready(void);
int    store_save(void);                 /* -> 1 if everything landed      */
int    store_unsaved(void);
u32    store_boots(void);

void   store_journal_line(const char *s);
void   store_journal_printf(const char *fmt, ...);
const char *store_journal_text(void);
u32    store_journal_used(void);
u32    store_journal_size(void);

int    store_teach(const char *word, const char *resp);  /* -1 if full     */
int    store_forget(const char *word);
const char *store_lookup(const char *word);
int    store_taught_count(void);
const Taught *store_taught_at(int i);

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

#endif /* FLOPOS_H */
