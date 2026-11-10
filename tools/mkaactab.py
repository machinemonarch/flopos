#!/usr/bin/env python3
"""Make apps/lib/audio/aactab.h: the fixed tables an AAC decoder needs.

    tools/mkaactab.py aac_rom.c [apps/lib/audio/aactab.h]

The tables are data from the AAC standard (ISO/IEC 13818-7 and 14496-3):
the Huffman codes of the eleven spectral codebooks and the scalefactor
codebook, and where the scalefactor bands fall at each sample rate.  They
are taken from the VisualOn AAC encoder (Apache License 2.0,
github.com/mstorsjo/vo-aacenc, aacenc/src/aac_rom.c).  An encoder needs the
very same codes a decoder does, only the other way round, and keeps them
as plain (code, length) tables: two codebooks' lengths packed into one
16-bit number for books 1 to 10, which this unpacks.

Every codebook is checked to be a complete prefix code before anything is
written, so a misread table fails here and not in the decoder.

The output is committed, so building does not need this tool or the
encoder's source.
"""
import re
import sys


def array_body(src, name):
    m = re.search(r"\b" + name + r"\s*(\[[^\]]*\])+\s*=\s*\{", src)
    if not m:
        sys.exit("no %s in the input" % name)
    i = m.end()
    depth = 1
    j = i
    while depth:
        if src[j] == "{":
            depth += 1
        elif src[j] == "}":
            depth -= 1
        j += 1
    body = re.sub(r"/\*.*?\*/", "", src[i:j - 1], flags=re.S)
    return [int(v, 0) for v in re.findall(r"0x[0-9a-fA-F]+|\d+", body)]


# (name of the code table, name of the length table, which byte, entries)
BOOKS = [
    None,
    ("huff_ctab1", "huff_ltab1_2", 1, 81),
    ("huff_ctab2", "huff_ltab1_2", 0, 81),
    ("huff_ctab3", "huff_ltab3_4", 1, 81),
    ("huff_ctab4", "huff_ltab3_4", 0, 81),
    ("huff_ctab5", "huff_ltab5_6", 1, 81),
    ("huff_ctab6", "huff_ltab5_6", 0, 81),
    ("huff_ctab7", "huff_ltab7_8", 1, 64),
    ("huff_ctab8", "huff_ltab7_8", 0, 64),
    ("huff_ctab9", "huff_ltab9_10", 1, 169),
    ("huff_ctab10", "huff_ltab9_10", 0, 169),
    ("huff_ctab11", "huff_ltab11", 0, 289),
]


def check(codes, what):
    """A complete prefix code: Kraft sum exactly one, and no code a prefix
    of another."""
    total = sum(2.0 ** -n for c, n in codes)
    if abs(total - 1.0) > 1e-12:
        sys.exit("%s: lengths do not make a complete code (%.9f)" % (what, total))
    words = set(format(c, "0%db" % n) for c, n in codes)
    if len(words) != len(codes):
        sys.exit("%s: two entries share a code" % what)
    for wd in words:
        for k in range(1, len(wd)):
            if wd[:k] in words:
                sys.exit("%s: one code is the start of another" % what)


def main():
    src = open(sys.argv[1]).read()
    out_path = sys.argv[2] if len(sys.argv) > 2 else "apps/lib/audio/aactab.h"

    codes, lens, starts = [], [], [0]
    for b in range(1, 12):
        cname, lname, byte, n = BOOKS[b]
        c = array_body(src, cname)
        l = array_body(src, lname)
        if len(c) != n or len(l) != n:
            sys.exit("book %d: expected %d entries, found %d and %d" % (b, n, len(c), len(l)))
        l = [(v >> 8) & 0xFF if byte else v & 0xFF for v in l]
        check(list(zip(c, l)), "book %d" % b)
        codes += c
        lens += l
        starts.append(len(codes))

    sc = array_body(src, "huff_ctabscf")
    sl = array_body(src, "huff_ltabscf")
    if len(sc) != 121 or len(sl) != 121:
        sys.exit("the scalefactor book is not 121 entries")
    check(list(zip(sc, sl)), "scalefactor book")
    if sl[60] != 1 or sc[60] != 0:
        sys.exit("the scalefactor book's middle (no change) should be the one-bit code 0")

    long_off = array_body(src, "sfBandTabLongOffset")
    long_tab = array_body(src, "sfBandTabLong")
    short_off = array_body(src, "sfBandTabShortOffset")
    short_tab = array_body(src, "sfBandTabShort")

    def bands(tab, offs, i, top):
        o = offs[i]
        edges = [tab[o]]
        k = o + 1
        while edges[-1] < top:
            edges.append(tab[k])
            k += 1
        if edges[0] != 0 or edges[-1] != top:
            sys.exit("band table %d does not run from 0 to %d" % (i, top))
        return edges

    longs = [bands(long_tab, long_off, i, 1024) for i in range(12)]
    shorts = [bands(short_tab, short_off, i, 128) for i in range(12)]

    o = open(out_path, "w")
    w = o.write
    w("/* aactab.h -- made by tools/mkaactab.py from the VisualOn AAC encoder's\n")
    w(" * tables (Apache License 2.0); see that tool.  Do not edit by hand. */\n\n")
    w("/* The eleven spectral codebooks, one after another: book b's entries\n")
    w(" * are AAC_HCODE[AAC_HSTART[b] + i], i the codebook's own index. */\n")
    w("static const unsigned short AAC_HSTART[13] = { %s };\n" % ", ".join(str(v) for v in [0] + starts))
    w("static const unsigned short AAC_HCODE[%d] = {\n" % len(codes))
    for i in range(0, len(codes), 12):
        w("    " + ", ".join("0x%04x" % v for v in codes[i:i + 12]) + ",\n")
    w("};\n")
    w("static const unsigned char AAC_HLEN[%d] = {\n" % len(lens))
    for i in range(0, len(lens), 20):
        w("    " + ", ".join("%d" % v for v in lens[i:i + 20]) + ",\n")
    w("};\n\n")
    w("/* The scalefactor book: differences of -60 to +60, index 60 is none. */\n")
    w("static const unsigned int AAC_SFCODE[121] = {\n")
    for i in range(0, 121, 8):
        w("    " + ", ".join("0x%05x" % v for v in sc[i:i + 8]) + ",\n")
    w("};\n")
    w("static const unsigned char AAC_SFLEN[121] = {\n")
    for i in range(0, 121, 20):
        w("    " + ", ".join("%d" % v for v in sl[i:i + 20]) + ",\n")
    w("};\n\n")
    w("/* Where the scalefactor bands start, for each of the twelve sample\n")
    w(" * rates (96000 down to 8000), long windows then short; the last entry\n")
    w(" * of each is the window's length. */\n")
    w("static const unsigned short AAC_SWB_LONG[12][52] = {\n")
    for e in longs:
        w("    { " + ", ".join(str(v) for v in e) + " },\n")
    w("};\n")
    w("static const unsigned char AAC_NSWB_LONG[12] = { %s };\n" % ", ".join(str(len(e) - 1) for e in longs))
    w("static const unsigned char AAC_SWB_SHORT[12][16] = {\n")
    for e in shorts:
        w("    { " + ", ".join(str(v) for v in e) + " },\n")
    w("};\n")
    w("static const unsigned char AAC_NSWB_SHORT[12] = { %s };\n" % ", ".join(str(len(e) - 1) for e in shorts))
    o.close()
    print("wrote %s: %d spectral codes, 121 scalefactor codes, 12 sample rates" % (out_path, len(codes)))
    print("long bands per rate:", [len(e) - 1 for e in longs])
    print("short bands per rate:", [len(e) - 1 for e in shorts])
    print("book 1 all-zero length:", lens[40], " book 2:", lens[81 + 40])


main()
