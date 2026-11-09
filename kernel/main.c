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

/* The perf counters' clock: the cycle counter where there is one, and a
 * steady zero where there is not, so a 486 times everything as free
 * rather than faulting on an instruction it has never heard of. */
static u32 perf_clock(void)
{
    return g_has_tsc ? tsc32() : 0;
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

/* How often the screen is actually pushed to the card.  At 320x200 that
 * is 64000 bytes and may as well be every time round; at 800x600 it is
 * 480000, and thirty times a second is as smooth as anybody can see while
 * leaving the loop free to look at the network and the serial line in
 * between. */
static u32 s_last_frame;

static int frame_due(void)
{
    u32 every = vga_is_big() ? 33 : 0;
    if (ms_now() - s_last_frame < every) return 0;
    s_last_frame = ms_now();
    return 1;
}

/* What was typed only reaches the terminal's history when a frame draws
 * it, and a line can now arrive and finish between two frames -- all at
 * once down the serial line, or from anybody typing faster than thirty
 * keys a second.  So the line is written in one last time before the
 * newline commits it, or the history keeps a prompt with nothing after. */
static void commit_line(void)
{
    term_input_line(s_input_col, line_text(), line_caret(), C_TEXT);
    term_newline();
}

static void draw_frame_now(u32 t)
{
    u32 a, b;

    a = perf_clock();
    mood_draw(g_back, t);
    banana_draw(g_back, t);              /* over the background, always    */
    vga_compose();                       /* and blown up, if we are big    */
    b = perf_clock(); perf_add(PERF_ROOM, b - a); a = b;

    /* The text goes on last and at the real size, which is the whole point
     * of the bigger screen: the background can be stretched because it is
     * a gradient, and a letter cannot. */
    term_input_line(s_input_col, line_text(), line_caret(), C_TEXT);
    term_render(vga_canvas(), t);
    b = perf_clock(); perf_add(PERF_TEXT, b - a); a = b;

    if (!vga_is_big()) vga_wait_retrace();
    b = perf_clock(); perf_add(PERF_WAIT, b - a); a = b;

    vga_present();
    b = perf_clock(); perf_add(PERF_BLIT, b - a);

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

    /* The panel's own 800x600, from the boot screen on.  This once froze
     * the machine solid before the boot screen had finished, and the
     * reason is understood now: the video BIOS switches interrupts on, a
     * timer tick landed inside it on a vector meant for something else,
     * and the clock stopped.  Every BIOS call goes through bios_int() now,
     * which cannot let that happen, and a stopped clock is caught and
     * restarted by ms_now() even if something else ever manages it.
     *
     * Holding Shift while it boots keeps 320x200, because the one thing
     * that cannot be fixed from this end is a panel nobody here can see.
     * The BIOS's own record of the shift keys is used rather than ours:
     * a key held since power-on was pressed before our keyboard driver
     * existed, so only the BIOS saw it go down. */
    {
        int shift = (*(volatile u8 *)0x417 & 0x03) ||
                    kbd_down(0x2A) || kbd_down(0x36);
        if (!shift && vga_go_big()) term_set_face(0);
        else if (shift) serial_print("[vga] shift held: staying at 320x200\n");
    }

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

        /* Anything that arrived on the wire.  Cheap when there is nothing
         * there, which is almost always, and it means the machine answers
         * when something asks after it rather than only when we ask. */
        if (net_ready()) ip_poll();

        /* Characters from the serial line are treated exactly as if they
         * had been typed here, so the far end can drive the machine. */
        {
            int sc;
            while ((sc = serial_getc()) >= 0) {
                if (line_inject(sc)) {
                    term_scroll_home();
                    commit_line();
                    shell_submit(line_text());
                    line_reset();
                    shell_prompt();
                    after_prompt();
                }
            }
        }

        while ((ev = kbd_event()) >= 0) {
            int sc = KEV_SCAN(ev);

            if (sc == SC_PGUP) { term_scroll_view(+(term_rows() - 2)); continue; }
            if (sc == SC_PGDN) { term_scroll_view(-(term_rows() - 2)); continue; }

            /* Any other key means you are done looking back: snap to the
             * bottom so what you type is where you can see it. */
            term_scroll_home();

            if (line_key(ev)) {           /* Enter: the line is finished    */
                term_scroll_home();
                commit_line();
                shell_submit(line_text());
                line_reset();
                shell_prompt();
                after_prompt();
            }
        }

        if (frame_due()) draw_frame_now(t);
    }
}
