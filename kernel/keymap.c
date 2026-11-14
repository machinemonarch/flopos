/* ===========================================================================
 * keymap.c -- turning a key into a character, on whichever keyboard is
 * plugged in: Swedish (the default) and sixteen others, `keyboard` lists
 * them.  Bulgarian, Ukrainian, Hebrew and Yiddish keyboards have Latin
 * letters too: Alt and Shift pressed together switch between the two.
 *
 * The layouts themselves are tables made by tools/mkkeymap.py, in
 * keymaps.c.  What lives here is what the tables cannot say: the accent
 * keys that wait for the next letter (´ then e is é), Caps Lock, and the
 * keypad, which on this laptop doubles as the arrows.
 * ===========================================================================
 */
#include "flopos.h"
#include "keymaps.h"

enum { D_GRAVE = 1, D_ACUTE, D_CIRC, D_DIAER };

static int s_map;                        /* index into g_keymaps           */
static int s_dead;                       /* an accent waiting, or 0        */
static volatile int s_latin;             /* on its Latin side (Alt+Shift)  */

/* Alt and Shift together: from the keyboard's interrupt, so only a flag. */
void keymap_switch(void)
{
    if (g_keymaps[s_map].latin >= 0) { s_latin = !s_latin; s_dead = 0; }
}

int keymap_on_latin(void) { return g_keymaps[s_map].latin >= 0 && s_latin; }
int keymap_has_latin(void) { return g_keymaps[s_map].latin >= 0; }

/* The mark on its own, for an accent key followed by space. */
static int mark_alone(int d)
{
    switch (d) {
    case D_GRAVE: return '`';
    case D_ACUTE: return 0xB4;
    case D_CIRC:  return '^';
    default:      return CH_DIAER;
    }
}

/* A vowel with a mark, by its Latin-1 number; 0 if there is no such
 * letter.  Rows: grave, acute, circumflex, diaeresis. */
static int with_mark(int d, int c)
{
    static const char vowels[] = "aeiouAEIOU";
    static const u8 cp[4][10] = {
        { 0xE0, 0xE8, 0xEC, 0xF2, 0xF9, 0xC0, 0xC8, 0xCC, 0xD2, 0xD9 },
        { 0xE1, 0xE9, 0xED, 0xF3, 0xFA, 0xC1, 0xC9, 0xCD, 0xD3, 0xDA },
        { 0xE2, 0xEA, 0xEE, 0xF4, 0xFB, 0xC2, 0xCA, 0xCE, 0xD4, 0xDB },
        { 0xE4, 0xEB, 0xEF, 0xF6, 0xFC, 0xC4, 0xCB, 0xCF, 0xD6, 0xDC },
    };
    int i;
    if (d == D_DIAER && c == 'y') return ch_from_latin1(0xFF);
    for (i = 0; vowels[i]; i++)
        if (vowels[i] == c) return ch_from_latin1(cp[d - 1][i]);
    return 0;
}

/* Caps Lock turns letters over, and only letters: the Swedish three, and
 * the Latin-1 ones, whose capitals sit 0x20 below (but not ß or ÿ, which
 * have none, and not the multiplication and division signs among them). */
static int caps(int c)
{
    if (c >= 'a' && c <= 'z') return c - 32;
    if (c >= 'A' && c <= 'Z') return c + 32;
    if (c >= CH_ARING_L && c <= CH_ODIA_L) return c - (CH_ARING_L - CH_ARING);
    if (c >= CH_ARING && c <= CH_ODIA) return c + (CH_ARING_L - CH_ARING);
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return c - 0x20;
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 0x20;
    if (c >= 0x430 && c <= 0x44F) return c - 0x20;          /* Cyrillic    */
    if (c >= 0x410 && c <= 0x42F) return c + 0x20;
    if (c >= 0x450 && c <= 0x45F) return c - 0x50;
    if (c >= 0x400 && c <= 0x40F) return c + 0x50;
    if (c == 0x490 || c == 0x102 || c == 0x218 || c == 0x21A) return c + 1;   /* Ґ Ă Ș Ț */
    if (c == 0x491 || c == 0x103 || c == 0x219 || c == 0x21B) return c - 1;
    return c;
}

/* What the key types on this keyboard, before accents are thought about:
 * a character, an accent key (1..4), or 0. */
static int lookup(int event)
{
    int m = keymap_on_latin() ? g_keymaps[s_map].latin : s_map;
    const u16 (*k)[KEYMAP_SCANS] = g_keymaps[m].keys;
    int sc = KEV_SCAN(event);
    int shift = (event & KEV_SHIFT) != 0;
    int c;

    if (sc >= KEYMAP_SCANS) return 0;

    /* The numeric keypad doubles as the arrow cluster on this laptop.
     * Without an E0 prefix those scancodes mean navigation, so they must
     * not also produce digits. */
    if (!(event & KEV_EXT) && sc >= 0x47 && sc <= 0x53) {
        if (sc == 0x4A || sc == 0x4E) return k[0][sc];      /* - and +     */
        return 0;
    }

    if (event & KEV_ALTGR) {
        c = shift ? k[3][sc] : 0;
        return c ? c : k[2][sc];         /* AltGr-nothing is nothing       */
    }

    c = k[shift][sc];
    if (c > D_DIAER && (event & KEV_CAPS)) {
        /* Caps Lock inverts whatever Shift did, for letters.  French and
         * Italian keep their accented letters on the number row and beside
         * P, where Shift gives a digit or a sign: those stay as they are. */
        int other = k[!shift][sc];
        if (caps(c) != c && (other == caps(c) || other == 0 || (c >= 'A' && c <= 'z') ||
                             (c >= CH_ARING && c <= CH_ODIA_L)))
            c = caps(c);
    }
    return c;
}

/* For Ctrl and its shortcuts.  On a keyboard with a second alphabet the
 * letter is taken from its Latin side, so Ctrl-S saves in Hebrew too. */
int keymap_key(int event)
{
    int c = lookup(event);
    if (g_keymaps[s_map].latin >= 0 && !keymap_on_latin() && !(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z')) {
        int was = s_latin;
        s_latin = 1;
        c = lookup(event);
        s_latin = was;
    }
    return c > D_DIAER ? c : 0;
}

int keymap_char(int event)
{
    int c = lookup(event), d = s_dead;

    if (!c) return 0;
    if (c <= D_DIAER) {                  /* an accent key                  */
        s_dead = c;
        if (d) { s_dead = (d == c) ? 0 : c; return mark_alone(d); }
        return 0;
    }
    if (!d) return c;
    s_dead = 0;
    if (c == ' ') return mark_alone(d);
    {
        int m = with_mark(d, c);
        return m ? m : c;                /* no such letter: just the key   */
    }
}

/* ---- choosing a keyboard --------------------------------------------- */
static int same(const char *a, const char *b)       /* either case        */
{
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
    }
    return *a == *b;
}

int keymap_set(const char *code)
{
    int i;
    for (i = 0; i < KEYMAP_COUNT; i++)
        if (same(code, g_keymaps[i].code)) { s_map = i; s_dead = 0; s_latin = 0; return 1; }
    if (same(code, "gb")) return keymap_set("uk");
    if (same(code, "he")) return keymap_set("il");
    return 0;
}

const char *keymap_code(void) { return g_keymaps[s_map].code; }
int keymap_count(void) { return KEYMAP_COUNT; }
const char *keymap_code_at(int i) { return g_keymaps[i].code; }
const char *keymap_name_at(int i) { return g_keymaps[i].name; }
