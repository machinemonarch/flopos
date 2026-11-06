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
    term_printf("video   vga mode 13h, 320x200, 256 colours");
    term_newline();
    if (panel_present())
        term_printf("panel   flat panel found, %s",
                    panel_wide() ? "stretched to fill" : "centred");
    else
        term_printf("panel   no flat panel controller answered");
    term_newline();
    term_printf("screen  %u columns by %u rows", TERM_COLS, TERM_ROWS);
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
