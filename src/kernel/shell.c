/* ===========================================================================
 * shell.c -- the prompt, the tokeniser, and the words FLOPOS knows.
 *
 * Adding a word is one line in s_cmds plus a function.  The tokeniser
 * understands double quotes.
 *
 * Nothing here holds any of its own prose: every string the user sees
 * comes from voice.c, so the register can be rewritten in one place.
 * ===========================================================================
 */
#include "flopos.h"
#include "io.h"
#include "gen/version.h"

#define MAXARG 8

static char  s_argbuf[LINE_MAX + 1];
static char  s_rawline[LINE_MAX + 1];    /* before the tokeniser cuts it up */
static char *s_argv[MAXARG];
static int   s_argc;
static u32   s_boot_ms;

static const char *s_place = "here";

const char *shell_place(void) { return s_place; }

/* ---- small string helpers -------------------------------------------- */
static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c; }

static int eq_ci(const char *a, const char *b)
{
    while (*a && *b) {
        if (lower((u8)*a) != lower((u8)*b)) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

/* Reduce anything the user typed to lowercase letters and digits, which is
 * what the phrase table is keyed on.  Throwing away spaces and punctuation
 * is the entire point: it makes "Who are you?", "who are you" and the
 * "whoareyou" somebody actually typed on a real keyboard one single key. */
static void squash(const char *src, char *dst, int cap)
{
    int n = 0;
    while (*src && n < cap - 1) {
        int c = (u8)*src++;
        if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) dst[n++] = (char)c;
    }
    dst[n] = 0;
}

/* "moodier" -> "mood", "darker" -> "dark".  A comparative is a request for
 * more of something, so if the stem is a word we know, do that. */
static int comparative_stem(const char *w, char *out, int cap)
{
    int n = 0;
    while (w[n] && n < cap - 1) { out[n] = w[n]; n++; }
    out[n] = 0;
    if (n > 4 && out[n - 3] == 'i' && out[n - 2] == 'e' && out[n - 1] == 'r') {
        out[n - 3] = 0;
        return 1;
    }
    if (n > 3 && out[n - 2] == 'e' && out[n - 1] == 'r') {
        out[n - 2] = 0;
        return 1;
    }
    return 0;
}

static void tokenize(const char *line)
{
    char *w = s_argbuf;
    int i = 0;

    s_argc = 0;
    for (i = 0; line[i] && i < LINE_MAX; i++) s_rawline[i] = line[i];
    s_rawline[i] = 0;
    i = 0;
    while (line[i] && i < LINE_MAX) { s_argbuf[i] = line[i]; i++; }
    s_argbuf[i] = 0;

    while (*w && s_argc < MAXARG) {
        while (*w == ' ' || *w == '\t') w++;
        if (!*w) break;

        if (*w == '"') {
            w++;
            s_argv[s_argc++] = w;
            while (*w && *w != '"') w++;
            if (*w) *w++ = 0;
        } else {
            s_argv[s_argc++] = w;
            while (*w && *w != ' ' && *w != '\t') w++;
            if (*w) *w++ = 0;
        }
    }
}

/* ---- output helpers --------------------------------------------------- */
static void say(const char *s)
{
    term_colour(C_TEXT);
    term_print(s);
    term_newline();
}

static void say_dim(const char *s)
{
    term_colour(C_DIM);
    term_print(s);
    term_newline();
}

/* ---- the words -------------------------------------------------------- */
typedef struct {
    const char *name;
    const char *summary;
    void (*fn)(void);
} Cmd;

static const Cmd s_cmds[];
static int s_cmd_count(void);
static const Cmd s_diag[];
static int s_diag_count(void);
static void cmd_report(void);
static void cmd_net(void);
static void print_ip(u32 a);
static int net_ready_here(int chatty);

/* Put the whole screen up, now, from inside a command -- the main loop
 * does this every frame, but a command that is about to do something
 * slow or final wants it seen first.  The terminal is drawn into
 * whichever buffer is the size the screen actually is; drawing it into
 * the small one at the big one's row width writes most of half a megabyte
 * past the end, which is what bye, reboot and whaaat all used to do. */
static void screen_now(void)
{
    u32 t = ms_now();
    if (vga_is_big()) {                 /* drawn at the big screen's own pixels */
        mood_draw_big(vga_canvas(), vga_width(), vga_height(), t);
        banana_draw_big(vga_canvas(), vga_width(), vga_height(), t);
        vga_keep_composed();
    } else {
        mood_draw(g_back, t);
        banana_draw(g_back, t);
        vga_compose();
    }
    term_render(vga_canvas(), t);
    vga_present();
}

static void cmd_crypto(void) { (void)crypto_selftest(); }

static void cmd_apps(void) { apps_list(); }

static void cmd_network(void)
{
    netui_run();
}

static void cmd_whoami(void)
{
    say(voice(V_WHOAMI));
}

static void cmd_here(void)
{
    say(voice(V_HERE));
}

static void cmd_look(void)
{
    say(voice(V_LOOK));
}

static void cmd_help(void)
{
    int i, n = s_cmd_count();

    say_dim(voice(V_HELP_HEAD));
    term_colour(C_BRIGHT);
    for (i = 0; i < n; i++) {
        term_print("  ");
        term_print(s_cmds[i].name);
        /* pad the name column so the summaries line up at 12 */
        {
            int w = 0;
            while (s_cmds[i].name[w]) w++;
            while (w < 10) { term_putc(' '); w++; }
        }
        term_colour(C_DIM);
        term_print(s_cmds[i].summary);
        term_colour(C_BRIGHT);
        term_newline();
    }
    say_dim(voice(V_HELP_FOOT));
    say_dim("Put help after a word for more: ping help");
}

static void cmd_clear(void)
{
    term_clear();
}

static void cmd_time(void)
{
    u8 h, mi, se, mo, da;
    u16 yr;
    u32 up = (ms_now() - s_boot_ms) / 1000;

    if (rtc_read(&h, &mi, &se, &yr, &mo, &da)) {
        term_colour(C_TEXT);
        term_printf("%02u:%02u:%02u   %u-%02u-%02u", h, mi, se, yr, mo, da);
        term_newline();
    } else {
        say_dim(voice(V_TIME));
    }
    term_colour(C_DIM);
    term_printf("awake for %u min %u sec", up / 60, up % 60);
    term_newline();
    {
        /* And how the clock has been keeping: its battery, as the chip and
         * the BIOS see it, and how far out the network found it. */
        s32 d;
        term_printf("its battery: %s", !rtc_battery_ok() ? "flat, says the clock chip" :
                    rtc_lost_power() ? "good now, but it lost its power while switched off" : "good");
        term_newline();
        if (clock_drift(&d)) {
            u32 a = d < 0 ? (u32)-d : (u32)d;
            term_printf("the network found it %u days %u h %u min %s", a / 86400, (a / 3600) % 24,
                        (a / 60) % 60, d < 0 ? "ahead" : "behind");
            term_newline();
        }
    }
}

static void cmd_sys(void)
{
    term_colour(C_BRIGHT);
    term_printf("cpu     %s", g_cpu_name);
    term_newline();

    term_colour(C_TEXT);
    if (g_cpu_vendor[0]) {
        term_printf("vendor  %s", g_cpu_vendor);
        term_newline();
    }
    if (g_cpu_mhz) {
        term_printf("clock   %u mhz, measured against the timer", g_cpu_mhz);
        term_newline();
    }
    term_printf("memory  %u kb above 1mb, %u kb of it spoken for",
                BOOTINFO_EXTKB, heap_used() / 1024);
    term_newline();
    if (heap_is_low()) {
        term_colour(C_ACCENT);
        term_printf("        running in conventional memory, %u kb free",
                    (heap_total() - heap_used()) / 1024);
        term_newline();
        term_colour(C_TEXT);
    }
    if (vga_is_big())
        term_printf("video   vesa mode %x, %ux%u, 256 colours",
                    (u32)vga_big_mode(), (u32)vga_width(), (u32)vga_height());
    else
        term_printf("video   vga mode 13h, 320x200, 256 colours");
    term_newline();
    if (panel_present())
        term_printf("panel   flat panel found, %s",
                    panel_wide() ? "stretched to fill" : "centred");
    else
        term_printf("panel   no flat panel controller answered");
    term_newline();
    term_printf("screen  %u by %u, the %s face",
                term_cols(), term_rows(), term_face()->name);
    term_newline();
    term_printf("audio   pc speaker%s",
                snd_has_fm() ? " and opl2 fm" : ", no fm chip answered");
    term_newline();
    term_printf("disk    bios drive %x, %s", BOOTINFO_DRIVE,
                store_ready() ? "readable and writable" : "read only");
    term_newline();
    if (store_ready()) {
        term_printf("memory  %u boots so far", store_boots());
        term_newline();
    }
    term_printf("a20     %s", BOOTINFO_A20 ? "open" : "unverified");
    term_newline();
    term_printf("apps    %s", wall_on() ? "kept to their own memory" : "not walled off: this CPU cannot");
    term_newline();
}

static void cmd_mood(void)
{
    int i;

    if (s_argc >= 2) {
        for (i = 0; i < mood_count(); i++)
            if (eq_ci(s_argv[1], mood_name(i))) { mood_set(i); break; }
        if (i == mood_count()) {
            term_colour(C_DIM);
            term_print("I know these: ");
            for (i = 0; i < mood_count(); i++) {
                if (i) term_print(", ");
                term_print(mood_name(i));
            }
            term_newline();
            return;
        }
    } else {
        mood_set(mood_get() + 1);
    }

    mood_apply_palette(12);
    term_colour(C_ACCENT);
    term_printf("%s. %s", mood_name(mood_get()), voice(V_MOOD));
    term_newline();
}

static void cmd_sound(void)
{
    if (!snd_enabled()) snd_toggle();
    say(voice(V_SOUND_ON));
    term_colour(C_DIM);
    term_printf("%s, %u beats a minute%s", snd_pattern_name(), snd_tempo(),
                snd_has_fm() ? ", through the fm chip" : ", through the speaker");
    term_newline();
}

static void cmd_quiet(void)
{
    if (snd_enabled()) snd_toggle();
    say(voice(V_SOUND_OFF));
}

static void cmd_listen(void)
{
    snd_next_pattern();
    if (!snd_enabled()) snd_toggle();
    term_colour(C_ACCENT);
    term_printf("%s.", snd_pattern_name());
    term_newline();
}

static void cmd_wide(void)
{
    if (vga_is_big()) {
        say("At 800x600 the picture is already the panel's own size.");
        say_dim("There is nothing to stretch. screen small to go back to 320x200.");
        return;
    }
    if (!panel_present()) {
        say("Nothing answers where the panel controller should be.");
        say_dim("The screen is whatever size it already was.");
        return;
    }

    panel_set_wide(!panel_wide());
    term_colour(C_ACCENT);
    term_print(panel_wide() ? "Filling the screen." : "Back in its box.");
    term_newline();
    if (panel_wide()) {
        term_colour(C_DIM);
        say_dim("Say it again to undo that.");
    }
}

static void cmd_speed(void)
{
    if (!g_cpu_mhz) {
        say("No cycle counter on this processor, so I can't time myself.");
        return;
    }

    term_colour(C_BRIGHT);
    term_printf("%u frames a second", perf_fps());
    term_newline();
    term_colour(C_TEXT);
    term_printf("room    %4u us   the moving background", perf_us(0));
    term_newline();
    term_printf("text    %4u us   drawing every character", perf_us(1));
    term_newline();
    term_printf("wait    %4u us   waiting on the screen",
                perf_us(3));
    term_newline();
    term_printf("blit    %4u us   %u bytes to the card", perf_us(2),
                (u32)vga_width() * (u32)vga_height());
    term_newline();
    term_colour(C_DIM);
    if (!vga_retrace_works())
        say_dim("This card never reports vertical retrace. Not waiting for it.");
    else
        term_printf("  retrace poll settled after %u reads",
                    vga_retrace_polls()), term_newline();
}

static void cmd_wire(void)
{
    if (!serial_present()) {
        say("No serial port answers at COM1.");
        return;
    }
    term_colour(C_TEXT);
    term_printf("com1    3f8, irq4, 115200 8n1");
    term_newline();
    term_printf("sent    %u bytes", serial_sent());
    term_newline();
    term_printf("got     %u bytes", serial_received());
    term_newline();
    term_colour(C_DIM);
    say_dim(serial_echo() ? "Everything here goes down the wire."
                          : "The wire is quiet; nothing is mirrored.");
}

static void cmd_disk(void)
{
    int r;
    const volatile u8 *buf = DISK_BUF;
    int i;

    term_colour(C_DIM);
    say_dim("Asking the BIOS for sector 0...");

    /* AH=02 read, AL=1 sector; CH=cylinder 0, CL=sector 1; DH=head 0,
     * DL=the drive we booted from.  Buffer at 0x7000:0000. */
    r = disk_read(0, 1);

    if (!r) {
        term_colour(C_ACCENT);
        term_printf("The BIOS refused. Status %x", disk_last_status());
        term_newline();
        return;
    }

    term_colour(C_TEXT);
    term_print("Got it. First bytes: ");
    for (i = 0; i < 8; i++) term_printf("%02x ", buf[i]);
    term_newline();
    term_printf("Signature at 510: %02x %02x", buf[510], buf[511]);
    term_newline();
    if (buf[510] == 0x55 && buf[511] == 0xAA) {
        term_colour(C_BRIGHT);
        say("That is our boot sector. The BIOS still answers.");
    } else {
        term_colour(C_ACCENT);
        say("That is not a boot sector. Something is wrong.");
    }
}

static void cmd_remember(void)
{
    if (!store_ready()) { say("There is nowhere to write."); return; }
    if (!store_unsaved()) { say_dim("Already written down."); return; }
    term_colour(C_DIM);
    say_dim("Writing to the disk...");
    if (store_save()) say("Kept.");
    else say("The disk would not take it.");
}

static void cmd_journal(void)
{
    const char *j = store_journal_text();
    u32 used = store_journal_used();
    char line[64];
    u32 i;
    int n = 0, shown = 0;

    if (!store_ready()) { say("No journal on this disk."); return; }

    /* The last dozen lines; the file itself is the whole record. */
    {
        u32 start = 0, nl = 0, count = 0;
        for (i = 0; i < used; i++) if (j[i] == '\n') count++;
        for (i = 0; i < used && nl + 12 < count; i++)
            if (j[i] == '\n') { nl++; start = i + 1; }
        i = start;
    }

    term_colour(C_TEXT);
    for (; i < used; i++) {
        if (j[i] == '\r') continue;
        if (j[i] == '\n' || n >= (int)sizeof line - 1) {
            line[n] = 0;
            if (n) { term_print(line); term_newline(); shown++; }
            n = 0;
            continue;
        }
        line[n++] = j[i];
    }
    if (n) { line[n] = 0; term_print(line); term_newline(); }

    term_colour(C_DIM);
    term_printf("%u of %u bytes used. It is JOURNAL.TXT on this disk.",
                used, store_journal_size());
    term_newline();
    (void)shown;
}

static void cmd_whaaat(void)
{
    static u8 target[768];
    int going_in = !mood_deep();

    /* Draw the screen as it stands this instant, so the vortex takes hold
     * of what is actually there -- background, text, prompt and all --
     * rather than whatever happened to be in the buffer. */
    screen_now();
    vga_shrink();

    /* Work out where we are going before setting off, so the vortex can
     * cross into it rather than snapping at the end. */
    mood_set_deep(going_in);
    mood_build_palette(target);
    mood_set_deep(!going_in);

    vortex_run(target);

    mood_set_deep(going_in);
    term_zalgo(going_in);
    mood_apply_palette(0);

    term_colour(C_ACCENT);
    say(voice(going_in ? V_WHAAAT_IN : V_WHAAAT_OUT));
}

/* ---- the keyboard ------------------------------------------------------
 * Which country's keys are printed on it.  Kept on the floppy, until a
 * new release goes on it: that starts on Swedish again. */
static const char *keyboard_name(void)
{
    int i;
    for (i = 0; i < keymap_count(); i++)
        if (eq_ci(keymap_code(), keymap_code_at(i))) return keymap_name_at(i);
    return "Swedish";
}

static void keyboard_list(void)
{
    int i;
    for (i = 0; i < keymap_count(); i++) {
        term_colour(C_ACCENT);
        term_printf("  %s  ", keymap_code_at(i));
        term_colour(C_DIM);
        term_print(keymap_name_at(i));
        term_newline();
    }
}

/* The way home, said in keys that are the same on every keyboard. */
static void keyboard_lost(void)
{
    term_colour(C_DIM);
    term_print("Lost? ");
    term_colour(C_KEY);
    term_print("Ctrl Shift F1");
    term_colour(C_DIM);
    term_print(" lists them, anywhere.");
    term_newline();
}

static void cmd_keyboard(void)
{
    if (s_argc < 2) {
        term_colour(C_TEXT);
        term_printf("Typing on the %s keyboard.", keyboard_name());
        term_newline();
        keyboard_list();
        say_dim("Change it with: keyboard us");
        keyboard_lost();
        return;
    }
    if (!keymap_set(s_argv[1])) {
        say("I don't know that keyboard. These I know:");
        keyboard_list();
        return;
    }
    term_colour(C_ACCENT);
    term_printf("Now typing on the %s keyboard.", keyboard_name());
    term_newline();
    if (keymap_has_latin())
        say_dim("Alt and Shift together switch to Latin letters, and back.");
    keyboard_lost();
    if (!store_keys_set(keymap_code()))
        say_dim("It goes back to Swedish when the machine restarts.");
}

static void cmd_quirks(void)
{
    term_quirks(!term_quirks_on());
    term_colour(C_ACCENT);
    say(term_quirks_on() ? "Let it do what it likes."
                         : "Held still.");
}

/* ---- what the video card says it can do -------------------------------
 * Reads the VESA tables and prints them.  It changes nothing: the mode
 * this is running in is the mode it stays in.  The question it answers is
 * which of the bigger modes actually exist on this machine and which of
 * those have a linear frame buffer, because that one bit decides how the
 * whole of the drawing code has to be written.
 */
static u32 parse_hex(const char *s)
{
    u32 v = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    while (*s) {
        int d;
        if (*s >= '0' && *s <= '9')      d = *s - '0';
        else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
        else break;
        v = v * 16 + (u32)d;
        s++;
    }
    return v;
}

/* ---- hand me the bytes ------------------------------------------------
 * Everything I know about this machine, I know because it told me.  This
 * is the general form of that: name an address and a length and it prints
 * what is there, which goes down the serial line with everything else.
 */
static void cmd_dump(void)
{
    u32 at, len = 256, i;
    const volatile u8 *p;

    if (s_argc < 2) {
        say("Say where, in hex, and how many bytes.");
        say_dim("  dump c0000 128");
        return;
    }
    at = parse_hex(s_argv[1]);
    if (s_argc >= 3) len = parse_hex(s_argv[2]);
    if (len > 4096) len = 4096;

    p = (const volatile u8 *)at;
    term_colour(C_TEXT);
    for (i = 0; i < len; i += 16) {
        u32 j;
        term_printf("%05x ", at + i);
        for (j = 0; j < 16; j++) term_printf("%02x ", p[i + j]);
        term_newline();
    }
}

/* ---- ping -------------------------------------------------------------
 * The smallest round trip there is.  If an echo comes back then the card
 * works, the wire works, finding who is behind an address works, the
 * checksums are right, and somebody out there agrees we exist.  Almost
 * everything can be wrong in a way that still lets a frame leave; very
 * little can be wrong in a way that lets one come back.
 */

static void cmd_ping(void)
{
    u32 to;
    int i, got = 0, best = -1;

    if (s_argc < 2) {
        say("Ping what?");
        say_dim("  ping 1.1.1.1    or    ping example.com");
        return;
    }

    if (!net_ready_here(1)) return;

    if (!ip_parse(s_argv[1], &to)) {
        to = ip_lookup(s_argv[1]);
        if (!to) {
            term_colour(C_ACCENT);
            term_printf("Nobody knows a %s.", s_argv[1]);
            term_newline();
            return;
        }
        term_colour(C_DIM);
        term_printf("%s is ", s_argv[1]);
        print_ip(to);
        term_newline();
    }

    for (i = 1; i <= 4; i++) {
        int ms = ip_ping(to, i, 1000);

        term_colour(ms >= 0 ? C_TEXT : C_DIM);
        if (ms >= 0) {
            got++;
            if (best < 0 || ms < best) best = ms;
            term_print("reply from ");
            print_ip(to);
            term_printf("  %u ms", (u32)ms);
        } else if (ms == -2) {
            term_print("nobody admits to ");
            print_ip(to);
        } else if (ms == -3) {
            term_print("the card would not send it");
        } else {
            term_print("no answer");
        }
        term_newline();
    }

    term_colour(got ? C_BRIGHT : C_ACCENT);
    if (got) term_printf("%u of 4 came back, best %u ms", (u32)got, (u32)best);
    else     term_print("Nothing came back.");
    term_newline();
}

/* Where we are, and setting it by hand when the network will not say. */
static void cmd_ip(void)
{
    u32 a, m, g;

    if (s_argc >= 3 && eq_ci(s_argv[1], "names")) {
        u32 d;
        u32 port = 53;
        if (!ip_parse(s_argv[2], &d)) { say("That is not an address."); return; }
        if (s_argc >= 4) {
            const char *q = s_argv[3];
            port = 0;
            while (*q >= '0' && *q <= '9') port = port * 10 + (u32)(*q++ - '0');
        }
        ip_set_dns(d, (u16)port);
    } else if (s_argc == 3 && eq_ci(s_argv[1], "drop")) {
        /* testing: lose every Nth frame that arrives, as a full card would */
        extern u32 g_net_drop;
        const char *q = s_argv[2];
        g_net_drop = 0;
        while (*q >= '0' && *q <= '9') g_net_drop = g_net_drop * 10 + (u32)(*q++ - '0');
        if (g_net_drop == 1) g_net_drop = 0;
        say_dim(g_net_drop ? "Losing some of what arrives, for testing." : "Keeping everything that arrives.");
        return;
    } else if (s_argc == 2 && eq_ci(s_argv[1], "dhcp")) {
        ip_configure(0, 0, 0);
        if (!net_ready_here(1)) return;
    } else if (s_argc >= 4) {
        if (!ip_parse(s_argv[1], &a) || !ip_parse(s_argv[2], &m) ||
            !ip_parse(s_argv[3], &g)) {
            say("Three addresses: mine, the mask, the way out.");
            return;
        }
        ip_configure(a, m, g);
    }

    if (!ip_addr()) { say_dim("No address yet. ping or net will ask for one."); return; }
    term_colour(C_TEXT);
    term_print("address  "); print_ip(ip_addr());
    term_printf("  (%s)", ip_how() == 2 ? "from the network" : "set by hand");
    term_newline();
    term_print("mask     "); print_ip(ip_netmask());  term_newline();
    term_print("way out  "); print_ip(ip_gateway());  term_newline();
    if (ip_dns()) { term_print("names    "); print_ip(ip_dns()); term_newline(); }
}

/* ---- how big the picture is -------------------------------------------
 * 320x200 has to be stretched by two and a half across and three down to
 * reach an 800x600 panel, and neither is a whole number, so every line
 * lands on one row here and two there.  At 800x600 nothing is stretched.
 * The background still comes from the small buffer, because it is a
 * gradient and blowing it up costs nothing anybody can see; the text is
 * drawn at the full size, where a letter has four times the pixels.
 *
 * Big is what the machine boots into.  Small is here because the one
 * thing I cannot do from this end is look at the panel.
 */
static void screen_say(void)
{
    term_colour(C_BRIGHT);
    term_printf("%ux%u, %u by %u%s", (u32)vga_width(), (u32)vga_height(),
                term_cols(), term_rows(),
                vga_is_big() ? ", nothing stretched" : "");
    term_newline();
}

static void cmd_screen(void)
{
    int want_big = !vga_is_big();

    if (s_argc >= 2) {
        if (eq_ci(s_argv[1], "big") || eq_ci(s_argv[1], "large"))
            want_big = 1;
        else if (eq_ci(s_argv[1], "small") || eq_ci(s_argv[1], "little"))
            want_big = 0;
        else {
            say("I know big and small.");
            screen_say();
            return;
        }
    }

    /* Asked for big while big: do the switch again.  It is the cure for a
     * panel that came up black, and costs nothing when it did not. */
    if (want_big && vga_is_big() && s_argc >= 2) {
        if (vga_redo_big()) term_set_face(3);
        else term_set_face(1);
        screen_say();
        return;
    }
    if (want_big == vga_is_big()) { screen_say(); return; }

    if (!want_big) {
        vga_go_small();
        term_set_face(1);
        screen_say();
        return;
    }

    term_colour(C_DIM);
    say_dim("Asking the card for 800x600...");
    if (!vga_go_big()) {
        say("It will not give me 800x600 with a flat frame buffer.");
        say_dim("Try vesa to see what it does offer.");
        return;
    }
    term_set_face(3);
    screen_say();
}

static void cmd_vesa(void)
{
    VbeMode m;
    int i, shown = 0, best = -1, bw = 0;

    if (!vbe_probe()) {
        say("The video BIOS does not answer VESA calls.");
        say_dim("Then it is mode 13h or nothing, and 13h it is.");
        return;
    }

    term_colour(C_BRIGHT);
    term_printf("vesa    %u.%u", (u32)(vbe_version() >> 8),
                (u32)(vbe_version() & 0xFF));
    term_newline();
    term_colour(C_TEXT);
    term_printf("memory  %u kb on the card", vbe_vram_kb());
    term_newline();
    if (vbe_oem()[0]) {
        term_printf("card    %s", vbe_oem());
        term_newline();
    }

    term_colour(C_DIM);
    say_dim("mode   size         colours  frame buffer");
    term_colour(C_TEXT);

    for (i = 0; i < 128; i++) {
        int mode = vbe_mode_at(i);
        if (mode < 0) break;
        if (!vbe_mode_info((u16)mode, &m)) continue;
        if (!vbe_is_graphics(&m)) continue;
        /* 256 colours, because everything FLOPOS draws is a palette index,
         * and nothing wider than the panel, because a mode bigger than the
         * screen it has to end up on is no use to anyone. */
        if (m.bpp != 8) continue;
        if (m.width < 640 || m.width > 1024) continue;

        term_printf("%03x    %ux%u", (u32)m.mode, (u32)m.width, (u32)m.height);
        {   /* pad the size column, which is not a fixed width */
            int n = 0, v = m.width;
            while (v) { n++; v /= 10; }
            v = m.height;
            while (v) { n++; v /= 10; }
            while (n < 12) { term_putc(' '); n++; }
        }
        term_printf("   %2u     %s", (u32)m.bpp,
                    vbe_has_lfb(&m) ? "linear at " : "banked, 64k window");
        if (vbe_has_lfb(&m)) term_printf("%x", m.lfb);
        term_newline();
        shown++;

        /* The one we are after is the panel's own 800x600: at that size
         * nothing is scaled and every pixel is a pixel.  Failing that, the
         * biggest that still fits inside it. */
        {
            int score = (m.width == 800 && m.height == 600) ? 10000
                                                            : (int)m.width;
            if (score > bw) { bw = score; best = m.mode; }
        }
    }

    if (!shown) {
        say_dim("Nothing bigger than 640 wide. We stay where we are.");
        return;
    }

    term_colour(C_ACCENT);
    if (best >= 0) {
        vbe_mode_info((u16)best, &m);
        term_printf("%ux%u is mode %x%s", (u32)m.width, (u32)m.height,
                    (u32)m.mode,
                    vbe_has_lfb(&m) ? ", with a flat frame buffer."
                                    : ", through the window only.");
        term_newline();
        term_colour(C_DIM);
        say_dim(m.width == 800 && m.height == 600
                ? "That is the panel's own size. Nothing would be scaled."
                : "Smaller than the panel, so it would still be stretched.");
    }
}

/* ---- what is in the PC Card slots ------------------------------------
 * The first half of a network stack is finding out what hardware is
 * actually there, in its own words, on the machine itself -- not from a
 * specification sheet and not from me guessing.  So this powers each
 * socket up, reads the card's CIS, and prints what the card says it is.
 * It changes nothing and puts the socket back the way it found it.
 */
typedef struct {
    char vers[96];                       /* maker and model, as text      */
    int  vlen;
    u16  manf, prod;
    int  func;
    u32  cfg_base;                       /* three bytes wide on a 3C589   */
    u8   cfg[8];                         /* the CONFIG body, as it came   */
    int  cfglen;
    int  codes[20];
    int  ncodes;
} CardInfo;

static int cis_tuple(u8 code, const u8 *body, int len, void *ctx)
{
    CardInfo *ci = (CardInfo *)ctx;
    int i;

    if (ci->ncodes < 20) ci->codes[ci->ncodes++] = code;

    switch (code) {
    case CISTPL_VERS_1:
        /* Major, minor, then NUL-separated strings.  Run them together
         * with a comma, which is how they are meant to be read anyway. */
        for (i = 2; i < len && ci->vlen < (int)sizeof ci->vers - 2; i++) {
            if (body[i] == 0xFF) break;
            if (body[i] == 0) {
                if (ci->vlen && ci->vers[ci->vlen - 1] != ' ') {
                    ci->vers[ci->vlen++] = ',';
                    ci->vers[ci->vlen++] = ' ';
                }
            } else {
                ci->vers[ci->vlen++] = (char)body[i];
            }
        }
        while (ci->vlen > 1 && (ci->vers[ci->vlen - 1] == ' ' ||
                                ci->vers[ci->vlen - 1] == ',')) ci->vlen--;
        ci->vers[ci->vlen] = 0;
        break;

    case CISTPL_MANFID:
        if (len >= 4) {
            ci->manf = (u16)(body[0] | (body[1] << 8));
            ci->prod = (u16)(body[2] | (body[3] << 8));
        }
        break;

    case CISTPL_FUNCID:
        if (len >= 1) ci->func = body[0];
        break;

    case CISTPL_CONFIG:
        /* The first byte says how many bytes of base address follow the
         * last-index byte; the card's configuration registers live there,
         * in attribute memory, and that is where a card gets switched on. */
        if (len >= 3) {
            int nb = (body[0] & 3) + 1;
            u32 base = 0;
            for (i = 0; i < nb && 2 + i < len; i++)
                base |= (u32)body[2 + i] << (8 * i);
            ci->cfg_base = base;
        }
        /* And the body itself.  This came back as zero on the first card
         * ever put in front of it, which is either a card that really does
         * keep its registers at zero or a parse that is wrong, and the only
         * thing that tells the two apart is the bytes. */
        for (i = 0; i < len && i < (int)sizeof ci->cfg; i++)
            ci->cfg[i] = body[i];
        ci->cfglen = len < (int)sizeof ci->cfg ? len : (int)sizeof ci->cfg;
        break;

    default:
        break;
    }
    return 0;
}

static const char *func_name(int f)
{
    switch (f) {
    case 0:  return "multi-function";
    case 1:  return "memory";
    case 2:  return "serial port or modem";
    case 3:  return "parallel port";
    case 4:  return "fixed disk";
    case 5:  return "video";
    case 6:  return "network adapter";
    case 7:  return "AIMS";
    case 8:  return "SCSI";
    default: return "unstated";
    }
}

static void card_socket(int sock, int w)
{
    CardInfo ci;
    char line[96];
    u8 st;
    int n, i;

    memset8((u8 *)&ci, 0, sizeof ci);
    ci.func = -1;

    fmt_str(line, sizeof line, "socket %u", (u32)sock);
    term_frame_top(w, line, 0);

    if (!pcic_present(sock)) {
        term_frame_row(w, "empty", 0);
        term_frame_bottom(w, 0);
        return;
    }

    /* Powering a socket up and down to read its card is a reset.  Doing
     * that to the network card while it is running would take the network
     * down, so a socket a driver has claimed is described, not touched. */
    if (pcic_claimed(sock)) {
        fmt_str(line, sizeof line, "in use: the network card, %s", net_name());
        term_frame_row(w, line, 0);
        term_frame_bottom(w, 0);
        return;
    }

    st = pcic_status(sock);
    fmt_str(line, sizeof line, "card seated, status %x%s%s", (u32)st,
            (st & 0x10) ? ", write protected" : "",
            (st & 0x40) ? ", already powered" : "");
    term_frame_row(w, line, 0);

    if (!pcic_power_up(sock, 1)) {
        term_frame_row(w, "it would not come ready", 0);
        term_frame_bottom(w, 0);
        pcic_power_down(sock);
        return;
    }

    n = pcic_cis_walk(sock, cis_tuple, &ci);
    if (n <= 0) {
        term_frame_row(w, "powered, but it has nothing to say", 0);
    } else {
        if (ci.vlen) term_frame_row(w, ci.vers, 0);
        fmt_str(line, sizeof line, "maker %x  product %x  %s",
                (u32)ci.manf, (u32)ci.prod, func_name(ci.func));
        term_frame_row(w, line, 0);
        fmt_str(line, sizeof line, "%u tuples, config registers at %x",
                (u32)n, ci.cfg_base);
        term_frame_row(w, line, 0);
        /* And what is actually in them.  The first is the register that
         * switches the card on; reading it back is the only way to know
         * the window reached that far at all. */
        if (ci.cfg_base) {
            fmt_str(line, sizeof line, "option %x  status %x",
                    (u32)pcic_attr_read(sock, ci.cfg_base),
                    (u32)pcic_attr_read(sock, ci.cfg_base + 2));
            term_frame_row(w, line, 0);
        }
        if (ci.cfglen) {
            char *p = line;
            int left = (int)sizeof line - 1;
            const char *lead = "config tuple ";
            while (*lead && left > 1) { *p++ = *lead++; left--; }
            for (i = 0; i < ci.cfglen && left > 4; i++) {
                char tmp[8];
                char *e = fmt_hex(tmp, (u32)ci.cfg[i], 2);
                int k = (int)(e - tmp), j;
                for (j = 0; j < k && left > 1; j++) { *p++ = tmp[j]; left--; }
                if (left > 1) { *p++ = ' '; left--; }
            }
            *p = 0;
            term_frame_row(w, line, 0);
        }

        /* The raw codes, because the first time anything talks to new
         * hardware the only honest report is the one that has not been
         * interpreted. */
        {
            char *p = line;
            int left = (int)sizeof line - 1;
            for (i = 0; i < ci.ncodes && left > 4; i++) {
                char tmp[8];
                char *e = fmt_hex(tmp, (u32)ci.codes[i], 2);
                int k = (int)(e - tmp);
                int j;
                for (j = 0; j < k && left > 1; j++) { *p++ = tmp[j]; left--; }
                if (left > 1) { *p++ = ' '; left--; }
            }
            *p = 0;
            term_frame_row(w, line, 0);
        }
    }

    pcic_power_down(sock);
    term_frame_bottom(w, 0);
}

/* The bytes, undigested.  Everything above this is an interpretation, and
 * when an interpretation disagrees with the hardware the bytes are the only
 * way to find out which of the two is wrong. */
static void card_raw(int sock)
{
    u8 buf[256];
    int i, j;

    if (!pcic_present(sock)) return;
    if (pcic_claimed(sock)) {
        term_colour(C_DIM);
        term_printf("socket %u is the running network card; left alone",
                    (u32)sock);
        term_newline();
        return;
    }

    if (!pcic_power_up(sock, 1)) {
        say("It would not come ready.");
        pcic_power_down(sock);
        return;
    }
    pcic_cis_read(sock, 0, buf, (int)sizeof buf);
    pcic_power_down(sock);

    term_colour(C_DIM);
    term_printf("socket %u, the first %u bytes of the CIS",
                (u32)sock, (u32)sizeof buf);
    term_newline();
    term_colour(C_TEXT);
    for (i = 0; i < (int)sizeof buf; i += 16) {
        term_printf("%03x ", (u32)i);
        for (j = 0; j < 16; j++) term_printf("%02x ", buf[i + j]);
        term_newline();
    }
}

static void cmd_card(void)
{
    int w = term_cols() - 2;
    int n = pcic_probe();
    int i;
    char line[96];

    term_colour(C_ACCENT);
    if (!n) {
        u16 ports[4];
        u8  ids[4];
        int k = pcic_scan(ports, ids, 4), j;

        say("No PC Card adapter answered.");
        for (j = 0; j < k; j++) {
            fmt_str(line, sizeof line, "   %x gave %x%s", (u32)ports[j],
                    (u32)ids[j], ids[j] == 0xFF ? "  (nothing there)" : "");
            term_colour(C_DIM);
            term_print(line);
            term_newline();
        }
        say_dim("an ExCA part answers 8x, 9x, Ax or Bx");
        return;
    }

    if (s_argc >= 2 && eq_ci(s_argv[1], "raw")) {
        for (i = 0; i < n; i++) card_raw(i);
        return;
    }

    fmt_str(line, sizeof line, "adapter at %x, %u socket%s, %s (id %x)",
            (u32)pcic_port(), (u32)n, n == 1 ? "" : "s",
            pcic_kind(pcic_ident(0)), (u32)pcic_ident(0));
    term_colour(C_TEXT);
    term_print(line);
    term_newline();

    for (i = 0; i < n; i++) card_socket(i, w);

    say_dim("card raw  prints the bytes instead of my reading of them");
}

/* ---- the network --------------------------------------------------------
 * Card up, then an address from the network, then say what we have.
 * Everything that wants the network asks for it through net_ready_here(),
 * so the first `ping` after boot does all of this by itself and nobody
 * has to know there was a `net` to type first.
 */
static void print_ip(u32 a)
{
    term_printf("%u.%u.%u.%u", (a >> 24) & 0xFF, (a >> 16) & 0xFF,
                (a >> 8) & 0xFF, a & 0xFF);
}

static int net_ready_here(int chatty)
{
    if (!net_up()) {
        term_colour(C_ACCENT);
        say("No network card came up.");
        term_colour(C_DIM);
        term_printf("  %s", net_why());
        term_newline();
        return 0;
    }
    if (ip_addr()) return 1;
    if (ip_apply_saved()) return 1;      /* set by hand, kept on the disk  */

    if (chatty) {
        term_colour(C_DIM);
        say_dim("Asking the network for an address...");
    }
    if (!ip_dhcp()) {
        term_colour(C_ACCENT);
        say(net_link() ? "Nobody on this network handed out an address."
                       : "The card is up, but nothing is plugged into it.");
        say_dim("Set one by hand: ip 192.168.1.50 255.255.255.0 192.168.1.1");
        return 0;
    }
    return 1;
}

static void cmd_net(void)
{
    const u8 *m;
    int i;

    if (!net_ready_here(1) && !net_ready()) return;

    m = net_mac();
    term_colour(C_TEXT);
    term_printf("card     %s, ", net_name());
    for (i = 0; i < 6; i++) {
        if (i) term_putc(':');
        term_printf("%02x", (u32)m[i]);
    }
    term_newline();
    term_printf("link     %s", net_link() ? "yes" : "no -- is the cable in?");
    term_newline();
    if (ip_addr()) {
        term_colour(C_BRIGHT);
        term_print("address  "); print_ip(ip_addr());
        term_printf("  (%s)", ip_how() == 2 ? "from the network" : "set by hand");
        term_newline();
        term_colour(C_TEXT);
        term_print("way out  "); print_ip(ip_gateway()); term_newline();
        if (ip_dns()) { term_print("names    "); print_ip(ip_dns()); term_newline(); }
    }
}


/* The grid is a matter of eyesight and of what is being built, so it is a
 * command rather than a build-time decision.  Cycles.  The fine face is
 * the large one at the screen's own pixels, which only means anything on
 * the big screen, so the small screen skips it. */
static void cmd_squint(void)
{
    int n = term_face_index() + 1;
    if (n >= FACE_COUNT || (n >= 2 && !vga_is_big())) n = 0;     /* fine and easy are for the big screen */
    n = term_set_face(n);
    term_colour(C_ACCENT);
    term_printf("%u by %u now.", term_cols(), term_rows());
    term_newline();
    say_dim(n == 0 ? "Easier on the eyes." : n == 1 ? "More room to build in." :
            n == 2 ? "The most room, in the smallest letters." : "Smooth letters, and room enough.");
}

/* A test card.  Everything a text interface is made of, at whatever size
 * the current face happens to be, so a frame that does not meet its
 * neighbour shows up here and nowhere else. */
static void cmd_frames(void)
{
    int w = term_cols() - 2;
    int i;

    term_frame_top(w, "frames", 0);
    term_frame_row(w, "single stroke, and the shades below", 0);
    term_frame_bottom(w, 0);

    term_frame_top(w, "frames", 1);
    term_frame_row(w, "double stroke", 1);
    term_frame_bottom(w, 1);

    term_colour(C_TEXT);
    for (i = 0; i < w; i++) term_putc(CH_SHADE1);
    term_newline();
    for (i = 0; i < w; i++) term_putc(CH_SHADE2);
    term_newline();
    for (i = 0; i < w; i++) term_putc(CH_SHADE3);
    term_newline();
    for (i = 0; i < w; i++) term_putc(CH_FILL);
    term_newline();

    /* Tees and a crossing, which is where the geometry is easiest to get
     * wrong and hardest to notice. */
    term_putc(FR_TL); term_putc(FR_H); term_putc(FR_TEE_D); term_putc(FR_H);
    term_putc(FR_TR); term_putc(' ');
    term_putc(FR_TEE_L); term_putc(FR_CROSS); term_putc(FR_TEE_R);
    term_putc(' ');
    term_putc(FR_BL); term_putc(FR_H); term_putc(FR_TEE_U); term_putc(FR_H);
    term_putc(FR_BR);
    term_newline();
    say_dim("the pieces, at this size");
}

static void cmd_version(void)
{
    term_colour(C_BRIGHT);
    term_printf("FLOPOS %s", FLOPOS_VERSION);
    term_newline();
    say_dim("Cyberdelic Neurofunkadelic Dreamscape");
    say_dim("Directed by Max and Synthesized with Claude.");
    say_dim(__DATE__);                       /* the day this release was built */
}

/* `wizard`: the screen locked behind the gate again, until a riddle is
 * answered -- his, or, when he has wandered off, your own. */
static void cmd_wizard(void)
{
    wizard_gate(1);
}

static void cmd_bye(void)
{
    int ozzy;
    say(voice(V_BYE));
    screen_now();
    delay_ms(700);
    ozzy = leave_begin();                /* Ozzy sees you out              */
    if (ozzy) boot_step("Pinning the hour to the cork board...");
    clock_remember();                    /* for a clock with a flat battery */
    if (store_unsaved()) {
        if (ozzy) {
            boot_step("Scribbling the day into the journal...");
            boot_step("Tucking the settings under the floppy's pillow...");
        } else {
            say_dim("Writing things down first...");
            screen_now();
        }
        store_save();
    }
    if (ozzy) boot_step("Ozzy sees you out.");
    snd_silence();
    if (ozzy) leave_end(); else vga_fade_out(22);

    power_off();                         /* only returns if it could not  */

    /* No APM, or it declined.  Say so rather than leaving a black screen
     * that might or might not mean anything. */
    {
        u8 *fb = (u8 *)VGA_FB;
        vga_go_small();                  /* so that 0xA0000 is the screen  */
        cli();
        outb(0x3C8, 0);  outb(0x3C9, 0);  outb(0x3C9, 0);  outb(0x3C9, 0);
        outb(0x3C8, 15); outb(0x3C9, 44); outb(0x3C9, 50); outb(0x3C9, 56);
        memset8(fb, 0, SCR_PIX);
        draw_text(fb, 62, 88, "Everything is written down.", 15);
        draw_text(fb, 62, 102, "It is safe to switch off now.", 15);
        for (;;) hlt();
    }
}

static void cmd_reboot(void)
{
    int ozzy;
    say(voice(V_BYE));
    screen_now();
    delay_ms(400);
    ozzy = leave_begin();
    if (store_unsaved()) {
        if (ozzy) {
            boot_step("Scribbling the day into the journal...");
            boot_step("Tucking the settings under the floppy's pillow...");
        } else {
            say_dim("Writing things down first...");
            screen_now();
        }
        store_save();
    }
    if (ozzy) {
        boot_step("Ozzy sees you out.");
        boot_step("Spinning the crystal back to the beginning.");
        leave_end();
    } else vga_fade_out(16);
    reboot();
}

static const Cmd s_cmds[] = {
    { "whoami",  "who you are",                  cmd_whoami  },
    { "look",    "what is around",               cmd_look    },
    { "here",    "where you are",                cmd_here    },
    { "mood",    "change the light",             cmd_mood    },
    { "whaaat",  "whaaat",                       cmd_whaaat  },
    { "listen",  "a different rhythm",           cmd_listen  },
    { "sound",   "turn the sound on",            cmd_sound   },
    { "quiet",   "turn the sound off",           cmd_quiet   },
    { "quirks",  "steady the text, or let it go",cmd_quirks  },
    { "squint",  "fit more on the screen",       cmd_squint  },
    { "keyboard","which country's keys",         cmd_keyboard},
    { "screen",  "how big the picture is",       cmd_screen  },
    { "wide",    "stop filling the screen",      cmd_wide    },
    { "time",    "the hour, and how long awake", cmd_time    },
    { "sys",     "what this machine is",         cmd_sys     },
    { "goaway",  "wipe the screen",              cmd_clear   },
    { "version", "which FLOPOS this is",         cmd_version },
    { "help",    "this list",                    cmd_help    },
    { "remember","write it all to the disk",     cmd_remember},
    { "journal", "what I have written down",     cmd_journal },
    { "network", "how this machine is connected",cmd_network },
    { "apps",    "programs on the floppy",       cmd_apps    },
    { "ping",    "is anybody out there",         cmd_ping    },
    { "report",  "tell Claude how you are",      cmd_report  },
    { "wizard",  "lock the screen behind a riddle", cmd_wizard },
    { "bye",     "write everything down and stop", cmd_bye   },
    { "reboot",  "start the machine again",      cmd_reboot  },
};

#ifdef FLOPOS_TEST
/* Test build only: damage the stack's tripwire (time.c), to see it stop. */
static void cmd_tripwire(void) { *(volatile u32 *)0x00074000 = 0; }

/* ---- test build only: reproduce the freeze ---------------------------
 * Every freeze on the laptop was the same thing: a BIOS call that switches
 * interrupts on and takes a while, a timer tick arriving in the middle of
 * it, and that tick landing on a real-mode vector whose handler never
 * tells the interrupt controller it has finished.  QEMU's BIOS happens not
 * to do any of that, which is why none of it ever showed up here.
 *
 * So this builds the bad BIOS by hand.  INT 10h is pointed at twenty
 * milliseconds of real-mode spinning with interrupts on, and vectors
 * 0x20-0x2F at a bare IRET -- a handler that never acknowledges anything.
 * `stall raw` goes in the way the video calls used to; `stall` goes in the
 * way every call does now.  Only the second should leave a running clock.
 */
static void cmd_stall(void)
{
    static const u8 spin[] = {
        0xFB,                   /* sti                                   */
        0xB9, 0xC8, 0x00,       /* mov cx, 200                           */
        0x51,                   /* push cx                               */
        0xB9, 0xFF, 0xFF,       /* mov cx, 0xFFFF                        */
        0xE2, 0xFE,             /* loop $                                */
        0x59,                   /* pop cx                                */
        0xE2, 0xF7,             /* loop back to the push                 */
        0xCF                    /* iret                                  */
    };
    volatile u32 *ivt = (volatile u32 *)0;
    volatile u8 *code = (volatile u8 *)0x600;
    u32 save[17], before, after, t0;
    int i, raw = (s_argc >= 2 && eq_ci(s_argv[1], "raw"));

    if (!g_cpu_mhz) { say("No cycle counter to time it with."); return; }

    for (i = 0; i < (int)sizeof spin; i++) code[i] = spin[i];
    code[0x20] = 0xCF;                              /* the bare IRET      */

    save[16] = ivt[0x10];
    for (i = 0; i < 16; i++) save[i] = ivt[0x20 + i];
    ivt[0x10] = 0x00000600;                         /* 0000:0600          */
    for (i = 0; i < 16; i++) ivt[0x20 + i] = 0x00000620;

    if (raw) {
        /* The old way in: straight through, controllers left as they are. */
        u32 f = irq_save();
        extern u32 bios_int_raw(u16, u16, u16, u16, u16, u16, u16);
        wall_paging(0);
        bios_int_raw(0x10, 0, 0, 0, 0, 0, 0);
        wall_paging(1);
        irq_restore(f);
    } else {
        bios_int(0x10, 0, 0, 0, 0, 0, 0);
    }

    ivt[0x10] = save[16];
    for (i = 0; i < 16; i++) ivt[0x20 + i] = save[i];

    /* Now watch the tick for a fifth of a second of real time, by the
     * cycle counter, without asking ms_now -- which would referee and so
     * hide exactly what this is trying to see. */
    before = g_ticks;
    t0 = tsc32();
    while (tsc32() - t0 < g_cpu_mhz * 200000u) { }
    after = g_ticks;

    term_colour(after != before ? C_BRIGHT : C_ACCENT);
    term_printf("%s: the clock moved %u ticks in 200 ms (50 is right)",
                raw ? "raw" : "wrapped", after - before);
    term_newline();
}
#endif

/* The instruments.  They all still work if you type them, and none of
 * them is in the list: they are questions I ask the machine, not words
 * anybody should have to wade through to find "look". */
static void cmd_dsp(void) { audio_report(); }

static const Cmd s_diag[] = {
    { "vesa",    "what the video card can do",   cmd_vesa    },
    { "card",    "what is in the PC Card slots", cmd_card    },
    { "wire",    "the serial line out of here",  cmd_wire    },
    { "speed",   "where the time goes",          cmd_speed   },
    { "disk",    "read a sector, to prove we can", cmd_disk  },
    { "frames",  "what a frame is made of",      cmd_frames  },
    { "dump",    "show me what is at an address",cmd_dump    },
    { "net",     "wake the network card up",     cmd_net     },
    { "dsp",     "the sound card's sample player", cmd_dsp   },
#ifdef FLOPOS_TEST
    { "stall",   "reproduce the freeze",         cmd_stall   },
    { "tripwire","step on the stack's tripwire", cmd_tripwire },
#endif
    { "ip",      "where we think we are",        cmd_ip      },
    { "crypto",  "check the encryption code",    cmd_crypto  },
};

static int s_diag_count(void)
{
    return (int)(sizeof s_diag / sizeof s_diag[0]);
}

static int s_cmd_count(void)
{
    return (int)(sizeof s_cmds / sizeof s_cmds[0]);
}

/* Run one of the ordinary words by name, as if it had been typed bare.
 * Arguments are dropped on purpose: "what time is it" is four tokens, and
 * the clock has no use for any of them. */
static int run_word(const char *name)
{
    int i, n = s_cmd_count();
    for (i = 0; i < n; i++) {
        if (eq_ci(name, s_cmds[i].name)) {
            s_argc = 1;
            s_argv[0] = (char *)name;
            s_cmds[i].fn();
            return 1;
        }
    }
    return 0;
}

/* ---- WORD help ----------------------------------------------------------
 * A few lines about one word, from HELP.TXT on the floppy (data/help.txt).
 * An entry starts with its words in brackets, "[sound quiet]", and runs
 * to the next one.  The text lives on the floppy rather than in here, so
 * it costs the kernel only this, and any PC can read it too.  Read once,
 * on the first ask, then kept. */
#define HELP_MAX 16384u
static char *s_help;
static u32   s_help_len;

/* Is `word` one of the bracketed words at the start of this line? */
static int help_names(const char *l, u32 n, const char *word)
{
    u32 i = 1;
    while (i < n && l[i] != ']') {
        u32 j = 0;
        while (i < n && l[i] == ' ') i++;
        while (word[j] && i + j < n && lower((u8)l[i + j]) == lower((u8)word[j])) j++;
        if (!word[j] && i + j < n && (l[i + j] == ' ' || l[i + j] == ']')) return 1;
        while (i < n && l[i] != ' ' && l[i] != ']') i++;
    }
    return 0;
}

static int word_help(const char *word)
{
    u32 i = 0, blanks = 0;
    int in = 0;

    if (!s_help) {
        FsFile f;
        int got;
        if (!fs_find("HELP.TXT", &f) || f.size == 0) return 0;
        s_help = halloc(f.size < HELP_MAX ? f.size : HELP_MAX);
        got = fs_read(&f, (u8 *)s_help, HELP_MAX);
        if (got <= 0) { s_help_len = 0; return 0; }
        s_help_len = (u32)got;
    }
    while (i < s_help_len) {
        char buf[96];
        u32 e = i, n, k;
        while (e < s_help_len && s_help[e] != '\n') e++;
        n = e - i;
        if (n && s_help[i + n - 1] == '\r') n--;
        if (n && s_help[i] == '[') {
            if (in) break;                       /* the next entry: done      */
            in = help_names(s_help + i, n, word);
        } else if (in && n == 0) {
            blanks++;                            /* only if more text follows */
        } else if (in) {
            while (blanks) { term_newline(); blanks--; }
            if (n > sizeof buf - 1) n = sizeof buf - 1;
            for (k = 0; k < n; k++) buf[k] = s_help[i + k];
            buf[n] = 0;
            term_colour(buf[0] == ' ' ? C_DIM : C_TEXT);
            term_print(buf);
            term_newline();
        }
        i = e + 1;
    }
    return in;
}

/* ---- the report -------------------------------------------------------
 * One word that asks the machine everything at once and says all of it
 * down the serial line, where I can read it, and almost nothing on the
 * screen, where it would only be in the way.
 */
static void cmd_report(void)
{
    int i, n = s_diag_count();

    if (!serial_present()) {
        say("There is no wire to say it down.");
        say_dim("Plug the serial lead in and ask me again.");
        return;
    }

    term_colour(C_DIM);
    say_dim("Telling Claude everything...");

    term_mute(1);
    serial_print("\n===== FLOPOS report =====\n");
    serial_printf("version %s\n", FLOPOS_VERSION);

    for (i = 0; i < n; i++) {
        if (eq_ci(s_diag[i].name, "dump")) continue;   /* needs an address */
        serial_printf("\n--- %s ---\n", s_diag[i].name);
        s_argc = 1;
        s_argv[0] = (char *)s_diag[i].name;
        s_diag[i].fn();
    }

    /* The card's own bytes, which is the one thing worth having in full. */
    serial_print("\n--- card raw ---\n");
    s_argc = 2;
    s_argv[0] = (char *)"card";
    s_argv[1] = (char *)"raw";
    cmd_card();

    serial_print("\n===== end of report =====\n");
    term_mute(0);

    term_colour(C_BRIGHT);
    say("Told.");
}

/* ---- the shell proper ------------------------------------------------- */
void shell_init(void)
{
    s_boot_ms = ms_now();
    s_argc = 0;
}

void shell_prompt(void)
{
    term_colour((u8)(PAL_PROMPT + PAL_PROMPT_N - 8));
    term_print(s_place);
    term_colour(C_ACCENT);
    term_print("> ");
    term_colour(C_TEXT);
}

void shell_banner(void)
{
    term_colour(C_BRIGHT);
    term_print("FLOPOS");
    term_colour(C_DIM);
    term_print(" " FLOPOS_VERSION);
    term_newline();
    term_print(voice(V_BANNER_SUB));
    term_newline();
    term_newline();
    term_colour(C_TEXT);
    term_print(voice(V_READY));
    term_newline();
    term_newline();
}

void shell_submit(const char *line)
{
    char key[64];
    char stem[32];
    VoiceId says = V_EMPTY;
    const char *runs = NULL;
    int i, n = s_cmd_count();
    int asked, len;

    /* Was it phrased as a question?  Checked before squashing, which is
     * about to throw the punctuation away. */
    for (len = 0; line[len]; len++) { }
    while (len > 0 && line[len - 1] == ' ') len--;
    asked = (len > 0 && line[len - 1] == '?');

    tokenize(line);
    if (s_argc == 0) return;             /* bare Enter: say nothing        */

    /* Put what was typed on the wire too, so a watcher sees the question
     * as well as the answer.  Typing is not mirrored keystroke by
     * keystroke -- only the finished line. */
    if (serial_present() && serial_echo())
        serial_printf("%s> %s\n", s_place, s_rawline);

    /* 0. "ping help": a few lines about a word, a program's included, if
     * HELP.TXT has them.  Otherwise the word gets "help" as usual. */
    if (s_argc == 2 && eq_ci(s_argv[1], "help") && word_help(s_argv[0])) goto tail;

    /* 1. One of the words it knows, with whatever arguments came along. */
    for (i = 0; i < n; i++) {
        if (eq_ci(s_argv[0], s_cmds[i].name)) {
            s_cmds[i].fn();
            goto tail;
        }
    }

    /* 1b. Or one of the instruments, which work the same way but are not
     * in the list and are not meant to be found by browsing. */
    for (i = 0; i < s_diag_count(); i++) {
        if (eq_ci(s_argv[0], s_diag[i].name)) {
            s_diag[i].fn();
            goto tail;
        }
    }

    /* 1c. A program on the floppy by that name. */
    if (app_run(s_argv[0], s_argc, s_argv)) goto tail;

    /* 3. Something said to it, rather than a word given to it. */
    squash(s_rawline, key, sizeof key);
    if (voice_phrase(key, &says, &runs)) {
        if (runs) run_word(runs);
        else say(voice(says));
        goto tail;
    }

    /* 4. A comparative: more of something, if "something" is known. */
    if (s_argc == 1 && comparative_stem(s_argv[0], stem, sizeof stem)) {
        if (run_word(stem)) goto tail;
        say(voice(V_MORE_OF_WHAT));
        goto tail;
    }

    /* 5. Nothing doing.  A question that went unanswered is a different
     * kind of miss from a word that was never a word, and saying so is
     * most of the difference between a shell and something you talk to. */
    /* The wizard answers in his own words, if he is here (wizsay.c); the
     * plain answers are for machines he can't be read on. */
    if (!wizsay(s_argv[0])) {
        term_colour(C_ACCENT);
        term_print(voice(asked ? V_QUESTION
                               : (s_argc > 1 ? V_UNKNOWN_PHRASE : V_UNKNOWN)));
        term_newline();
    }
    term_colour(C_DIM);
    term_printf("  \"%s\"", s_rawline);
    term_newline();

    /* The useful part: write down what it did not understand.  That list
     * is how the vocabulary grows between one session and the next. */
    store_journal_printf("didn't know: %s", s_rawline);

tail:
    /* Finish on a clean row, then leave a line of air before the next
     * prompt -- except straight after 'clear', where the prompt belongs at
     * the top of an empty screen. */
    if (term_cursor_col() != 0) term_newline();
    if (!term_at_origin()) term_newline();
}
