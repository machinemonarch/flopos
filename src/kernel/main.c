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
static int s_pumping;                    /* a menu has the keyboard        */

/* At 800x600 a frame every 33 ms, on a steady beat: one drawn late does
 * not push all the later ones back with it. */
static int frame_due(void)
{
    u32 every = vga_is_big() ? 33 : 0, now = ms_now();
    if (now - s_last_frame < every) return 0;
    s_last_frame += every;
    if (now - s_last_frame >= every) s_last_frame = now;    /* far behind: start over */
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

/* Ctrl+C: what was typed stays on the screen, marked ^C, and is not done
 * or kept in the history; a fresh prompt follows. */
static void drop_line(void)
{
    term_scroll_home();
    term_input_line(s_input_col, line_text(), line_caret(), C_TEXT);
    term_colour(C_DIM);
    term_print("^C");
    term_newline();
    line_reset();
    shell_prompt();
    after_prompt();
}

void (*g_frame_fx)(u8 *canvas, int w, int h, u32 t);

/* Ctrl+Shift+F5, from anywhere: the screen switched again, for the start
 * where the panel stayed black with everything running behind it.  It
 * works blind -- nothing needs to be seen to press it.  And once, a moment
 * after the start, the panel's off switches are looked at again, in case
 * its power came up late and wrong. */
volatile int g_screen_again;
static u32 s_wake_check;                 /* ms_now() to look again at, or 0 */

static void screen_again(void)
{
    int was_big = vga_is_big();
    serial_print("[vga] Ctrl+Shift+F5: the screen switched again\n");
    if (was_big) {
        if (!vga_redo_big()) term_set_face(1);   /* 800x600 would not come back: 320x200 */
    } else vga_redo_small();
    term_refresh();
}

static void draw_frame_now(u32 t)
{
    u32 a, b;
    int covered;

    if (g_screen_again) { g_screen_again = 0; screen_again(); }
    if (s_wake_check && (s32)(ms_now() - s_wake_check) >= 0) {
        s_wake_check = 0;
        if (panel_wake()) term_refresh();
    }
    covered = term_win_covers();

    /* Under a window that covers the whole screen nothing can be seen,
     * so nothing is drawn there; see term.c. */
    a = perf_clock();
    if (!covered) {
        if (vga_is_big()) {                 /* drawn at the big screen's own pixels */
            mood_draw_big(vga_canvas(), vga_width(), vga_height(), t);
            banana_draw_big(vga_canvas(), vga_width(), vga_height(), t);
            vga_keep_composed();
        } else {
            mood_draw(g_back, t);
            banana_draw(g_back, t);          /* over the background, always    */
            vga_compose();                   /* and blown up, if we are big    */
        }
    } else
        vga_keep_composed();
    b = perf_clock(); perf_add(PERF_ROOM, b - a); a = b;

    /* The text goes on last and at the real size, which is the whole point
     * of the bigger screen: the background can be stretched because it is
     * a gradient, and a letter cannot. */
    /* While something else has the keyboard, the line being typed is not
     * the thing being typed into, so it is not drawn. */
    if (!s_pumping)
        term_input_line(s_input_col, line_text(), line_caret(), C_TEXT);
    term_render(vga_canvas(), t);
    if (g_frame_fx) g_frame_fx(vga_canvas(), vga_width(), vga_height(), t);
    hud_frame(vga_canvas(), t);
    kbpick_frame(vga_canvas(), t);
    mood_neon(ms_now());
    b = perf_clock(); perf_add(PERF_TEXT, b - a); a = b;

    if (!vga_is_big()) vga_wait_retrace();
    b = perf_clock(); perf_add(PERF_WAIT, b - a); a = b;

    if (covered && vga_is_big()) {
        int y0, y1, hx0, hy0, hx1, hy1;
        if (term_dirty_rows(&y0, &y1)) vga_present_rows(y0, y1);
        if (hud_rect(&hx0, &hy0, &hx1, &hy1)) vga_present_rect(hx0, hy0, hx1, hy1);
        if (kbpick_rect(&hx0, &hy0, &hx1, &hy1)) vga_present_rect(hx0, hy0, hx1, hy1);
    } else {
        int y0, y1;
        (void)term_dirty_rows(&y0, &y1);
        vga_present();
    }
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

/* For anything that takes over the keyboard for a while -- a menu, a
 * long fetch -- but should leave the machine alive underneath: the
 * network answered, the clock refereed, the screen drawn when a frame is
 * due.  `now` draws one regardless, for putting up a message before
 * something slow starts. */
static u32 s_pump_out;                   /* the cycle counter, when it last returned */

void main_pump(int now)
{
    u32 t;
    /* Called again at once, with nothing done in between: whoever calls
     * is only waiting.  So the machine waits for the next interrupt --
     * a millisecond at most -- and the HUD counts it as idle. */
    if (!now && g_cpu_mhz && tsc32() - s_pump_out < g_cpu_mhz * 40u) idle_wait();
    t = ms_now();
    if (net_ready()) ip_poll();
    audio_tell(s_fps);
    s_pumping = 1;
    if (now || frame_due()) draw_frame_now(t);
    s_pumping = 0;
    if (g_cpu_mhz) s_pump_out = tsc32();
}

void kmain(void)
{
    static u8 black[768];
    u32 t;

    /* --- the machine ---------------------------------------------------- */
    /* Saffran is on the screen already: the boot sector put him there
     * while it loaded us.  These first few are over in a millisecond,
     * before there is a clock to tell them by, so they are told just
     * after, in the order they happened. */
    heap_init();
    mathx_init();
    vga_init();
    vga_adopt();
    cpu_detect();
    idt_init();
    wall_init();                         /* paging on; apps will be kept to their own memory */
    time_init();
    irq_enable();

    boot_begin();
    {
        extern char _image_bytes[];
        u32 kb = ((u32)_image_bytes + 1023u) / 1024u;
        boot_step("Hauling Saffran and %u KB off the wobbly disc...", kb);
    }
    boot_step("Forgot where I placed stuff.");
    if (BOOTINFO_EXTKB) boot_result("found %u MB", (BOOTINFO_EXTKB + 1024u + 512u) / 1024u);
    else                boot_result("640 KB, all low");
    boot_step("Bending sine-noodles into perfect circles...");
    boot_step("Lifting Saffran off the glass onto a fresh canvas...");
    boot_step("Questioning Normalcy");
    {
        char name[16];
        int i;
        for (i = 0; g_cpu_name[i] && i < 15; i++) name[i] = g_cpu_name[i];
        name[i] = 0;
        if (name[0]) boot_result("%s", name);
    }
    boot_step("Weighing the Sympathetic with the Para-Sympathetic.");
    boot_step("Aligning the realm to the beat of the crystal...");

    /* From here, each line is told before its step runs. */
    boot_step("Tuning an ear to the clatter of Swedish runes...");
    kbd_init();

    boot_step("Stringing a whisper-wire out the back...");
    serial_init();
    if (serial_present()) irq_unmask(4);
    if (serial_present()) {
        serial_print("\n--- FLOPOS is awake and talking. Type here to drive it. ---\n");
        boot_result("COM1");
    } else
        boot_result("no wire");

    boot_step("Negotiating with the silicone...");
    cpu_measure_mhz();                   /* needs the timer running        */
    if (g_cpu_mhz) boot_result("%u MHz", g_cpu_mhz);
    {
        /* The stack's canary (idt.c), from the random pool once it has
         * something in it.  Here in kmain, which never returns, so no
         * function waiting to check the old value is left on the stack. */
        extern u32 __stack_chk_guard;
        u32 g;
        (void)rand_health();
        rand_bytes((u8 *)&g, 4);
        __stack_chk_guard = (g & ~0xFFu) ? (g & ~0xFFu) : 0x2A2A2A00u;
    }

    boot_step("Schnipskiing the Schnoodlz");
    snd_init();
    boot_result(snd_has_fm() ? "FM choir" : "the beeper");

    /* Pull the journal and whatever has been taught off the disk.  This
     * spins the drive for a moment, which is the first honest sign that
     * the machine remembers anything. */
    boot_step("Flippely-Flopping");
    store_init();
    if (store_keys_get()) keymap_set(store_keys_get());
    if (rtc_lost_power() || !rtc_battery_ok()) {
        boot_step("The clock dozed off without its battery...");
        boot_result(rtc_battery_ok() ? "it lost its power while unplugged" : "its battery is flat");
        if (store_ready())
            store_journal_printf("the clock %s", rtc_battery_ok() ? "had lost its power while switched off"
                                                                  : "reports its battery flat");
    }
    if (store_ready() && clock_from_goodbye()) {
        boot_step("Winding the clock forward to our last goodbye...");
        boot_result("near enough");
    }
    if (store_ready()) {
        u8 h, mi, se, mo, da; u16 yr;
        if (rtc_read(&h, &mi, &se, &yr, &mo, &da))
            store_journal_printf("--- awake %u-%02u-%02u %02u:%02u, boot %u",
                                 yr, mo, da, h, mi, store_boots());
        else
            store_journal_printf("--- awake, boot %u", store_boots());
        boot_result("boot %u", store_boots());
    } else
        boot_result("nothing kept");

    /* His way with words is a quarter of a megabyte on the floppy, read
     * once, here, so that no answer ever waits for the drive. */
    boot_step("Pouring the wizard his morning tea...");
    boot_result(wizsay_init() ? "he is humming" : "he is sleeping in");
    leave_load();

    /* --- the environment ------------------------------------------------ */
    /* panel_init reads and keeps the old register values first, so `wide`
     * can still put the panel back exactly as it was found. */
    boot_step("Searching for stuff...");
    panel_init();
    boot_result(panel_present() ? "a flat panel" : "no panel");
    if (panel_present()) {
        boot_step("Coaxing the glass to fill its frame...");
        panel_set_wide(1);
    }

    boot_step("Inking the little letters...");
    term_init();
    boot_step("Schnoodling the Schnabbleyblobs.");
    mood_init();
    boot_step("Memorising every word I know...");
    line_init();
    shell_init();

    /* The panel's own 800x600, from the shell's first frame.  This once
     * froze the machine solid, and the reason is understood now: the
     * video BIOS switches interrupts on, a timer tick landed inside it on
     * a vector meant for something else, and the clock stopped.  Every
     * BIOS call goes through bios_int() now, which cannot let that happen,
     * and a stopped clock is caught and restarted by ms_now() even if
     * something else ever manages it.
     *
     * Holding Shift while it boots keeps 320x200, because the one thing
     * that cannot be fixed from this end is a panel nobody here can see.
     * The BIOS's own record of the shift keys is used rather than ours:
     * a key held since power-on was pressed before our keyboard driver
     * existed, so only the BIOS saw it go down.
     *
     * Saffran stays up across the switch: the screen blanks for a moment,
     * then he comes back at the new size with his own colours. */
    {
        int shift = (*(volatile u8 *)0x417 & 0x03) ||
                    kbd_down(0x2A) || kbd_down(0x36);
        if (shift) {
            boot_step("A finger on Shift. Staying small and chunky.");
            serial_print("[vga] shift held: staying at 320x200\n");
        } else {
            boot_step("Begging the glass for 480,000 lights...");
            if (vga_go_big()) { term_set_face(3); boot_result("800x600"); s_wake_check = ms_now() + 1500; }
            else              boot_result("it said no");
        }
    }
    boot_step("Being generally cool.");
    boot_end();

    /* Then the wizard, up out of the dark.  Nobody reaches the shell --
     * cyberdelia -- without answering his riddle, and every answer is the
     * right one.  He leaves the palette as the mood's own. */
    vga_set_palette(black);
    wizard_gate(0);

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
                int r = line_inject(sc);
                if (r == 2) { drop_line(); continue; }
                if (r) {
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

            int r = line_key(ev);
            if (r == 2) { drop_line(); continue; }
            if (r) {                      /* Enter: the line is finished    */
                term_scroll_home();
                commit_line();
                shell_submit(line_text());
                line_reset();
                shell_prompt();
                after_prompt();
            }
        }

        if (frame_due()) draw_frame_now(t);
        else idle_wait();                /* nothing to do: wait, counted    */
    }
}
