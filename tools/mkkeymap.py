#!/usr/bin/env python3
"""Generate kernel/keymaps.c from the keyboard layouts written out below.

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

# Character codes, as kernel/flopos.h defines them.
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
    if 32 <= n < 127 or 0xB4 <= n <= 0xFF:
        return n
    sys.exit("no character code for %r" % ch)

# name, what it is called, and the levels.  Each level is four rows.
LAYOUTS = [
  ("se", "Swedish", {
    "plain": ["§1234567890+ˊ", "qwertyuiopå‥", "asdfghjklöä'", "<zxcvbnm,.-"],
    "shift": ["·!\"#·%&/()=?ˋ", "QWERTYUIOPÅ^", "ASDFGHJKLÖÄ*", ">ZXCVBNM;:_"],
    "altgr": ["··@£$€·{[]}\\·", "··€········~", "············", "|······µ···"],
  }),
  ("fi", "Finnish", None),                    # the same keys as Swedish
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

LEVELS = ["plain", "shift", "altgr", "altgr_shift"]

def table(levels):
    out = []
    for lv in LEVELS:
        t = [0] * 0x59
        rows = levels.get(lv)
        if rows:
            assert len(rows) == 4, lv
            for r, (scs, keys) in enumerate(zip(ROWS, rows)):
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
 * keymaps.c -- the keyboard layouts.  GENERATED by tools/mkkeymap.py from
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
    if levels is None:
        continue
    sym = "s_" + name
    lines.append("static const u8 %s[4][KEYMAP_SCANS] = {" % sym)
    for lv, t in zip(LEVELS, table(levels)):
        lines.append("    { /* %s */" % lv)
        for i in range(0, 0x59, 16):
            lines.append("      " + ", ".join("%3d" % v for v in t[i:i + 16]) + ",")
        lines.append("    },")
    lines.append("};")
    lines.append("")
    done[name] = sym

lines.append("const Keymap g_keymaps[KEYMAP_COUNT] = {")
for name, title, levels in LAYOUTS:
    sym = done[name] if levels else done["se"]
    lines.append('    { "%s", "%s", %s },' % (name, title, sym))
lines.append("};")
open("kernel/keymaps.c", "w").write("\n".join(lines) + "\n")

hdr = """/* keymaps.h -- GENERATED by tools/mkkeymap.py.  Do not hand edit. */
#ifndef KEYMAPS_H
#define KEYMAPS_H

#define KEYMAP_SCANS 0x59
#define KEYMAP_COUNT %d

typedef struct {
    const char *code;                    /* what you type: se, us, de ...  */
    const char *name;                    /* what it is called              */
    const u8 (*keys)[KEYMAP_SCANS];      /* [level][scancode]              */
} Keymap;

extern const Keymap g_keymaps[KEYMAP_COUNT];

#endif /* KEYMAPS_H */
""" % len(LAYOUTS)
open("kernel/keymaps.h", "w").write(hdr)
print("wrote kernel/keymaps.c and kernel/keymaps.h: %s"
      % " ".join(n for n, _, _ in LAYOUTS))
