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
    assert len(rows) in (CELL_H - 1, CELL_H), (ch, len(rows))
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


# ===========================================================================
# The second face: four columns by six rows.
#
# Eighty columns by thirty-three rows, which is the grid the whole text-user-
# interface tradition was built on, and the reason this face exists. Caps and
# digits fill rows 0..4 on a baseline at row 4. Lowercase keeps a four-row
# x-height (rows 1..4) rather than the three rows that would be
# typographically correct, because at three pixels of width a three-pixel
# body stops being a letter and starts being a smudge.
#
# Three ink columns, with the fourth left as air. M N W m and a few symbols
# take the fourth, so those kiss their neighbour by a pixel. That is the
# trade every four-pixel face makes and it is the right one.
# ===========================================================================
G5, EXTRA5, CELL5 = G, EXTRA, (CELL_W, CELL_H)
G, EXTRA = {}, []
CELL_W, CELL_H = 4, 6

g(' ', "....", "....", "....", "....", "....")
g('!', ".#..", ".#..", ".#..", "....", ".#..")
g('"', "#.#.", "#.#.", "....", "....", "....")
g('#', "#.#.", "###.", "#.#.", "###.", "#.#.")
g('$', "###.", "##..", "###.", ".##.", "###.")
g('%', "#.#.", "..#.", ".#..", "#...", "#.#.")
g('&', ".#..", "#.#.", ".#..", "#.#.", ".##.")
g("'", ".#..", ".#..", "....", "....", "....")
g('(', "..#.", ".#..", ".#..", ".#..", "..#.")
g(')', ".#..", "..#.", "..#.", "..#.", ".#..")
g('*', "....", "#.#.", ".#..", "#.#.", "....")
g('+', "....", ".#..", "###.", ".#..", "....")
g(',', "....", "....", "....", "....", ".#..", "#...")
g('-', "....", "....", "###.", "....", "....")
g('.', "....", "....", "....", "....", ".#..")
g('/', "..#.", "..#.", ".#..", "#...", "#...")
g('0', ".#..", "#.#.", "#.#.", "#.#.", ".#..")
g('1', ".#..", "##..", ".#..", ".#..", "###.")
g('2', "###.", "..#.", "###.", "#...", "###.")
g('3', "###.", "..#.", ".##.", "..#.", "###.")
g('4', "#.#.", "#.#.", "###.", "..#.", "..#.")
g('5', "###.", "#...", "###.", "..#.", "###.")
g('6', "###.", "#...", "###.", "#.#.", "###.")
g('7', "###.", "..#.", "..#.", ".#..", ".#..")
g('8', "###.", "#.#.", "###.", "#.#.", "###.")
g('9', "###.", "#.#.", "###.", "..#.", "###.")
g(':', "....", ".#..", "....", ".#..", "....")
g(';', "....", "....", ".#..", "....", ".#..", "#...")
g('<', "..#.", ".#..", "#...", ".#..", "..#.")
g('=', "....", "###.", "....", "###.", "....")
g('>', "#...", ".#..", "..#.", ".#..", "#...")
g('?', "###.", "..#.", ".##.", "....", ".#..")
g('@', "###.", "#.#.", "###.", "#...", ".##.")
g('A', "###.", "#.#.", "###.", "#.#.", "#.#.")
g('B', "##..", "#.#.", "##..", "#.#.", "##..")
g('C', ".##.", "#...", "#...", "#...", ".##.")
g('D', "##..", "#.#.", "#.#.", "#.#.", "##..")
g('E', "###.", "#...", "##..", "#...", "###.")
g('F', "###.", "#...", "##..", "#...", "#...")
g('G', ".##.", "#...", "#.#.", "#.#.", ".##.")
g('H', "#.#.", "#.#.", "###.", "#.#.", "#.#.")
g('I', "###.", ".#..", ".#..", ".#..", "###.")
g('J', "..#.", "..#.", "..#.", "#.#.", ".#..")
g('K', "#.#.", "#.#.", "##..", "#.#.", "#.#.")
g('L', "#...", "#...", "#...", "#...", "###.")
g('M', "#..#", "####", "#..#", "#..#", "#..#")
g('N', "#..#", "##.#", "#.##", "#..#", "#..#")
g('O', "###.", "#.#.", "#.#.", "#.#.", "###.")
g('P', "##..", "#.#.", "##..", "#...", "#...")
g('Q', "###.", "#.#.", "#.#.", "###.", "..#.")
g('R', "##..", "#.#.", "##..", "#.#.", "#.#.")
g('S', "###.", "#...", "###.", "..#.", "###.")
g('T', "###.", ".#..", ".#..", ".#..", ".#..")
g('U', "#.#.", "#.#.", "#.#.", "#.#.", "###.")
g('V', "#.#.", "#.#.", "#.#.", "#.#.", ".#..")
g('W', "#..#", "#..#", "#..#", "####", "#..#")
g('X', "#.#.", "#.#.", ".#..", "#.#.", "#.#.")
g('Y', "#.#.", "#.#.", ".#..", ".#..", ".#..")
g('Z', "###.", "..#.", ".#..", "#...", "###.")
g('[', "##..", "#...", "#...", "#...", "##..")
g('\\', "#...", "#...", ".#..", "..#.", "..#.")
g(']', ".##.", "..#.", "..#.", "..#.", ".##.")
g('^', ".#..", "#.#.", "....", "....", "....")
g('_', "....", "....", "....", "....", "###.")

x("A-ring", ".#..", "###.", "#.#.", "###.", "#.#.")
x("A-diaeresis", "#.#.", "###.", "#.#.", "###.", "#.#.")
x("O-diaeresis", "#.#.", "###.", "#.#.", "#.#.", "###.")
x("solid block", "####", "####", "####", "####", "####", "####")
x("soft block", "#.#.", ".#.#", "#.#.", ".#.#", "#.#.", ".#.#")
x("left caret", ".#..", "##..", "###.", "##..", ".#..")
x("brace open", "..#.", ".#..", "##..", ".#..", "..#.")
x("brace close", "#...", ".#..", ".##.", ".#..", "#...")
x("pipe", ".#..", ".#..", ".#..", ".#..", ".#..")
x("tilde", "....", "....", ".#.#", "#.#.", "....")
x("backtick", "#...", ".#..", "....", "....", "....")
x("noodler", ".##.", "#..#", ".##.", ".##.", ".##.", "#..#")

lo('a', "....", "###.", "#.#.", "#.#.", ".##.")
lo('b', "#...", "#...", "##..", "#.#.", "##..")
lo('c', "....", ".##.", "#...", "#...", ".##.")
lo('d', "..#.", "..#.", ".##.", "#.#.", ".##.")
lo('e', "....", ".##.", "###.", "#...", ".##.")
lo('f', ".##.", ".#..", "###.", ".#..", ".#..")
lo('g', "....", ".##.", "#.#.", ".##.", "..#.", "##..")
lo('h', "#...", "#...", "##..", "#.#.", "#.#.")
lo('i', ".#..", "....", ".#..", ".#..", ".#..")
lo('j', "..#.", "....", "..#.", "..#.", "..#.", "##..")
lo('k', "#...", "#.#.", "##..", "##..", "#.#.")
lo('l', "##..", ".#..", ".#..", ".#..", ".##.")
lo('m', "....", "###.", "###.", "#.#.", "#.#.")
lo('n', "....", "##..", "#.#.", "#.#.", "#.#.")
lo('o', "....", ".#..", "#.#.", "#.#.", ".#..")
lo('p', "....", "##..", "#.#.", "##..", "#...", "#...")
lo('q', "....", ".##.", "#.#.", ".##.", "..#.", "..#.")
lo('r', "....", ".##.", "#...", "#...", "#...")
lo('s', "....", ".##.", "##..", "..#.", "##..")
lo('t', ".#..", "###.", ".#..", ".#..", ".##.")
lo('u', "....", "#.#.", "#.#.", "#.#.", ".##.")
lo('v', "....", "#.#.", "#.#.", "#.#.", ".#..")
lo('w', "....", "#..#", "#..#", "####", ".##.")
lo('x', "....", "#.#.", ".#..", ".#..", "#.#.")
lo('y', "....", "#.#.", "#.#.", ".##.", "..#.", "##..")
lo('z', "....", "###.", "..#.", "#...", "###.")

lo(chr(0xE5), ".#..", "###.", "#.#.", "#.#.", ".##.")   # a-ring
lo(chr(0xE4), "#.#.", "###.", "#.#.", "#.#.", ".##.")   # a-diaeresis
lo(chr(0xF6), "#.#.", ".#..", "#.#.", "#.#.", ".#..")   # o-diaeresis

G4, EXTRA4, CELL4 = G, EXTRA, (CELL_W, CELL_H)

# ---------------------------------------------------------------------------
# Emit kernel/font.c and kernel/fontmap.h.  Both faces are laid out in the
# same slot order, so glyph_index() serves either one and a face can be
# swapped under the terminal at runtime.
#
# Box drawing is deliberately absent here.  A line that has to join its
# neighbour cannot come out of a glyph narrower than the advance, so the
# terminal draws those geometrically at cell size instead -- see term.c.
# ---------------------------------------------------------------------------

def columns(rows, w, h):
    cols = []
    for cx in range(w):
        byte = 0
        for cy in range(h):
            if rows[cy][cx] == '#':
                byte |= 1 << cy
        cols.append(byte)
    return cols


def build(gd, extra, cell):
    """One face, in the canonical slot order, as a list of column lists."""
    w, h = cell
    out = []
    for code in range(32, 96):
        ch = chr(code)
        if ch not in gd:
            sys.exit("face %dx%d: missing glyph for %r" % (w, h, ch))
        label = "\\\\" if ch == "\\" else ch
        out.append(("%-3d '%s'" % (code, label), columns(gd[ch], w, h)))
    for name, rows in extra:
        out.append((name, columns(rows, w, h)))
    for ch in LOWER:
        out.append(("'%s'" % ch, columns(gd[ch], w, h)))
    for ch, name in ((chr(0xE5), "a-ring"), (chr(0xE4), "a-diaeresis"),
                     (chr(0xF6), "o-diaeresis")):
        out.append(("lowercase " + name, columns(gd[ch], w, h)))
    return out


five = build(G5, EXTRA5, CELL5)
four = build(G4, EXTRA4, CELL4)
assert len(five) == len(four), (len(five), len(four))

total      = len(five)
extra_base = 64
lower_base = 64 + len(EXTRA5)
swed_base  = lower_base + len(LOWER)
extra_names = [(name, extra_base + i) for i, (name, _) in enumerate(EXTRA5)]


def table(sym, cell, rows):
    w, _ = cell
    out = ["const u8 %s[FONT_GLYPHS][%d] = {" % (sym, w)]
    for n, (comment, cols) in enumerate(rows):
        out.append("    { %s },  /* %3d  %s */"
                   % (", ".join("0x%02X" % c for c in cols), n, comment))
    out.append("};")
    return out


lines = ["""/* ===========================================================================
 * font.c -- the FLOPOS text faces.  GENERATED by tools/mkfont.py from the
 * glyph art in that file.  Do not hand edit; edit the art and regenerate.
 *
 * One byte per column with bit 0 at the top, which makes the renderer a
 * shift-and-test loop.  Two faces, laid out in the same slot order so one
 * glyph_index() serves both and the terminal can change face while running:
 *
 *   g_font5x8   five wide, eight tall, 6x8 cell -> 53 columns by 25 rows
 *   g_font4x6   four wide, six tall,  4x6 cell -> 80 columns by 33 rows
 *
 * The large face puts capitals on a baseline at row 6 with descenders in
 * row 7, the row that is otherwise the gap between lines.  The small face
 * has its baseline at row 4 and keeps a four-row x-height, because three
 * pixels of body on three pixels of width is not a letter any more.
 * ===========================================================================
 */
#include "flopos.h"
"""]
lines += table("g_font5x8", CELL5, five)
lines.append("")
lines += table("g_font4x6", CELL4, four)
lines.append("")
lines.append("""/* The faces as the terminal sees them.  adv and pitch are the cell, which
 * is one pixel wider and (for the large face) the same height as the ink;
 * cols and rows are what then fits in 320x200 starting at x0. */
const Face g_faces[FACE_COUNT] = {
    { (const u8 *)g_font5x8, 5, 8, 6, 8, 53, 25, 1, 1, "roomy"  },
    { (const u8 *)g_font4x6, 4, 6, 4, 6, 80, 33, 0, 1, "close"  },
};""")
lines.append("")
open("kernel/font.c", "w").write("\n".join(lines) + "\n")

hdr = ["""/* ===========================================================================
 * fontmap.h -- where each glyph lives in both faces.  GENERATED by
 * tools/mkfont.py.  Do not hand edit.
 * ===========================================================================
 */
#ifndef FONTMAP_H
#define FONTMAP_H

#define FONT_GLYPHS   %d
#define FACE_COUNT    2

/* The widest and tallest cell any face uses, for buffers sized once. */
#define GLYPH_W       %d
#define GLYPH_H       %d
#define TERM_MAX_COLS %d
#define TERM_MAX_ROWS %d

/* ASCII 32..95 occupy slots 0..63, so a printable capital is (c - 32). */
#define G_ASCII_BASE  0

/* Lowercase a..z, then the three Swedish lowercase letters. */
#define G_LOWER_BASE  %d
#define G_LOWER_ARING %d
#define G_LOWER_ADIA  %d
#define G_LOWER_ODIA  %d
""" % (total, CELL5[0], CELL5[1], 80, 33,
       lower_base, swed_base, swed_base + 1, swed_base + 2)]

for name, n in extra_names:
    sym = name.upper().replace("-", "_").replace(" ", "_")
    hdr.append("#define G_%-12s %d" % (sym, n))

hdr.append("")
hdr.append("#endif /* FONTMAP_H */")
open("kernel/fontmap.h", "w").write("\n".join(hdr) + "\n")

print("wrote kernel/font.c and kernel/fontmap.h")
print("  %d glyphs in each of 2 faces  (%d ascii, %d extra, %d lower, 3 swedish)"
      % (total, 64, len(EXTRA5), len(LOWER)))

# ---------------------------------------------------------------------------
# A proof, printed, because a font is the one thing that cannot be verified
# by compiling it.  --preview renders a line of text in both faces.
# ---------------------------------------------------------------------------
if "--preview" in sys.argv:
    sample = ("Vem kan segla forutan vind?  jag 0123456789 "
              "{}[]()<>/\\|~`!?@#$%&*+-=_;:,.'\"^")
    def show(gd, extra, cell, name):
        w, h = cell
        print("\n--- %s  (%dx%d) ---" % (name, w, h))
        for ch in (sample[:52], sample[52:]):
            art = [""] * h
            for c in ch:
                rows = gd.get(c) or gd.get(c.lower()) or gd[' ']
                for y in range(h):
                    art[y] += rows[y].replace('#', '#').replace('.', ' ') + " "
            for ln in art:
                print("  |" + ln.rstrip())
            print()
    show(G5, EXTRA5, CELL5, "roomy")
    show(G4, EXTRA4, CELL4, "close")
