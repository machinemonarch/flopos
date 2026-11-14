/* keytest.c -- the keyboard layouts (kernel/keymap.c and the tables
 * tools/mkkeymap.py makes), built for this PC and fed scancodes as the
 * laptop's keyboard would send them.  Prints ok, or FAILED and what came
 * out wrong.  Run by tools/keytest/run.sh.
 */
#include <stdio.h>
#include "../../kernel/flopos.h"

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

    /* Ctrl shortcuts go by the letter: ^A on a French board is the A key,
     * and an accent key with Ctrl starts nothing. */
    keymap_set("fr");
    if (keymap_key(0x10) != 'a' || keymap_key(0x1E) != 'q') { puts("FAILED  fr: Ctrl-A is not where A is"); s_bad++; }
    keymap_key(0x1A);
    if (keymap_char(0x12) != 'e') { puts("FAILED  fr: a Ctrl-press of an accent key was kept"); s_bad++; }

    /* Every character any layout can type, the font can draw. */
    {
        int m, lv, sc;
        extern const struct { const char *c; const char *n; const u8 (*k)[0x59]; } g_keymaps[];
        for (m = 0; m < keymap_count(); m++)
            for (lv = 0; lv < 4; lv++)
                for (sc = 0; sc < 0x59; sc++) {
                    int c = g_keymaps[m].k[lv][sc];
                    if (c > 4 && glyph_index(c) == '?' - 32 && c != '?') {
                        printf("FAILED  %s: scancode 0x%02X, level %d types 0x%02X, which the font lacks\n",
                               keymap_code_at(m), sc, lv, c);
                        s_bad++;
                    }
                }
    }

    if (!keymap_set("xx") && keymap_set("DE") && keymap_set("se")) ; else { puts("FAILED  keymap_set"); s_bad++; }
    puts(s_bad ? "FAILED" : "ok");
    return s_bad != 0;
}
