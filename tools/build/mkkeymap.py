#!/usr/bin/env python3
"""Generate src/kernel/gen/keymaps.c from the keyboard layouts written out below.

Each layout is the four rows of keys a typist sees, one character per key,
for each of the levels: plain, with Shift, with AltGr (right Alt), and with
AltGr and Shift together (only Italian needs that one; elsewhere AltGr
ignores Shift).  The rows are, by the scancodes of set 1:

    row 1   the key left of 1, then 1 2 3 ... 0 and the two after   13 keys
    row 2   Q to the two keys right of P                            12 keys
    row 3   A to the key left of Enter (US: backslash)              12 keys
    row 4   the key right of left Shift (US boards lack it), Z to / 11 keys

A '·' is a key that types nothing at that level: a sign the font cannot
draw, like the French ² or the Spanish ordinals, or a level a board does
not use.

Accent keys that wait for the next letter ("dead keys") are written as:

    ˋ  grave        ˊ  acute        ˆ  circumflex        ‥  diaeresis

Pressing one and then a vowel gives the vowel with the mark; pressing it
and then space (or itself again) gives the mark alone.

A keyboard whose letters are not Latin (Bulgarian, Ukrainian, Hebrew,
Yiddish) names a second layout for when Latin is wanted: Alt and Shift
pressed together switch between the two.  A layout written as another's
name (fi is se) has the same keys under its own name.

The Bulgarian, Ukrainian, Romanian and Hebrew rows follow the standard
layouts as Linux's keyboard files have them (xkb: bg bds, ua, ro std, il),
less the signs the font cannot draw.  Yiddish is the Hebrew keyboard with
the pointed letters Yiddish spells with on AltGr; that arrangement is
FLOPOS's own.
"""
import sys

ROWS = [
    [0x29] + list(range(0x02, 0x0E)),
    list(range(0x10, 0x1C)),
    list(range(0x1E, 0x29)) + [0x2B],
    [0x56] + list(range(0x2C, 0x36)),
]
NONE = "·"
DEAD = {"ˋ": 1, "ˊ": 2, "ˆ": 3, "‥": 4}

# Character codes, as src/kernel/flopos.h defines them.
SPECIAL = {"Å": 0x80, "Ä": 0x81, "Ö": 0x82, "å": 0x86, "ä": 0x87, "ö": 0x88,
           "¡": 0x8A, "£": 0x8B, "€": 0x8C, "§": 0x8D, "¨": 0x8E, "°": 0x8F}

def code(ch):
    if ch == NONE:
        return 0
    if ch in DEAD:
        return DEAD[ch]
    if ch in SPECIAL:
        return SPECIAL[ch]
    n = ord(ch)
    if 32 <= n < 127 or 0xB4 <= n <= 0xFF or 0x100 <= n <= 0xFFFF:
        return n
    sys.exit("no character code for %r" % ch)

# name, what it is called, and the levels.  Each level is four rows.
LAYOUTS = [
  ("se", "Swedish", {
    "plain": ["§1234567890+ˊ", "qwertyuiopå‥", "asdfghjklöä'", "<zxcvbnm,.-"],
    "shift": ["·!\"#·%&/()=?ˋ", "QWERTYUIOPÅ^", "ASDFGHJKLÖÄ*", ">ZXCVBNM;:_"],
    "altgr": ["··@£$€·{[]}\\·", "··€········~", "············", "|······µ···"],
  }),
  ("fi", "Finnish", "se"),                    # the same keys as Swedish
  ("no", "Norwegian", {
    "plain": ["|1234567890+\\", "qwertyuiopå‥", "asdfghjkløæ'", "<zxcvbnm,.-"],
    "shift": ["§!\"#·%&/()=?ˋ", "QWERTYUIOPÅˆ", "ASDFGHJKLØÆ*", ">ZXCVBNM;:_"],
    "altgr": ["··@£$€·{[]}·ˊ", "··€········~", "············", "|······µ···"],
  }),
  ("dk", "Danish", {
    "plain": ["·1234567890+ˊ", "qwertyuiopå‥", "asdfghjklæø'", "<zxcvbnm,.-"],
    "shift": ["§!\"#·%&/()=?ˋ", "QWERTYUIOPÅˆ", "ASDFGHJKLÆØ*", ">ZXCVBNM;:_"],
    "altgr": ["··@£$€·{[]}·|", "··€········~", "············", "\\······µ···"],
  }),
  ("us", "American", {
    "plain": ["`1234567890-=", "qwertyuiop[]", "asdfghjkl;'\\", "\\zxcvbnm,./"],
    "shift": ["~!@#$%^&*()_+", "QWERTYUIOP{}", "ASDFGHJKL:\"|", "|ZXCVBNM<>?"],
  }),
  ("uk", "British", {
    "plain": ["`1234567890-=", "qwertyuiop[]", "asdfghjkl;'#", "\\zxcvbnm,./"],
    "shift": ["·!\"£$%^&*()_+", "QWERTYUIOP{}", "ASDFGHJKL:@~", "|ZXCVBNM<>?"],
    "altgr": ["····€········", "··é···úíó···", "á···········", "···········"],
  }),
  ("de", "German", {
    "plain": ["ˆ1234567890ßˊ", "qwertzuiopü+", "asdfghjklöä#", "<yxcvbnm,.-"],
    "shift": ["°!\"§$%&/()=?ˋ", "QWERTZUIOPÜ*", "ASDFGHJKLÖÄ'", ">YXCVBNM;:_"],
    "altgr": ["·······{[]}\\·", "@·€········~", "············", "|······µ···"],
  }),
  ("fr", "French", {
    "plain": ["·&é\"'(-è_çà)=", "azertyuiopˆ$", "qsdfghjklmù*", "<wxcvbn,;:!"],
    "shift": ["·1234567890°+", "AZERTYUIOP‥£", "QSDFGHJKLM%µ", ">WXCVBN?./§"],
    "altgr": ["··~#{[|`\\^@]}", "··€·········", "············", "···········"],
  }),
  ("be", "Belgian", {
    "plain": ["·&é\"'(§è!çà)-", "azertyuiopˆ$", "qsdfghjklmùµ", "<wxcvbn,;:="],
    "shift": ["·1234567890°_", "AZERTYUIOP‥*", "QSDFGHJKLM%£", ">WXCVBN?./+"],
    "altgr": ["·|@#··^··{}··", "··€·······[]", "··········ˊˋ", "\\·········~"],
  }),
  ("es", "Spanish", {
    "plain": ["·1234567890'¡", "qwertyuiopˋ+", "asdfghjklñˊç", "<zxcvbnm,.-"],
    "shift": ["·!\"·$%&/()=?¿", "QWERTYUIOPˆ*", "ASDFGHJKLÑ‥Ç", ">ZXCVBNM;:_"],
    "altgr": ["\\|@#~€·······", "··€·······[]", "··········{}", "···········"],
  }),
  ("it", "Italian", {
    "plain": ["\\1234567890'ì", "qwertyuiopè+", "asdfghjklòàù", "<zxcvbnm,.-"],
    "shift": ["|!\"£$%&/()=?^", "QWERTYUIOPé*", "ASDFGHJKLç°§", ">ZXCVBNM;:_"],
    "altgr": ["···········`~", "··€·······[]", "·········@#·", "···········"],
    "altgr_shift": ["·············", "··········{}", "············", "···········"],
  }),
]

def put(row, at, ch):
    """A row with the key at `at` changed to ch (for the pointed letters,
    which do not survive being typed into a string)."""
    r = list(row)
    r[at] = ch
    return r

HE = {
    "plain": [";1234567890-=", "/'קראטוןםפ][", "שדגכעיחלךף,\\", "\\זסבהנמצתץ."],
    "shift": ["~!@#$%^&*)(_+", "QWERTYUIOP}{", "ASDFGHJKL:\"|", "|ZXCVBNM><?"],
    "altgr": ["···€·°·······", "·····װ······", "····ױײ······", "···········"],
}
YI = {
    "plain": HE["plain"], "shift": HE["shift"],
    "altgr": [HE["altgr"][0],
              put(put(put("·····װ······", 4, "\uFB2E"), 6, "\uFB35"), 9, "\uFB44"),
              put(put(put("····ױײ······", 0, "\uFB2B"), 3, "\uFB3B"), 6, "\uFB1D"),
              put(put("···········", 3, "\uFB4C"), 8, "\uFB4A")],
    "altgr_shift": ["·············",
                    put(put("············", 4, "\uFB2F"), 9, "\uFB4E"),
                    put("············", 5, "\uFB1F"),
                    "···········"],
}
LAYOUTS += [
  ("nl", "Dutch", "us"),                   # the American keys, as the Dutch use
  ("ro", "Romanian", {
    "plain": ["·1234567890-=", "qwertyuiopăî", "asdfghjklșțâ", "\\zxcvbnm,./"],
    "shift": ["·!@#$%^&*()_+", "QWERTYUIOPĂÎ", "ASDFGHJKLȘȚÂ", "|ZXCVBNM;:?"],
    "altgr": ["`············", "··€······§[]", "·ß·······;'\\", "········<>·"],
    "altgr_shift": ["~············", "··········{}", "·········:\"|", "···········"],
  }),
  ("bg", "Bulgarian", {
    "plain": ["(1234567890-.", ",уеишщксдзц;", "ьяаожгтнвмч·", "ѝюйъэфхпрлб"],
    "shift": [")!?+\"%=:/··$€", "ыУЕИШЩКСДЗЦ§", "ѝЯАОЖГТНВМЧ·", "ЍЮЙЪЭФХПРЛБ"],
    "altgr": ["[············", "··эѝ········", "ы···········", "···········"],
    "altgr_shift": ["]············", "··ЭЍ········", "Ы···········", "···········"],
    "latin": "us",
  }),
  ("ua", "Ukrainian", {
    "plain": ["'1234567890-=", "йцукенгшщзхї", "фівапролджєґ", "/ячсмитьбю."],
    "shift": ["~!\"·;%:?*()_+", "ЙЦУКЕНГШЩЗХЇ", "ФІВАПРОЛДЖЄҐ", "|ЯЧСМИТЬБЮ,"],
    "altgr": ["···§$°<>·[]··", "···········ъ", "·ы········э\\", "··········/"],
    "altgr_shift": ["····€····{}··", "···········Ъ", "·Ы········Э|", "···········"],
    "latin": "us",
  }),
  ("il", "Hebrew", dict(HE, latin="us")),
  ("yi", "Yiddish", dict(YI, latin="us")),
]

LEVELS = ["plain", "shift", "altgr", "altgr_shift"]

def table(levels):
    out = []
    for lv in LEVELS:
        t = [0] * 0x59
        rows = levels.get(lv)
        if rows:
            assert len(rows) == 4, lv
            for r, (scs, keys) in enumerate(zip(ROWS, rows)):
                keys = list(keys)
                assert len(keys) == len(scs), (lv, r + 1, keys, len(keys), len(scs))
                for sc, ch in zip(scs, keys):
                    t[sc] = code(ch)
        # Fixed on every board: space, and the keypad's * - +.
        t[0x39] = 0x20
        if lv in ("plain", "shift"):
            t[0x37] = ord("*"); t[0x4A] = ord("-"); t[0x4E] = ord("+")
        out.append(t)
    return out

lines = ["""/* ===========================================================================
 * keymaps.c -- the keyboard layouts.  GENERATED by tools/build/mkkeymap.py from
 * the layouts written out there.  Do not hand edit.
 *
 * Per layout, four tables by scancode: plain, Shift, AltGr, AltGr+Shift.
 * 0 types nothing; 1..4 are the accent keys that wait for a letter
 * (grave, acute, circumflex, diaeresis).
 * ===========================================================================
 */
#include "flopos.h"
#include "keymaps.h"
"""]
done = {}
for name, title, levels in LAYOUTS:
    if not isinstance(levels, dict):
        continue
    sym = "s_" + name
    lines.append("static const u16 %s[4][KEYMAP_SCANS] = {" % sym)
    for lv, t in zip(LEVELS, table(levels)):
        lines.append("    { /* %s */" % lv)
        for i in range(0, 0x59, 16):
            lines.append("      " + ", ".join("%3d" % v for v in t[i:i + 16]) + ",")
        lines.append("    },")
    lines.append("};")
    lines.append("")
    done[name] = sym

lines.append("const Keymap g_keymaps[KEYMAP_COUNT] = {")
names = [n for n, _, _ in LAYOUTS]
for name, title, levels in LAYOUTS:
    sym = done[name] if isinstance(levels, dict) else done[levels]
    latin = names.index(levels["latin"]) if isinstance(levels, dict) and "latin" in levels else -1
    lines.append('    { "%s", "%s", %s, %d },' % (name, title, sym, latin))
lines.append("};")
open("src/kernel/gen/keymaps.c", "w").write("\n".join(lines) + "\n")

hdr = """/* keymaps.h -- GENERATED by tools/build/mkkeymap.py.  Do not hand edit. */
#ifndef KEYMAPS_H
#define KEYMAPS_H

#define KEYMAP_SCANS 0x59
#define KEYMAP_COUNT %d

typedef struct {
    const char *code;                    /* what you type: se, us, de ...  */
    const char *name;                    /* what it is called              */
    const u16 (*keys)[KEYMAP_SCANS];     /* [level][scancode]              */
    int latin;                           /* Alt+Shift switches to this one, or -1 */
} Keymap;

extern const Keymap g_keymaps[KEYMAP_COUNT];

#endif /* KEYMAPS_H */
""" % len(LAYOUTS)
open("src/kernel/gen/keymaps.h", "w").write(hdr)
print("wrote src/kernel/gen/keymaps.c and src/kernel/gen/keymaps.h: %s"
      % " ".join(n for n, _, _ in LAYOUTS))
