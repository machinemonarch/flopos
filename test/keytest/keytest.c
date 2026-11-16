/* keytest.c -- the keyboard layouts (src/kernel/keymap.c and the tables
 * tools/build/mkkeymap.py makes), built for this PC and fed scancodes as the
 * laptop's keyboard would send them.  Prints ok, or FAILED and what came
 * out wrong.  Run by test/keytest/run.sh.
 */
#include <stdio.h>
#include "../../src/kernel/flopos.h"
#include "../../src/kernel/gen/keymaps.h"

#define S  KEV_SHIFT
#define G  KEV_ALTGR
#define C  KEV_CAPS

static int s_bad;

/* Keys typed one after another must give these characters (0 = nothing). */
static void want(const char *map, const char *what, const int *ev, int n, const int *out)
{
    int i, got[16];
    keymap_set(map);
    for (i = 0; i < n; i++) got[i] = keymap_char(ev[i]);
    for (i = 0; i < n; i++)
        if (got[i] != out[i]) {
            printf("FAILED  %s %s: key %d gave 0x%02X, wanted 0x%02X\n", map, what, i + 1, got[i], out[i]);
            s_bad++;
            return;
        }
}
#define WANT(map, what, evs, outs) do { static const int e[] = evs, o[] = outs; \
    want(map, what, e, (int)(sizeof e / sizeof *e), o); } while (0)
#define L(...) { __VA_ARGS__ }

int main(void)
{
    /* Swedish, as it always was. */
    WANT("se", "quote, slash, at, backslash", L(S|0x03, S|0x08, G|0x03, G|0x0C), L('"', '/', '@', '\\'));
    WANT("se", "the three letters", L(0x1A, 0x28, 0x27, S|0x1A), L(CH_ARING_L, CH_ADIA_L, CH_ODIA_L, CH_ARING));
    WANT("se", "caps lock on a-ring", L(C|0x1A, C|S|0x1A), L(CH_ARING, CH_ARING_L));
    WANT("se", "acute then e", L(0x0D, 0x12), L(0, 0xE9));
    WANT("se", "diaeresis then u", L(0x1B, 0x16), L(0, 0xFC));
    WANT("se", "acute then space", L(0x0D, 0x39), L(0, 0xB4));
    WANT("se", "acute twice", L(0x0D, 0x0D), L(0, 0xB4));
    WANT("se", "acute then x", L(0x0D, 0x2D), L(0, 'x'));
    WANT("se", "euro, pound, tilde, pipe", L(G|0x12, G|0x04, G|0x1B, G|0x56), L(CH_EURO, CH_POUND, '~', '|'));
    WANT("fi", "the same as Swedish", L(0x27, S|0x08), L(CH_ODIA_L, '/'));

    /* Norwegian and Danish: the same rows, the two letters swapped. */
    WANT("no", "o-slash, ae, a-ring", L(0x27, 0x28, 0x1A, S|0x27), L(0xF8, 0xE6, CH_ARING_L, 0xD8));
    WANT("dk", "ae, o-slash", L(0x27, 0x28, S|0x27), L(0xE6, 0xF8, 0xC6));
    WANT("no", "pipe, backslash, at", L(0x29, 0x0D, G|0x03), L('|', '\\', '@'));
    WANT("dk", "pipe, backslash", L(G|0x0D, G|0x56), L('|', '\\'));
    WANT("no", "acute on AltGr, then e", L(G|0x0D, 0x12), L(0, 0xE9));
    WANT("dk", "caps lock on ae", L(C|0x27), L(0xC6));

    /* American and British. */
    WANT("us", "signs", L(S|0x03, 0x35, 0x2B, S|0x29, 0x1A), L('@', '/', '\\', '~', '['));
    WANT("us", "no accent keys", L(0x28, 0x12), L('\'', 'e'));
    WANT("uk", "pound, at, hash", L(S|0x04, S|0x28, 0x2B, S|0x03), L(CH_POUND, '@', '#', '"'));
    WANT("gb", "the same as uk", L(S|0x04), L(CH_POUND));

    /* German: Y and Z swapped, accents waiting. */
    WANT("de", "z and y", L(0x15, 0x2C), L('z', 'y'));
    WANT("de", "umlauts, sharp s", L(0x1A, 0x27, 0x28, 0x0C, S|0x1A), L(0xFC, CH_ODIA_L, CH_ADIA_L, 0xDF, 0xDC));
    WANT("de", "caps lock on u-umlaut", L(C|0x1A), L(0xDC));
    WANT("de", "slash, at, braces", L(S|0x08, G|0x10, G|0x08, G|0x0B, G|0x0C), L('/', '@', '{', '}', '\\'));
    WANT("de", "circumflex then o", L(0x29, 0x18), L(0, 0xF4));
    WANT("de", "circumflex then space", L(0x29, 0x39), L(0, '^'));
    WANT("de", "grave then a", L(S|0x0D, 0x1E), L(0, 0xE0));
    WANT("de", "acute then E", L(0x0D, S|0x12), L(0, 0xC9));
    WANT("de", "euro, micro, degree", L(G|0x12, G|0x32, S|0x29), L(CH_EURO, 0xB5, CH_DEGREE));

    /* French: AZERTY, digits under Shift. */
    WANT("fr", "a z q w m", L(0x10, 0x11, 0x1E, 0x2C, 0x27), L('a', 'z', 'q', 'w', 'm'));
    WANT("fr", "number row", L(0x02, 0x03, 0x08, 0x0A, 0x0B, S|0x03), L('&', 0xE9, 0xE8, 0xE7, 0xE0, '2'));
    WANT("fr", "caps lock leaves the number row", L(C|0x03, C|0x10), L(0xE9, 'A'));
    WANT("fr", "circumflex then e", L(0x1A, 0x12), L(0, 0xEA));
    WANT("fr", "diaeresis then i", L(S|0x1A, 0x17), L(0, 0xEF));
    WANT("fr", "at, tilde, backslash, braces", L(G|0x0B, G|0x03, G|0x09, G|0x05, G|0x0D),
         L('@', '~', '\\', '{', '}'));
    WANT("fr", "live circumflex on AltGr-9", L(G|0x0A, 0x12), L('^', 'e'));
    WANT("fr", "u-grave, colon, slash, full stop", L(0x28, 0x34, S|0x34, S|0x33), L(0xF9, ':', '/', '.'));

    /* Belgian: AZERTY, but not the French one. */
    WANT("be", "number row", L(0x02, 0x07, 0x09, 0x0D, S|0x0D), L('&', CH_SECTION, '!', '-', '_'));
    WANT("be", "at, pipe, braces", L(G|0x03, G|0x02, G|0x0A, G|0x0B), L('@', '|', '{', '}'));
    WANT("be", "micro, pound, equals, backslash", L(0x2B, S|0x2B, 0x35, G|0x56), L(0xB5, CH_POUND, '=', '\\'));
    WANT("be", "acute on AltGr, then e; grave, then a", L(G|0x28, 0x12, G|0x2B, 0x10), L(0, 0xE9, 0, 0xE0));
    WANT("be", "circumflex then o", L(0x1A, 0x18), L(0, 0xF4));

    /* Spanish. */
    WANT("es", "n-tilde, c-cedilla", L(0x27, S|0x27, 0x2B), L(0xF1, 0xD1, 0xE7));
    WANT("es", "acute then o", L(0x28, 0x18), L(0, 0xF3));
    WANT("es", "diaeresis then u", L(S|0x28, 0x16), L(0, 0xFC));
    WANT("es", "grave then a, circumflex then e", L(0x1A, 0x1E, S|0x1A, 0x12), L(0, 0xE0, 0, 0xEA));
    WANT("es", "inverted marks", L(0x0D, S|0x0D), L(CH_IEXCL, 0xBF));
    WANT("es", "at, hash, tilde, brackets", L(G|0x03, G|0x04, G|0x05, G|0x1A, G|0x2B), L('@', '#', '~', '[', '}'));

    /* Italian. */
    WANT("it", "accented letters", L(0x1A, S|0x1A, 0x27, 0x28, 0x2B, 0x0D), L(0xE8, 0xE9, 0xF2, 0xE0, 0xF9, 0xEC));
    WANT("it", "brackets and braces", L(G|0x1A, G|0x1B, G|S|0x1A, G|S|0x1B), L('[', ']', '{', '}'));
    WANT("it", "at, hash, backquote, tilde", L(G|0x27, G|0x28, G|0x0C, G|0x0D), L('@', '#', '`', '~'));

    /* Romanian: its letters where the standard puts them. */
    WANT("ro", "a-breve, i-circumflex, a-circumflex", L(0x1A, 0x1B, 0x2B, S|0x1A), L(0x103, 0xEE, 0xE2, 0x102));
    WANT("ro", "s-comma, t-comma", L(0x27, 0x28, S|0x27, C|0x28), L(0x219, 0x21B, 0x218, 0x21A));
    WANT("ro", "brackets on AltGr, euro", L(G|0x1A, G|0x1B, G|S|0x1A, G|0x12), L('[', ']', '{', CH_EURO));
    WANT("nl", "the American keys", L(S|0x03, 0x2B), L('@', '\\'));

    /* Bulgarian (BDS): Cyrillic, and Latin after Alt+Shift. */
    WANT("bg", "the home row", L(0x1E, 0x1F, 0x20, 0x21, 0x26), L(0x44C, 0x44F, 0x430, 0x43E, 0x432));
    WANT("bg", "capitals, Caps Lock", L(S|0x20, C|0x10, C|0x31), L(0x410, ',', 0x425));
    WANT("bg", "the rest", L(0x56, 0x2D, 0x2F, 0x35, S|0x10, G|0x1E), L(0x45D, 0x439, 0x44D, 0x431, 0x44B, 0x44B));
    keymap_set("bg"); keymap_switch();
    if (keymap_char(0x10) != 'q' || keymap_char(S|0x03) != '@') { puts("FAILED  bg: Alt+Shift did not give Latin"); s_bad++; }
    keymap_switch();
    if (keymap_char(0x10) != ',') { puts("FAILED  bg: Alt+Shift did not come back"); s_bad++; }
    keymap_set("bg");
    if (keymap_on_latin()) { puts("FAILED  bg: keyboard did not start on Cyrillic"); s_bad++; }
    keymap_set("se"); keymap_switch();
    if (keymap_on_latin() || keymap_char(0x1A) != CH_ARING_L) { puts("FAILED  se: Alt+Shift changed a Latin keyboard"); s_bad++; }

    keymap_set("il");
    if (keymap_key(0x1F) != 's' || keymap_key(0x10) != 'q') { puts("FAILED  il: Ctrl-S is not where S is"); s_bad++; }

    /* Ukrainian. */
    WANT("ua", "letters", L(0x10, 0x1B, 0x1F, 0x28, 0x2B, 0x56), L(0x439, 0x457, 0x456, 0x454, 0x491, '/'));
    WANT("ua", "capitals", L(S|0x1B, S|0x1F, S|0x2B), L(0x407, 0x406, 0x490));
    WANT("ua", "signs", L(S|0x05, S|0x07, S|0x08, 0x35, S|0x35), L(';', ':', '?', '.', ','));

    /* Hebrew, and Yiddish on top of it. */
    WANT("il", "letters", L(0x14, 0x1E, 0x2E, 0x18, 0x26), L(0x5D0, 0x5E9, 0x5D1, 0x5DD, 0x5DA));
    WANT("il", "Shift gives Latin capitals", L(S|0x10, S|0x1E), L('Q', 'A'));
    WANT("he", "the same as il", L(0x14), L(0x5D0));
    WANT("il", "Yiddish letters on AltGr", L(G|0x15, G|0x22, G|0x23), L(0x5F0, 0x5F1, 0x5F2));
    WANT("yi", "pointed letters", L(G|0x14, G|S|0x14, G|0x19, G|S|0x19, G|0x2E, G|0x33),
         L(0xFB2E, 0xFB2F, 0xFB44, 0xFB4E, 0xFB4C, 0xFB4A));
    WANT("yi", "more pointed letters", L(G|0x16, G|0x1E, G|0x21, G|0x24, G|S|0x23), L(0xFB35, 0xFB2B, 0xFB3B, 0xFB1D, 0xFB1F));

    /* Ctrl shortcuts go by the letter: ^A on a French board is the A key,
     * and an accent key with Ctrl starts nothing. */
    keymap_set("fr");
    if (keymap_key(0x10) != 'a' || keymap_key(0x1E) != 'q') { puts("FAILED  fr: Ctrl-A is not where A is"); s_bad++; }
    keymap_key(0x1A);
    if (keymap_char(0x12) != 'e') { puts("FAILED  fr: a Ctrl-press of an accent key was kept"); s_bad++; }

    /* Every character any layout can type, the font can draw. */
    {
        int m, lv, sc;
        for (m = 0; m < keymap_count(); m++)
            for (lv = 0; lv < 4; lv++)
                for (sc = 0; sc < 0x59; sc++) {
                    int c = g_keymaps[m].keys[lv][sc];
                    if (c > 4 && glyph_index(c) == '?' - 32 && c != '?') {
                        printf("FAILED  %s: scancode 0x%02X, level %d types 0x%02X, which the font lacks\n",
                               keymap_code_at(m), sc, lv, c);
                        s_bad++;
                    }
                }
    }

    /* Right to left: the order a row is drawn in. */
    {
        static const struct { const char *in, *out; } rtl[] = {
            { "abc", "abc" },
            { "ABC", "CBA" },                   /* capitals stand in for Hebrew */
            { "ABC def", "CBA def" },
            { "xy ABC DE z", "xy ED CBA z" },
            { "AB 2024 CD", "DC 2024 BA" },
            { "AB (CD) ok", "(DC) BA ok" },
            { "AB 3.14 C!", "C 3.14 BA!" },
            { "AB CD 2024", "2024 DC BA" },
            { "AB 12 ok", "12 BA ok" },
        };
        int t, i;
        for (t = 0; t < (int)(sizeof rtl / sizeof *rtl); t++) {
            u16 ch[32], order[32];
            char got[32];
            int n = 0;
            for (n = 0; rtl[t].in[n]; n++) {
                int c = rtl[t].in[n];
                ch[n] = (u16)(c >= 'A' && c <= 'Z' ? 0x5D0 + c - 'A' : c);
            }
            if (!bidi_order(ch, n, order)) for (i = 0; i < n; i++) order[i] = (u16)i;
            for (i = 0; i < n; i++) {
                int c = ch[order[i] & ~BIDI_MIRROR];
                if (order[i] & BIDI_MIRROR) c = bidi_mirror(c);
                got[i] = (char)(c >= 0x5D0 && c < 0x5D0 + 26 ? 'A' + c - 0x5D0 : c);
            }
            got[n] = 0;
            for (i = 0; i <= n; i++) if (got[i] != rtl[t].out[i]) break;
            if (i <= n) { printf("FAILED  right to left: \"%s\" drawn as \"%s\", wanted \"%s\"\n", rtl[t].in, got, rtl[t].out); s_bad++; }
        }
    }

    if (!keymap_set("xx") && keymap_set("DE") && keymap_set("se")) ; else { puts("FAILED  keymap_set"); s_bad++; }
    puts(s_bad ? "FAILED" : "ok");
    return s_bad != 0;
}
