#!/usr/bin/env python3
"""Generate kernel/font.c and kernel/fontmap.h from readable glyph art.

Each glyph is five columns wide and eight rows tall. The C side stores one
byte per column, bit 0 being the top row, which makes the renderer a tight
shift-and-test loop.

Capitals and digits fill rows 0..6 on a baseline at row 6. Lowercase has an
x-height of rows 2..6 with ascenders reaching row 0 and descenders in row 7
-- the row that is otherwise the gap between lines, which is how twenty-five
rows of readable text fit in 200 scanlines.

Covered: ASCII 32..126, the Swedish letters in both cases, and the blocks the
terminal draws with. The slot numbers go into fontmap.h so nothing has to be
kept in sync by hand.
"""
import sys

CELL_H = 8          # rows per glyph; row 7 is the descender / gap row
CELL_W = 5

# Capitals and digits occupy rows 0..6, baseline at row 6.
# Lowercase x-height is rows 2..6, ascenders reach row 0, descenders row 7.
# Art may be written 7 rows tall and is padded; 8 rows when it descends.

G = {}
def pad(ch, rows):
    rows = list(rows)
    assert len(rows) in (7, 8), (ch, len(rows))
    for r in rows:
        assert len(r) == CELL_W, (ch, r)
    while len(rows) < CELL_H:
        rows.append("." * CELL_W)
    return rows

def g(ch, *rows):
    G[ch] = pad(ch, rows)

g(' ', ".....", ".....", ".....", ".....", ".....", ".....", ".....")
g('!', "..#..", "..#..", "..#..", "..#..", "..#..", ".....", "..#..")
g('"', ".#.#.", ".#.#.", ".....", ".....", ".....", ".....", ".....")
g('#', ".#.#.", ".#.#.", "#####", ".#.#.", "#####", ".#.#.", ".#.#.")
g('$', "..#..", ".####", "#.#..", ".###.", "..#.#", "####.", "..#..")
g('%', "##..#", "##..#", "...#.", "..#..", ".#...", "#..##", "#..##")
g('&', ".##..", "#..#.", "#.#..", ".#...", "#.#.#", "#..#.", ".##.#")
g("'", "..#..", "..#..", ".....", ".....", ".....", ".....", ".....")
g('(', "...#.", "..#..", ".#...", ".#...", ".#...", "..#..", "...#.")
g(')', ".#...", "..#..", "...#.", "...#.", "...#.", "..#..", ".#...")
g('*', ".....", "#.#.#", ".###.", "#####", ".###.", "#.#.#", ".....")
g('+', ".....", "..#..", "..#..", "#####", "..#..", "..#..", ".....")
g(',', ".....", ".....", ".....", ".....", ".....", ".....", "..##.", ".#...")
g('-', ".....", ".....", ".....", "#####", ".....", ".....", ".....")
g('.', ".....", ".....", ".....", ".....", ".....", ".##..", ".##..")
g('/', "....#", "...#.", "...#.", "..#..", ".#...", ".#...", "#....")
g('0', ".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###.")
g('1', "..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###.")
g('2', ".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####")
g('3', "#####", "...#.", "..#..", "...#.", "....#", "#...#", ".###.")
g('4', "...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#.")
g('5', "#####", "#....", "####.", "....#", "....#", "#...#", ".###.")
g('6', "..##.", ".#...", "#....", "####.", "#...#", "#...#", ".###.")
g('7', "#####", "....#", "...#.", "..#..", "..#..", "..#..", "..#..")
g('8', ".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###.")
g('9', ".###.", "#...#", "#...#", ".####", "....#", "...#.", ".##..")
g(':', ".....", ".##..", ".##..", ".....", ".##..", ".##..", ".....")
g(';', ".....", ".##..", ".##..", ".....", ".##..", "..#..", ".#...")
g('<', "....#", "...#.", "..#..", ".#...", "..#..", "...#.", "....#")
g('=', ".....", ".....", "#####", ".....", "#####", ".....", ".....")
g('>', "#....", ".#...", "..#..", "...#.", "..#..", ".#...", "#....")
g('?', ".###.", "#...#", "....#", "...#.", "..#..", ".....", "..#..")
g('@', ".###.", "#...#", "....#", ".##.#", "#.#.#", "#.#.#", ".##..")
g('A', ".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#")
g('B', "####.", "#...#", "#...#", "####.", "#...#", "#...#", "####.")
g('C', ".###.", "#...#", "#....", "#....", "#....", "#...#", ".###.")
g('D', "####.", "#...#", "#...#", "#...#", "#...#", "#...#", "####.")
g('E', "#####", "#....", "#....", "####.", "#....", "#....", "#####")
g('F', "#####", "#....", "#....", "####.", "#....", "#....", "#....")
g('G', ".###.", "#...#", "#....", "#..##", "#...#", "#...#", ".###.")
g('H', "#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#")
g('I', ".###.", "..#..", "..#..", "..#..", "..#..", "..#..", ".###.")
g('J', "..###", "...#.", "...#.", "...#.", "...#.", "#..#.", ".##..")
g('K', "#...#", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#")
g('L', "#....", "#....", "#....", "#....", "#....", "#....", "#####")
g('M', "#...#", "##.##", "#.#.#", "#.#.#", "#...#", "#...#", "#...#")
g('N', "#...#", "#...#", "##..#", "#.#.#", "#..##", "#...#", "#...#")
g('O', ".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###.")
g('P', "####.", "#...#", "#...#", "####.", "#....", "#....", "#....")
g('Q', ".###.", "#...#", "#...#", "#...#", "#.#.#", "#..#.", ".##.#")
g('R', "####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#")
g('S', ".####", "#....", "#....", ".###.", "....#", "....#", "####.")
g('T', "#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#..")
g('U', "#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###.")
g('V', "#...#", "#...#", "#...#", "#...#", "#...#", ".#.#.", "..#..")
g('W', "#...#", "#...#", "#...#", "#.#.#", "#.#.#", "##.##", "#...#")
g('X', "#...#", "#...#", ".#.#.", "..#..", ".#.#.", "#...#", "#...#")
g('Y', "#...#", "#...#", ".#.#.", "..#..", "..#..", "..#..", "..#..")
g('Z', "#####", "....#", "...#.", "..#..", ".#...", "#....", "#####")
g('[', ".###.", ".#...", ".#...", ".#...", ".#...", ".#...", ".###.")
g('\\', "#....", ".#...", ".#...", "..#..", "...#.", "...#.", "....#")
g(']', ".###.", "...#.", "...#.", "...#.", "...#.", "...#.", ".###.")
g('^', "..#..", ".#.#.", "#...#", ".....", ".....", ".....", ".....")
g('_', ".....", ".....", ".....", ".....", ".....", ".....", ".....", "#####")

# Glyphs past ASCII, for a machine with a Swedish keyboard. The kernel
# addresses these as character codes 0x80 upward; see CH_* in flopos.h.
EXTRA = []
def x(name, *rows):
    EXTRA.append((name, pad(name, rows)))

# A-ring and the umlauts lose a row off the letter to make room above.
x("A-ring", "..#..", ".....", ".###.", "#...#", "#####", "#...#", "#...#")
x("A-diaeresis", ".#.#.", ".....", ".###.", "#...#", "#####", "#...#", "#...#")
x("O-diaeresis", ".#.#.", ".....", ".###.", "#...#", "#...#", "#...#", ".###.")
x("solid block", "#####", "#####", "#####", "#####", "#####", "#####", "#####", "#####")
x("soft block", "#.#.#", ".#.#.", "#.#.#", ".#.#.", "#.#.#", ".#.#.", "#.#.#", ".#.#.")
x("left caret", "..#..", ".##..", "###..", ".##..", "..#..", ".....", ".....")

# Past ASCII 95, so not in the main table, but AltGr on a Swedish keyboard
# produces all of these and a shell has no business rendering them as '?'.
x("brace open", "..##.", ".#...", ".#...", "#....", ".#...", ".#...", "..##.")
x("brace close", ".##..", "...#.", "...#.", "....#", "...#.", "...#.", ".##..")
x("pipe", "..#..", "..#..", "..#..", "..#..", "..#..", "..#..", "..#..")
x("tilde", ".....", ".....", ".#...", "#.#.#", "...#.", ".....", ".....")
x("backtick", ".#...", "..#..", ".....", ".....", ".....", ".....", ".....")

# The whole of a little figure in one cell -- head, torso and legs. Its
# arms are not here: they are tildes drawn either side at wandering
# heights, so that they can noodle.
x("noodler", ".###.", ".#.#.", ".###.", "..#..", ".###.", "..#..", ".#.#.", "#...#")


# Lowercase. x-height is rows 2..6 with the baseline at row 6, so these sit
# a row shorter than the capitals, and g j p q y reach into row 7. That row
# is the inter-line gap for every other glyph, which is how 25 rows of text
# fit in 200 scanlines and still read as two separate lines.
LOWER = "abcdefghijklmnopqrstuvwxyz"
def lo(ch, *rows):
    G[ch] = pad(ch, rows)

lo('a', ".....", ".....", ".###.", "....#", ".####", "#...#", ".####")
lo('b', "#....", "#....", "####.", "#...#", "#...#", "#...#", "####.")
lo('c', ".....", ".....", ".###.", "#...#", "#....", "#...#", ".###.")
lo('d', "....#", "....#", ".####", "#...#", "#...#", "#...#", ".####")
lo('e', ".....", ".....", ".###.", "#...#", "#####", "#....", ".###.")
lo('f', "..##.", ".#...", "####.", ".#...", ".#...", ".#...", ".#...")
lo('g', ".....", ".....", ".####", "#...#", "#...#", ".####", "....#", ".###.")
lo('h', "#....", "#....", "####.", "#...#", "#...#", "#...#", "#...#")
lo('i', "..#..", ".....", ".##..", "..#..", "..#..", "..#..", ".###.")
lo('j', "...#.", ".....", "...#.", "...#.", "...#.", "...#.", "#..#.", ".##..")
lo('k', "#....", "#....", "#..#.", "#.#..", "##...", "#.#..", "#..#.")
lo('l', ".##..", "..#..", "..#..", "..#..", "..#..", "..#..", ".###.")
lo('m', ".....", ".....", "##.#.", "#.#.#", "#.#.#", "#.#.#", "#...#")
lo('n', ".....", ".....", "####.", "#...#", "#...#", "#...#", "#...#")
lo('o', ".....", ".....", ".###.", "#...#", "#...#", "#...#", ".###.")
lo('p', ".....", ".....", "####.", "#...#", "#...#", "####.", "#....", "#....")
lo('q', ".....", ".....", ".####", "#...#", "#...#", ".####", "....#", "....#")
lo('r', ".....", ".....", "#.##.", "##..#", "#....", "#....", "#....")
lo('s', ".....", ".....", ".####", "#....", ".###.", "....#", "####.")
lo('t', ".#...", ".#...", "####.", ".#...", ".#...", ".#..#", "..##.")
lo('u', ".....", ".....", "#...#", "#...#", "#...#", "#...#", ".####")
lo('v', ".....", ".....", "#...#", "#...#", "#...#", ".#.#.", "..#..")
lo('w', ".....", ".....", "#...#", "#...#", "#.#.#", "#.#.#", ".#.#.")
lo('x', ".....", ".....", "#...#", ".#.#.", "..#..", ".#.#.", "#...#")
lo('y', ".....", ".....", "#...#", "#...#", "#...#", ".####", "....#", ".###.")
lo('z', ".....", ".....", "#####", "...#.", "..#..", ".#...", "#####")

# The Swedish lowercase, same trick as the capitals: a shorter body with
# the mark above it.
lo(chr(0xE5), "..#..", ".....", ".###.", "....#", ".####", "#...#", ".####")   # a-ring
lo(chr(0xE4), ".#.#.", ".....", ".###.", "....#", ".####", "#...#", ".####")   # a-diaeresis
lo(chr(0xF6), ".#.#.", ".....", ".###.", "#...#", "#...#", "#...#", ".###.")   # o-diaeresis

# ---------------------------------------------------------------------------
# Emit kernel/font.c and kernel/fontmap.h.  The header carries the slot
# numbers so nothing has to be kept in sync by hand.
# ---------------------------------------------------------------------------

def columns(rows):
    cols = []
    for cx in range(CELL_W):
        byte = 0
        for cy in range(CELL_H):
            if rows[cy][cx] == '#':
                byte |= 1 << cy
        cols.append(byte)
    return cols


table = []          # (slot, comment, cols)
slot = 0

for code in range(32, 96):
    ch = chr(code)
    if ch not in G:
        sys.exit("missing glyph for %r" % ch)
    label = "\\\\" if ch == "\\" else ch
    table.append((slot, "%-3d '%s'" % (code, label), columns(G[ch])))
    slot += 1

extra_base = slot
extra_names = []
for name, rows in EXTRA:
    table.append((slot, name, columns(rows)))
    extra_names.append((name, slot))
    slot += 1

lower_base = slot
for ch in LOWER:
    table.append((slot, "'%s'" % ch, columns(G[ch])))
    slot += 1

swed_base = slot
for ch, name in ((chr(0xE5), "a-ring"), (chr(0xE4), "a-diaeresis"),
                 (chr(0xF6), "o-diaeresis")):
    table.append((slot, "lowercase " + name, columns(G[ch])))
    slot += 1

total = slot

lines = ["""/* ===========================================================================
 * font.c -- the FLOPOS text face.  GENERATED by tools/mkfont.py from the
 * glyph art in that file.  Do not hand edit; edit the art and regenerate.
 *
 * Five pixels wide, eight rows tall, one byte per column with bit 0 at the
 * top.  Capitals and digits fill rows 0..6 and sit on a baseline at row 6.
 * Lowercase has an x-height of rows 2..6, ascenders reaching row 0, and
 * descenders in row 7 -- the row that is otherwise the gap between lines,
 * which is how twenty-five rows of readable text fit in 200 scanlines.
 *
 * Slot numbers are published in fontmap.h so nothing is kept in sync by
 * hand.
 * ===========================================================================
 */
#include "flopos.h"

const u8 g_font5x8[FONT_GLYPHS][5] = {"""]

last = -1
for n, comment, cols in table:
    if n in (extra_base, lower_base, swed_base) and n != last + 1 - 1:
        pass
    lines.append("    { 0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X },  /* %3d  %s */"
                 % (*cols, n, comment))
    last = n
lines.append("};")
lines.append("")
open("kernel/font.c", "w").write("\n".join(lines) + "\n")

hdr = ["""/* ===========================================================================
 * fontmap.h -- where each glyph lives in g_font5x8.  GENERATED by
 * tools/mkfont.py.  Do not hand edit.
 * ===========================================================================
 */
#ifndef FONTMAP_H
#define FONTMAP_H

#define FONT_GLYPHS   %d
#define GLYPH_W       %d
#define GLYPH_H       %d

/* ASCII 32..95 occupy slots 0..63, so a printable capital is (c - 32). */
#define G_ASCII_BASE  0

/* Lowercase a..z, then the three Swedish lowercase letters. */
#define G_LOWER_BASE  %d
#define G_LOWER_ARING %d
#define G_LOWER_ADIA  %d
#define G_LOWER_ODIA  %d
""" % (total, CELL_W, CELL_H, lower_base, swed_base, swed_base + 1, swed_base + 2)]

for name, n in extra_names:
    sym = name.upper().replace("-", "_").replace(" ", "_")
    hdr.append("#define G_%-12s %d" % (sym, n))

hdr.append("")
hdr.append("#endif /* FONTMAP_H */")
open("kernel/fontmap.h", "w").write("\n".join(hdr) + "\n")

print("wrote kernel/font.c and kernel/fontmap.h")
print("  %d ascii  %d extra  %d lowercase  %d swedish  = %d glyphs"
      % (64, len(EXTRA), len(LOWER), 3, total))
