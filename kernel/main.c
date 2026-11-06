/* ===========================================================================
 * main.c -- FLOPOS comes up here.
 *
 * Bring the machine up, then one loop forever: drain the keyboard into the
 * line editor, paint the room, paint the terminal over it, show the frame.
 *
 * There is no idle path on purpose.  The background is always moving, so
 * every frame is a new frame, and the loop runs as fast as the retrace
 * will let it.
 * ===========================================================================
 */
#include "flopos.h"
#include "io.h"

static int s_input_col;

/* ---- where the time goes -------------------------------------------------
 * Guessing at frame cost from a desk is worthless; the only machine whose
 * answer matters is the Toshiba.  So each phase is timed with the cycle
 * counter and kept as a rolling average, and `speed` reads it back out.
 * On a CPU with no TSC these stay zero and the command says so.
 */
#define PERF_ROOM 0
#define PERF_TEXT 1
#define PERF_BLIT 2
#define PERF_WAIT 3
#define PERF_N    4

static u32 s_perf[PERF_N];               /* rolling average, in cycles     */
static u32 s_fps, s_frames, s_fps_at;

static u32 tsc32(void)
{
    u32 lo, hi;
    if (!g_has_tsc) return 0;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    (void)hi;
    return lo;
}

static void perf_add(int phase, u32 cycles)
{
    /* A cheap exponential average: seven parts old, one part new. */
    s_perf[phase] = s_perf[phase] - (s_perf[phase] >> 3) + (cycles >> 3);
}

u32 perf_us(int phase)
{
    if (phase < 0 || phase >= PERF_N || !g_cpu_mhz) return 0;
    return s_perf[phase] / g_cpu_mhz;
}

u32 perf_fps(void) { return s_fps; }

/* Where the shell's prompt ended and the typed line begins.  Recorded
 * after every prompt so the editor always writes to the right column. */
static void after_prompt(void)
{
    s_input_col = term_cursor_col();
}

static void draw_frame_now(u32 t)
{
    u32 a, b;

    a = tsc32();
    mood_draw(g_back, t);
    b = tsc32(); perf_add(PERF_ROOM, b - a); a = b;

    term_input_line(s_input_col, line_text(), line_caret(), C_TEXT);
    term_render(g_back, t);
    b = tsc32(); perf_add(PERF_TEXT, b - a); a = b;

    vga_wait_retrace();
    b = tsc32(); perf_add(PERF_WAIT, b - a); a = b;

    memcpy32((void *)VGA_FB, g_back, SCR_PIX / 4);
    b = tsc32(); perf_add(PERF_BLIT, b - a);

    s_frames++;
    {
        u32 now = ms_now();
        if (now - s_fps_at >= 1000) {
            s_fps = s_frames * 1000 / (now - s_fps_at);
            s_frames = 0;
            s_fps_at = now;
        }
    }
}

void kmain(void)
{
    static u8 black[768];
    u32 t;

    /* --- the machine ---------------------------------------------------- */
    heap_init();
    mathx_init();
    vga_init();
    cpu_detect();

    idt_init();
    time_init();
    kbd_init();
    serial_init();
    snd_init();
    irq_enable();
    if (serial_present()) irq_unmask(4);

    if (serial_present())
        serial_print("\n--- FLOPOS is awake and talking. Type here to drive it. ---\n");
    cpu_measure_mhz();                   /* needs the timer running        */

    /* Pull the journal and whatever has been taught off the disk.  This
     * spins the drive for a moment, which is the first honest sign that
     * the machine remembers anything. */
    store_init();
    if (store_ready()) {
        u8 h, mi, se, mo, da; u16 yr;
        if (rtc_read(&h, &mi, &se, &yr, &mo, &da))
            store_journal_printf("--- awake %u-%02u-%02u %02u:%02u, boot %u",
                                 yr, mo, da, h, mi, store_boots());
        else
            store_journal_printf("--- awake, boot %u", store_boots());
    }

    /* --- the environment ------------------------------------------------ */
    /* Full screen from the first frame, boot screen included.  panel_init
     * reads and keeps the old register values first, so `wide` can still
     * put the panel back exactly as it was found. */
    panel_init();
    panel_set_wide(1);
    term_init();
    mood_init();
    line_init();
    shell_init();

    /* Saffran first, out of black and back into it. */
    splash_show();

    /* Arrive out of black rather than snapping on. */
    vga_set_palette(black);
    shell_banner();
    shell_prompt();
    after_prompt();
    draw_frame_now(0);
    mood_apply_palette(28);

    /* --- and then just this --------------------------------------------- */
    for (;;) {
        int ev;

        t = ms_now();

        /* Characters from the serial line are treated exactly as if they
         * had been typed here, so the far end can drive the machine. */
        {
            int sc;
            while ((sc = serial_getc()) >= 0) {
                if (line_inject(sc)) {
                    term_scroll_home();
                    term_newline();
                    shell_submit(line_text());
                    line_reset();
                    shell_prompt();
                    after_prompt();
                }
            }
        }

        while ((ev = kbd_event()) >= 0) {
            int sc = KEV_SCAN(ev);

            if (sc == SC_PGUP) { term_scroll_view(+(TERM_ROWS - 2)); continue; }
            if (sc == SC_PGDN) { term_scroll_view(-(TERM_ROWS - 2)); continue; }

            /* Any other key means you are done looking back: snap to the
             * bottom so what you type is where you can see it. */
            term_scroll_home();

            if (line_key(ev)) {           /* Enter: the line is finished    */
                term_scroll_home();
                term_newline();           /* commit what was typed          */
                shell_submit(line_text());
                line_reset();
                shell_prompt();
                after_prompt();
            }
        }

        draw_frame_now(t);
    }
}
