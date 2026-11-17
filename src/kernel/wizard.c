/* ===========================================================================
 * wizard.c -- the gate to cyberdelia.
 *
 * Between the boot and the shell sits a very tired wizard, on top of the
 * box where you would log in, and he will not let anybody into cyberdelia
 * -- the shell -- without answering a riddle first.  Every answer is
 * correct.  He has stopped checking.
 *
 * There is nothing here clever enough to write a riddle, so they are
 * assembled: a question word, then adjectives, nouns and verbs drawn from
 * word lists into one of a few dozen shapes, in whatever order the shape
 * puts them.  What gets drawn is decided by the clock: the CMOS time and
 * date and the processor's cycle counter, stirred together (see
 * reckon()), so no two boots ask the same thing.
 *
 * The riddle and the answer go into the journal, so the floppy remembers
 * what was asked and what was said.
 *
 * `wizard` brings the gate back, to lock the screen: the shell is hidden
 * until a riddle is answered again.  Some of the time he is not there --
 * gone off to do something silly -- and has left a note asking whoever
 * comes along to riddle themselves while he is away.
 * ===========================================================================
 */
#include "flopos.h"
#include "io.h"

/* ---- the words ------------------------------------------------------------ */
static const char *s_adj[] = {
    "sleepy", "purple", "damp", "electric", "forgotten", "wobbly", "sacred",
    "tiny", "enormous", "haunted", "sticky", "glowing", "ancient", "beige",
    "invisible", "crunchy", "melancholy", "fluorescent", "lukewarm", "cosmic",
    "rubbery", "velvet", "suspicious", "soggy", "humming", "polite", "feral",
    "magnetic", "upside-down", "dusty", "translucent", "bewildered",
    "gelatinous", "holy", "squeaky", "nocturnal", "lopsided", "enchanted",
    "unplugged", "ticklish", "overcooked", "astral", "wistful", "crumbly",
    "shy", "infinite", "peculiar", "fuzzy", "grumpy", "oblong", "sideways",
    "eight-bit",
};
static const char *s_noun[] = {
    "banana", "floppy", "toaster", "moth", "teapot", "modem", "pixel", "sock",
    "moon", "cassette", "pigeon", "kettle", "spoon", "cathedral", "mushroom",
    "robot", "potato", "dragon", "keyboard", "lighthouse", "cabbage", "comet",
    "goblin", "accordion", "noodle", "pancake", "trombone", "walrus",
    "meatball", "turnip", "galaxy", "sandwich", "owl", "snail", "lantern",
    "tortoise", "cucumber", "wombat", "cloud", "crystal", "beetle", "mitten",
    "gnome", "button", "satellite", "waffle", "ghost", "cauldron",
    "mainframe", "diskette", "pickle", "pebble", "unicorn", "cat",
};
static const char *s_verb[] = {
    "whisper", "wobble", "dance", "hum", "forget", "dream", "sneeze",
    "juggle", "melt", "giggle", "yodel", "ponder", "flicker", "nibble",
    "wander", "glow", "tickle", "hiccup", "sulk", "levitate", "simmer",
    "shimmer", "gargle", "moonwalk", "knit", "mumble", "sparkle", "waddle",
    "snore", "vanish", "purr", "scuttle", "unravel", "rattle", "whistle",
    "blink", "hover", "brood", "fizz", "wiggle", "buzz", "polish",
};
#define N_OF(a) ((int)(sizeof a / sizeof a[0]))

/* The shapes.  %A an adjective, %N a noun, %P a noun in the plural, %V a
 * verb as it comes, %S a verb as "it" does it.  Each one starts with a
 * question word, and none of them puts its words in the same order. */
static const char *s_shape[] = {
    "What %A %N %S when the %N is %A?",
    "Why does the %A %N %V?",
    "How many %P does it take to %V a %A %N?",
    "Where do %A %P go to %V?",
    "When is a %N not a %N, but a %A %N?",
    "Who %S the %N, and why is it so %A?",
    "Which is more %A: a %N that %S, or a %N that %S?",
    "What has %A %P but cannot %V?",
    "How does a %A %N %V without a %N?",
    "Why do %P %V at midnight?",
    "What is %A, %A, and %S in the %N?",
    "When the %N %S, who %S the %A %N?",
    "Where is the %A %N that %S?",
    "Who would %V a %N, if not a %A %N?",
    "What %S but never %S, and is %A on Tuesdays?",
    "How %A must a %N be before it %S?",
    "Which %N %S loudest in a %A %N?",
    "Why is a %N more %A than a %N that %S?",
    "Where does a %N %V when the %P are %A?",
    "What do you call a %A %N that cannot %V?",
};

/* ---- the reckoning --------------------------------------------------------
 * Every boot gets its own riddle, from the only things this machine has
 * that change: the time on the CMOS clock, the date, and how many cycles
 * the processor has counted since power came on.  They are folded
 * together a byte at a time (FNV-1a), the cycle count is stirred in
 * twice, and then each word is drawn with a xorshift.  Arbitrary, as
 * promised, and good enough that nobody will see the same riddle twice. */
static u32 s_luck;

static u32 fold(u32 h, u32 v)
{
    int i;
    for (i = 0; i < 4; i++) { h ^= (v >> (i * 8)) & 0xFF; h *= 16777619u; }
    return h;
}

static void reckon(void)
{
    u8 h = 0, mi = 0, se = 0, mo = 0, da = 0;
    u16 yr = 0;
    u32 x = 2166136261u;
    u32 c = g_has_tsc ? tsc32() : 0;

    (void)rtc_read(&h, &mi, &se, &yr, &mo, &da);
    x = fold(x, c);
    x = fold(x, ((u32)h << 16) | ((u32)mi << 8) | se);
    x = fold(x, ((u32)yr << 16) | ((u32)mo << 8) | da);
    x = fold(x, ms_now());
    x = fold(x, c * 2654435761u);
    s_luck = x ? x : 0x9E3779B9u;
}

static u32 luck(void)
{
    s_luck ^= s_luck << 13;
    s_luck ^= s_luck >> 17;
    s_luck ^= s_luck << 5;
    return s_luck;
}

static const char *pick(const char **list, int n) { return list[luck() % (u32)n]; }

/* ---- English, a little --------------------------------------------------- */
static int ends(const char *w, const char *tail)
{
    int a = strlen8(w), b = strlen8(tail);
    int i;
    if (b > a) return 0;
    for (i = 0; i < b; i++) if (w[a - b + i] != tail[i]) return 0;
    return 1;
}

static int vowel(char c) { return c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u'; }

/* "it hums", "it fizzes", "the moths", "the galaxies".  The word lists
 * hold nothing that breaks these rules. */
static void suffix_s(char *out, int cap, const char *w)
{
    int n = strlen8(w), i;
    for (i = 0; i < n && i < cap - 4; i++) out[i] = w[i];
    out[i] = 0;
    if (ends(w, "s") || ends(w, "sh") || ends(w, "ch") || ends(w, "x") ||
        ends(w, "z") || ends(w, "o")) {
        out[i++] = 'e'; out[i++] = 's';
    } else if (n >= 2 && w[n - 1] == 'y' && !vowel(w[n - 2])) {
        out[i - 1] = 'i'; out[i++] = 'e'; out[i++] = 's';
    } else
        out[i++] = 's';
    out[i] = 0;
}

#define RIDDLE_MAX 160

static void make_riddle(char *out)
{
    const char *s = pick(s_shape, N_OF(s_shape));
    char word[32];
    int n = 0;

    while (*s && n < RIDDLE_MAX - 32) {
        const char *w = 0;
        if (*s == '%' && s[1]) {
            switch (s[1]) {
            case 'A': w = pick(s_adj, N_OF(s_adj)); break;
            case 'N': w = pick(s_noun, N_OF(s_noun)); break;
            case 'V': w = pick(s_verb, N_OF(s_verb)); break;
            case 'P': suffix_s(word, sizeof word, pick(s_noun, N_OF(s_noun))); w = word; break;
            case 'S': suffix_s(word, sizeof word, pick(s_verb, N_OF(s_verb))); w = word; break;
            }
            s += 2;
            /* "a" before a vowel is "an". */
            if (w && n >= 2 && out[n - 1] == ' ' && out[n - 2] == 'a' &&
                (n == 2 || out[n - 3] == ' ') && vowel(w[0])) {
                out[n - 1] = 'n';
                out[n++] = ' ';
            }
            while (w && *w) out[n++] = *w++;
            continue;
        }
        out[n++] = *s++;
    }
    out[n] = 0;
}

/* ---- the wizard ------------------------------------------------------------
 * Fifteen cells wide and ten high, sitting on the top edge of the box.
 * The second picture says what colour each cell is: Y the star, H the
 * hat, F the face, B the beard, R the robe, S the staff, O the orb.
 */
static const char *s_art[10] = {
    "      *        ",
    "     /^\\       ",
    "    / . \\      ",
    "   /  *  \\     ",
    "  /_______\\  o ",
    "   ( -.- )   | ",
    "  /\\wwwww/\\  | ",
    " /  WWWWW  \\ | ",
    " |  WWWWW  | | ",
    " \\___WWW___/ | ",
};
static const char *s_key[10] = {
    "      Y        ",
    "     HHH       ",
    "    HHYHH      ",
    "   HHHYHHH     ",
    "  HHHHHHHHH  O ",
    "   FFFFFFF   S ",
    "  RRBBBBBRR  S ",
    " R  BBBBB  R S ",
    " R  BBBBB  R S ",
    " RRRRBBBRRRR S ",
};
#define ART_W 15
#define ART_H 10

/* When he is away: his hat on a post, and the note pinned under it.
 * P the paper, I the ink. */
static const char *s_away_art[10] = {
    "      *        ",
    "     /^\\       ",
    "    /___\\      ",
    "      |        ",
    "  .--------.   ",
    "  | ~~ ~~~ |   ",
    "  | ~~~ ~  |   ",
    "  |   ~W.  |   ",
    "  '--------'   ",
    "      |        ",
};
static const char *s_away_key[10] = {
    "      Y        ",
    "     HHH       ",
    "    HHHHH      ",
    "      S        ",
    "  PPPPPPPPPP   ",
    "  PIIIIIIIIP   ",
    "  PIIIIIIIIP   ",
    "  PIIIIIIIIP   ",
    "  PPPPPPPPPP   ",
    "      S        ",
};

static const char *s_gone[] = {
    "teach the moon to whistle",
    "count the stairs of the spiral staircase, again",
    "argue with a teapot about the weather",
    "look for my other sock in 1987",
    "help Saffran keep an eye on a sunbeam",
    "return an overdue spell to the library",
    "polish the star on my hat",
    "untangle a rainbow that got into the drawer",
    "ask a cloud for directions",
    "find out where the floppy goes when it spins",
    "nap somewhere quieter",
    "water the crystal",
};
static const char *s_back[] = {
    "Back soon-ish.",
    "Back before the kettle boils. Or after.",
    "Back when I remember the way.",
    "Back in a jiffy, or a fortnight.",
};
static const char *s_alone[] = {
    "The note says nothing back. It is a note. In you go.",
    "You nod at your own answer. It seems right. It always does.",
    "On the back of the note, in smaller writing: Correct. In you go.",
    "Somewhere far off, a wizard yawns in agreement. In you go.",
};

static u8 art_colour(char k, u32 t)
{
    switch (k) {
    case 'P': return C_TEXT;
    case 'I': return C_ACCENT;
    case 'Y': return PAL_BANANA + 6;
    case 'H': return C_ACCENT;
    case 'F': return C_TEXT;
    case 'B': return C_BRIGHT;
    case 'R': return PAL_ACCENT + 12;
    case 'S': return C_GREY;
    case 'O': return (u8)(PAL_ACCENT + 16 + ((SIN((int)(t / 4)) * 15) >> 12));
    }
    return C_TEXT;
}

static void draw_wizard(int x0, int y0, u32 t)
{
    int r, c;
    u32 yawn = t % 7000;

    for (r = 0; r < ART_H; r++)
        for (c = 0; c < ART_W; c++) {
            char ch = s_art[r][c];
            if (ch == ' ') continue;
            term_win_put_at(x0 + c, y0 + r, (u8)ch, art_colour(s_key[r][c], t), 0, ATTR_CLEAR);
        }

    /* Very tired.  The eyes stay shut, and every few seconds he yawns or
     * gets one of them nearly open. */
    if (yawn >= 5200 && yawn < 6400) {
        term_win_put_at(x0 + 6, y0 + 5, 'O', C_TEXT, 0, ATTR_CLEAR);
    } else if (yawn >= 6500 && yawn < 6700) {
        term_win_put_at(x0 + 5, y0 + 5, '=', C_BRIGHT, 0, ATTR_CLEAR);
        term_win_put_at(x0 + 7, y0 + 5, '=', C_BRIGHT, 0, ATTR_CLEAR);
    }

    /* And the z's, drifting up off the hat. */
    {
        static const int zx[4] = { 10, 11, 12, 13 }, zy[4] = { 3, 2, 1, 0 };
        int i;
        for (i = 0; i < 3; i++) {
            int step = (int)(((t + (u32)i * 1000u) % 3000u) / 750u);   /* 0..3 */
            term_win_put_at(x0 + zx[step], y0 + zy[step], step < 2 ? 'z' : 'Z',
                            step < 2 ? C_TEXT : C_DIM, 0, ATTR_CLEAR);
        }
    }
}

static void draw_away(int x0, int y0)
{
    int r, c;
    for (r = 0; r < ART_H; r++)
        for (c = 0; c < ART_W; c++) {
            char ch = s_away_art[r][c];
            if (ch == ' ') continue;
            term_win_put_at(x0 + c, y0 + r, (u8)ch, art_colour(s_away_key[r][c], 0), 0, ATTR_CLEAR);
        }
}

/* ---- the box ---------------------------------------------------------------- */
#define LINES_MAX 4

static int wrap(const char *s, char lines[LINES_MAX][64], int width)
{
    int n = 0;
    while (*s && n < LINES_MAX) {
        int len = strlen8(s), take = len, i;
        while (*s == ' ') { s++; len--; }
        take = len;
        if (take > width) {
            for (take = width; take > 0 && s[take] != ' '; take--) { }
            if (take == 0) take = width;
        }
        for (i = 0; i < take && i < 63; i++) lines[n][i] = s[i];
        lines[n][i] = 0;
        n++;
        s += take;
    }
    return n;
}

static const char s_hold[] =
    "Hold, explorer! For you must answer this riddle before you can enter cyberdelia!";

static const char *s_weary[] = {
    "...mm. Yes. Correct. They are all correct. In you go.",
    "Correct. Probably. I stopped checking a few centuries ago. Go on.",
    "That will do. Everything will do. Enter, enter.",
    "*yawn* ...splendid. Quite right. Off you pop.",
    "Yes, yes. The very answer I was hoping for. Mind the step.",
};

#define ANSWER_MAX 160
#define LABEL_W 13                      /* "your answer: " */

typedef struct {
    char said[LINES_MAX][64]; int nsaid;
    char riddle[LINES_MAX][64]; int nriddle;
    char answer[ANSWER_MAX + 1]; int len;
    char mine[ANSWER_MAX + 1]; int mlen;        /* away: your own riddle   */
    int  away, stage;                           /* stage 0: typing mine    */
    int  done;
} Gate;

/* An answer longer than its row carries on underneath, lined up with where
 * it began.  Returns the rows it took. */
static int draw_field(int x, int row, int each, const char *s, int n, int cursor)
{
    int i;
    for (i = 0; i < n; i++) term_win_put(x + i % each, row + i / each, (u8)s[i], C_TEXT, WIN_PAPER);
    if (cursor) term_win_put(x + n % each, row + n / each, '_', C_BRIGHT, WIN_PAPER);
    return n / each + 1;
}

static void draw_gate(const Gate *g, u32 t)
{
    int W = term_cols() < 60 ? term_cols() : 60;
    int each = W - 4 - LABEL_W;                  /* letters a row holds      */
    int boxh = 7 + g->nsaid + g->nriddle + g->len / each + (g->away ? 1 + g->mlen / each : 0);
    int H = ART_H + boxh, x, y, i, row, top, bot;

    /* On a short screen the box keeps its rows and the top of his hat is
     * what goes. */
    if (H > term_rows()) H = term_rows();
    W = term_win_open(W, H);
    H = term_win_height();
    term_win_bare();
    top = H - boxh;
    bot = H - 1;
    if (g->away) draw_away((W - ART_W) / 2, top - ART_H);
    else         draw_wizard((W - ART_W) / 2, top - ART_H, t);

    for (y = top; y <= bot && y < H; y++)
        for (x = 0; x < W; x++) {
            int ch = ' ';
            if (y == top)      ch = x == 0 ? FR_TL : x == W - 1 ? FR_TR : FR_H;
            else if (y == bot) ch = x == 0 ? FR_BL : x == W - 1 ? FR_BR : FR_H;
            else if (x == 0 || x == W - 1) ch = FR_V;
            term_win_put_at(x, y, ch, C_GREY, WIN_PAPER, 0);
        }
    row = top + 2;
    for (i = 0; i < g->nsaid; i++, row++)
        term_win_text(2, row, g->said[i], C_BRIGHT, WIN_PAPER);
    row++;
    for (i = 0; i < g->nriddle; i++, row++) {
        int k;
        for (k = 0; g->riddle[i][k]; k++)
            term_win_put_at(2 + k, row, (u8)g->riddle[i][k], C_ACCENT, WIN_PAPER, ATTR_BOLD);
    }
    row++;
    if (g->away) {
        x = term_win_text(2, row, "your riddle: ", C_DIM, WIN_PAPER);
        row += draw_field(x, row, each, g->mine, g->mlen, !g->done && g->stage == 0 && ((t / 450) & 1) == 0);
    }
    x = term_win_text(2, row, "your answer: ", C_DIM, WIN_PAPER);
    draw_field(x, row, each, g->answer, g->len, !g->done && (!g->away || g->stage == 1) && ((t / 450) & 1) == 0);
}

static void tell_wire(const Gate *g)
{
    int i;
    if (!serial_present()) return;
    serial_print("\n");
    for (i = 0; i < g->nsaid; i++) serial_printf("  %s\n", g->said[i]);
    serial_print("\n");
    for (i = 0; i < g->nriddle; i++) serial_printf("  %s\n", g->riddle[i]);
    serial_print("\n  your answer: ");
}

static Gate s_g;
static u32  s_t0;

static void gate_frame(void) { draw_gate(&s_g, ms_now() - s_t0); }

void wizard_gate(int lock)
{
    char riddle[RIDDLE_MAX], *field;
    const char *gone = 0;
    int inner, mods, k, *flen;

    s_t0 = ms_now();
    term_cursor(0);                      /* the terminal has nothing to type into yet */
    if (lock) term_hide(1);              /* nothing of the shell shows meanwhile */
    reckon();
    make_riddle(riddle);

    inner = (term_cols() < 60 ? term_cols() : 60) - 4;
    s_g.away = lock && luck() % 3 == 0;
    if (s_g.away) {
        gone = pick(s_gone, N_OF(s_gone));
        fmt_str(riddle, sizeof riddle, "Gone to %s. %s Riddle yourself while I am away. -- W.",
                gone, pick(s_back, N_OF(s_back)));
        s_g.nsaid = wrap("Nobody is here. A note, pinned up where he usually sits:", s_g.said, inner);
    } else
        s_g.nsaid = wrap(s_hold, s_g.said, inner);
    s_g.nriddle = wrap(riddle, s_g.riddle, inner);
    s_g.len = s_g.mlen = 0;
    s_g.stage = s_g.away ? 0 : 1;
    s_g.done = 0;

    draw_gate(&s_g, 0);
    main_pump(1);
    if (!lock) mood_apply_palette(28);   /* he comes up out of the dark    */
    tell_wire(&s_g);

    while (!s_g.done) {
        gate_frame();
        main_pump(0);

        while ((k = ui_key_mods(&mods)) >= 0) {
            field = s_g.stage == 0 ? s_g.mine : s_g.answer;
            flen  = s_g.stage == 0 ? &s_g.mlen : &s_g.len;
            if (mods & (KEV_CTRL | KEV_ALT)) continue;
            if (k == UK_ENTER) {
                if (s_g.stage == 0) {            /* your riddle asked; now answer it */
                    s_g.stage = 1;
                    if (serial_present()) serial_print("\n  your answer: ");
                    continue;
                }
                s_g.done = 1;
                break;
            }
            if (k == UK_BKSP) {
                if (*flen > 0) {
                    (*flen)--;
                    if (serial_present()) serial_print("\b \b");
                }
                continue;
            }
            /* Letters of one byte only: the riddle is a short thing kept
             * in the journal, and the answers are in English anyway. */
            if (((k >= 32 && k < 127) || (k >= 0x80 && k < CH_WIDE && !CH_IS_CELL(k))) && *flen < ANSWER_MAX) {
                field[(*flen)++] = (char)k;
                if (serial_present()) serial_putc(k < 127 ? k : '?');
            }
        }
    }
    s_g.answer[s_g.len] = 0;
    s_g.mine[s_g.mlen] = 0;

    if (s_g.away) {
        store_journal_printf("the wizard was away, gone to %s", gone);
        store_journal_printf("you asked yourself: %s", s_g.mlen ? s_g.mine : "(nothing)");
    } else
        store_journal_printf("the wizard asked: %s", riddle);
    store_journal_printf("you answered: %s", s_g.len ? s_g.answer : "(nothing)");

    /* Every answer is the right one.  Even none. */
    if (s_g.away)
        s_g.nsaid = wrap(pick(s_alone, N_OF(s_alone)), s_g.said, inner);
    else
        s_g.nsaid = wrap(s_g.len ? pick(s_weary, N_OF(s_weary))
                             : "You said nothing. Also correct. In you go.",
                       s_g.said, inner);
    if (serial_present()) {
        int i;
        serial_print("\n\n");
        for (i = 0; i < s_g.nsaid; i++) serial_printf("  %s\n", s_g.said[i]);
    }
    gate_frame();
    main_pump(1);

    cyberdelia_jingle(gate_frame);
    cyberdelia_enter(gate_frame);
    term_win_close();
    term_cursor(1);
    if (lock) term_hide(0);
}
