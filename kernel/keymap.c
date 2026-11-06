/* ===========================================================================
 * keymap.c -- Swedish (SE) keyboard layout.
 *
 * The letters sit in the same physical places as US QWERTY -- Y and Z are
 * not swapped, that is the German board -- so nothing positional needs
 * remapping.  The symbols are another matter entirely, and a shell needs
 * them: on this keyboard " is Shift-2, / is Shift-7, @ is AltGr-2, \ is
 * AltGr-plus, and - sits where a US board keeps /.
 *
 * Three tables, indexed by scancode: unshifted, shifted, AltGr.  A zero
 * means the key produces no character, either because it is a modifier or
 * because the glyph is not in our font.  The two dead keys (acute and
 * diaeresis) insert nothing in this version rather than silently lying
 * about what they composed.
 * ===========================================================================
 */
#include "flopos.h"

#define _ 0
#define AA CH_ARING_L                    /* unshifted: lowercase            */
#define AE CH_ADIA_L
#define OE CH_ODIA_L
#define AAU CH_ARING                     /* shifted: capital                */
#define AEU CH_ADIA
#define OEU CH_ODIA

/* Scancode set 1, 0x00..0x58. */
static const u8 s_plain[0x59] = {
/* 00 */ _,   _,  '1', '2', '3', '4', '5', '6',
/* 08 */ '7', '8','9', '0', '+', _,   _,   _,
/* 10 */ 'q', 'w','e', 'r', 't', 'y', 'u', 'i',
/* 18 */ 'o', 'p',AA,  _,   _,   _,   'a', 's',
/* 20 */ 'd', 'f','g', 'h', 'j', 'k', 'l', OE,
/* 28 */ AE,  _,  _,   '\'','z', 'x', 'c', 'v',
/* 30 */ 'b', 'n','m', ',', '.', '-', _,   '*',
/* 38 */ _,   ' ',_,   _,   _,   _,   _,   _,
/* 40 */ _,   _,  _,   _,   _,   _,   _,   _,
/* 48 */ _,   _,  '-', _,   _,   _,   '+', _,
/* 50 */ _,   _,  _,   _,   _,   _,   '<', _,
};

static const u8 s_shift[0x59] = {
/* 00 */ _,   _,  '!', '"', '#', _,   '%', '&',
/* 08 */ '/', '(',')', '=', '?', _,   _,   _,
/* 10 */ 'Q', 'W','E', 'R', 'T', 'Y', 'U', 'I',
/* 18 */ 'O', 'P',AAU, '^', _,   _,   'A', 'S',
/* 20 */ 'D', 'F','G', 'H', 'J', 'K', 'L', OEU,
/* 28 */ AEU, _,  _,   '*', 'Z', 'X', 'C', 'V',
/* 30 */ 'B', 'N','M', ';', ':', '_', _,   '*',
/* 38 */ _,   ' ',_,   _,   _,   _,   _,   _,
/* 40 */ _,   _,  _,   _,   _,   _,   _,   _,
/* 48 */ _,   _,  '-', _,   _,   _,   '+', _,
/* 50 */ _,   _,  _,   _,   _,   _,   '>', _,
};

static const u8 s_altgr[0x59] = {
/* 00 */ _,   _,  _,   '@', _,   '$', _,   _,
/* 08 */ '{', '[',']', '}', '\\',_,   _,   _,
/* 10 */ _,   _,  _,   _,   _,   _,   _,   _,
/* 18 */ _,   _,  _,   '~', _,   _,   _,   _,
/* 20 */ _,   _,  _,   _,   _,   _,   _,   _,
/* 28 */ _,   _,  _,   _,   _,   _,   _,   _,
/* 30 */ _,   _,  'm', _,   _,   _,   _,   _,
/* 38 */ _,   ' ',_,   _,   _,   _,   _,   _,
/* 40 */ _,   _,  _,   _,   _,   _,   _,   _,
/* 48 */ _,   _,  _,   _,   _,   _,   _,   _,
/* 50 */ _,   _,  _,   _,   _,   _,   '|', _,
};

#undef _
#undef AA
#undef AE
#undef OE
#undef AAU
#undef AEU
#undef OEU

int keymap_char(int event)
{
    int sc = KEV_SCAN(event);
    int c;

    if (sc >= 0x59) return 0;

    /* The numeric keypad doubles as the arrow cluster on this laptop.
     * Without an E0 prefix those scancodes mean navigation, so they must
     * not also produce digits. */
    if (!(event & KEV_EXT) && sc >= 0x47 && sc <= 0x53) {
        if (sc == 0x4A || sc == 0x4E) return s_plain[sc];   /* - and +     */
        return 0;
    }

    if (event & KEV_ALTGR) {
        c = s_altgr[sc];
        if (c) return c;
        return 0;                        /* AltGr-nothing is nothing       */
    }

    c = (event & KEV_SHIFT) ? s_shift[sc] : s_plain[sc];
    if (!c) return 0;

    /* Caps Lock affects letters only, and inverts whatever Shift did.
     * The Swedish three count as letters, which is easy to forget. */
    if (event & KEV_CAPS) {
        if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
        else if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
        else if (c >= CH_ARING_L && c <= CH_ODIA_L) c -= (CH_ARING_L - CH_ARING);
        else if (c >= CH_ARING && c <= CH_ODIA) c += (CH_ARING_L - CH_ARING);
    }
    return c;
}
