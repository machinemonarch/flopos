/* ===========================================================================
 * shell.c -- the prompt, the tokeniser, and the words FLOPOS knows.
 *
 * Adding a word is one line in s_cmds plus a function.  The tokeniser
 * understands double quotes, which nothing needs yet but 'teach' will.
 *
 * Nothing here holds any of its own prose: every string the user sees
 * comes from voice.c, so the register can be rewritten in one place.
 * ===========================================================================
 */
#include "flopos.h"
#include "io.h"
#include "version.h"

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
}

static void cmd_clear(void)
{
    term_clear();
}

static void cmd_echo(void)
{
    int i;
    if (s_argc < 2) { say(voice(V_ECHO_NOTHING)); return; }
    term_colour(C_TEXT);
    for (i = 1; i < s_argc; i++) {
        if (i > 1) term_putc(' ');
        term_print(s_argv[i]);
    }
    term_newline();
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
        term_printf("memory  %u words taught, %u boots so far",
                    store_taught_count(), store_boots());
        term_newline();
    }
    term_printf("a20     %s", BOOTINFO_A20 ? "open" : "unverified");
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
    term_printf("blit    %4u us   64000 bytes to the card", perf_us(2));
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

static void cmd_teach(void)
{
    char resp[RESP_MAX];
    int i, n = 0;

    if (!store_ready()) {
        say("I have nowhere to keep it. The disk did not answer.");
        return;
    }
    if (s_argc < 3) {
        say("Say it like this:");
        say_dim("  teach frobnicate \"Done. Probably.\"");
        return;
    }

    /* Everything after the word is the answer, quoted or not. */
    for (i = 2; i < s_argc && n < RESP_MAX - 2; i++) {
        const char *w = s_argv[i];
        if (i > 2) resp[n++] = ' ';
        while (*w && n < RESP_MAX - 1) resp[n++] = *w++;
    }
    resp[n] = 0;

    i = store_teach(s_argv[1], resp);
    if (i < 0) {
        say("I am full. Make me forget something first.");
        return;
    }
    if (!i) { say("That will not do as a word."); return; }

    term_colour(C_ACCENT);
    say("Learned.");
    store_journal_printf("taught: %s = %s", s_argv[1], resp);
    if (store_save()) say_dim("Written to the disk. I will still know it tomorrow.");
    else say("I could not write it down, so I will forget by morning.");
}

static void cmd_forget(void)
{
    if (!store_ready()) { say("Nothing is kept anyway."); return; }
    if (s_argc < 2) { say("Forget what?"); return; }

    if (store_forget(s_argv[1])) {
        say("Gone.");
        store_journal_printf("forgot: %s", s_argv[1]);
        store_save();
    } else {
        say("I never knew that one.");
    }
}

static void cmd_learned(void)
{
    int i, n = store_taught_count();

    if (!n) { say_dim("Nothing yet. Teach me something."); return; }

    term_colour(C_DIM);
    term_printf("%u word%s, taught by you:", n, n == 1 ? "" : "s");
    term_newline();
    for (i = 0; i < n; i++) {
        const Taught *t = store_taught_at(i);
        int w = 0;
        term_colour(C_BRIGHT);
        term_print("  ");
        term_print(t->word);
        while (t->word[w]) w++;
        while (w < 12) { term_putc(' '); w++; }
        term_colour(C_DIM);
        term_print(t->resp);
        term_newline();
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
    mood_draw(g_back, ms_now());
    term_render(g_back, ms_now());
    vga_present();

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
/* ---- the whole panel --------------------------------------------------
 * 320x200 has to be stretched by two and a half across and three down to
 * reach an 800x600 panel, and neither is a whole number, so every line
 * lands on one row here and two there.  At 800x600 nothing is stretched.
 * The background still comes from the small buffer, because it is a
 * gradient and blowing it up costs nothing anybody can see; the text is
 * drawn at the full size, where a letter has four times the pixels.
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
 * The video BIOS at c0000, the BIOS data area, a card's registers -- it is
 * all just memory, and reading it changes nothing.
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

static void cmd_big(void)
{
    if (vga_is_big()) {
        vga_go_small();
        term_set_face(1);
        term_colour(C_ACCENT);
        term_printf("Back to 320x200, %u by %u.", term_cols(), term_rows());
        term_newline();
        say_dim("Say it again to go back up.");
        return;
    }

    term_colour(C_DIM);
    say_dim("Asking the card for 800x600...");

    if (!vga_go_big()) {
        say("It would not give me 800x600 with a flat frame buffer.");
        say_dim("Try vesa to see what it does offer.");
        return;
    }

    /* The roomy face at double size: bigger than the small face is now,
     * smoother than either, and 66 by 37 on the screen. */
    term_set_face(0);
    term_colour(C_BRIGHT);
    term_printf("%u by %u, and nothing is being stretched.",
                term_cols(), term_rows());
    term_newline();
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
            u8 cor[4];
            pcic_cis_read(sock, ci.cfg_base, cor, 4);
            fmt_str(line, sizeof line, "option %x  status %x",
                    (u32)cor[0], (u32)cor[2]);
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

/* The grid is a matter of eyesight and of what is being built, so it is a
 * command rather than a build-time decision.  Cycles, because there are
 * only two and there will be more. */
static void cmd_squint(void)
{
    int n = term_set_face(term_face_index() + 1 >= FACE_COUNT
                          ? 0 : term_face_index() + 1);
    term_colour(C_ACCENT);
    term_printf("%u by %u now.", term_cols(), term_rows());
    term_newline();
    say_dim(n == 0 ? "Easier on the eyes." : "More room to build in.");
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
    say_dim("built from nothing in particular");
}

static void cmd_bye(void)
{
    if (store_unsaved()) {
        say_dim("Writing things down first...");
        term_render(g_back, ms_now());
        vga_present();
        store_save();
    }

    say(voice(V_BYE));
    term_render(g_back, ms_now());
    vga_present();
    delay_ms(700);

    snd_silence();
    vga_fade_out(22);

    power_off();                         /* only returns if it could not  */

    /* No APM, or it declined.  Say so rather than leaving a black screen
     * that might or might not mean anything. */
    {
        u8 *fb = (u8 *)VGA_FB;
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
    if (store_unsaved()) {
        say_dim("Writing things down first...");
        term_render(g_back, ms_now());
        vga_present();
        store_save();
    }
    say(voice(V_BYE));
    term_render(g_back, ms_now());
    vga_present();
    delay_ms(400);
    vga_fade_out(16);
    reboot();
}

static const Cmd s_cmds[] = {
    { "whoami",  "who you are",                  cmd_whoami  },
    { "look",    "what is around",               cmd_look    },
    { "here",    "where you are",                cmd_here    },
    { "mood",    "change the light",             cmd_mood    },
    { "wide",    "stop filling the screen",      cmd_wide    },
    { "whaaat",  "whaaat",                       cmd_whaaat  },
    { "listen",  "a different rhythm",           cmd_listen  },
    { "sound",   "turn the sound on",            cmd_sound   },
    { "quiet",   "turn the sound off",           cmd_quiet   },
    { "quirks",  "steady the text, or let it go",cmd_quirks  },
    { "squint",  "fit more on the screen",       cmd_squint  },
    { "frames",  "what a frame is made of",      cmd_frames  },
    { "card",    "what is in the PC Card slots",  cmd_card    },
    { "vesa",    "what the video card can do",   cmd_vesa    },
    { "big",     "big screen, or back to small", cmd_big     },
    { "dump",    "show me what is at an address", cmd_dump    },
    { "time",    "the hour, and how long awake", cmd_time    },
    { "sys",     "what this machine is",         cmd_sys     },
    { "speed",   "where the time goes",          cmd_speed   },
    { "wire",    "the serial line out of here",  cmd_wire    },
    { "disk",    "read a sector, to prove we can", cmd_disk   },
    { "goaway",  "wipe the screen",              cmd_clear   },
    { "echo",    "say something back",           cmd_echo    },
    { "version", "which FLOPOS this is",         cmd_version },
    { "help",    "this list",                    cmd_help    },
    { "teach",   "give me a new word",           cmd_teach   },
    { "forget",  "take one away",                cmd_forget  },
    { "learned", "what you have taught me",      cmd_learned },
    { "remember","write it all to the disk",     cmd_remember},
    { "journal", "what I have written down",     cmd_journal },
    { "bye",     "write everything down and stop", cmd_bye   },
    { "reboot",  "start the machine again",      cmd_reboot  },
};

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
    term_newline();
    term_colour(C_DIM);
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

    /* 1. One of the words it knows, with whatever arguments came along. */
    for (i = 0; i < n; i++) {
        if (eq_ci(s_argv[0], s_cmds[i].name)) {
            s_cmds[i].fn();
            goto tail;
        }
    }

    /* 2. Anything you taught it. */
    if (s_argc == 1) {
        const char *learned = store_lookup(s_argv[0]);
        if (learned) { say(learned); goto tail; }
    }

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
    term_colour(C_ACCENT);
    term_print(voice(asked ? V_QUESTION
                           : (s_argc > 1 ? V_UNKNOWN_PHRASE : V_UNKNOWN)));
    term_newline();
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
