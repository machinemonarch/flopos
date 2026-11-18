#!/usr/bin/env python3
"""Make src/apps/lib/audio/gen/opustab.h: the fixed tables Opus's CELT layer needs.

    tools/build/mkopustab.py OPUS_SOURCE_DIR [src/apps/lib/audio/gen/opustab.h]

OPUS_SOURCE_DIR is an unpacked libopus release (opus-1.4 from
https://downloads.xiph.org/releases/opus/).  Only tables are taken -- the
numbers the format fixes, which every decoder must share: the bands, the bit
allocation, the energy probability model, the pulse cache and a few small
ones.  BSD licence, Copyright Xiph.Org and others; see licenses/THIRD-PARTY.txt.
No code is taken: src/apps/lib/audio/opus.c is FLOPOS's own, written from
RFC 6716.
"""
import os, re, sys

def array(path, name):
    """The numbers in `name[...] = { ... };` in the file, flattened."""
    text = open(path).read()
    m = re.search(r"\b%s\s*(\[[^\]]*\])+\s*=\s*\{" % re.escape(name), text)
    if not m: sys.exit("FAILED: no %s in %s" % (name, path))
    i, depth = m.end() - 1, 0
    for j in range(i, len(text)):
        if text[j] == "{": depth += 1
        elif text[j] == "}":
            depth -= 1
            if not depth: break
    body = re.sub(r"/\*.*?\*/", " ", text[i:j + 1], flags=re.S)
    body = re.sub(r"QCONST16\(\s*([-0-9.]+)f?\s*,\s*15\s*\)", r"\1", body)
    return [float(x) if "." in x else int(x) for x in re.findall(r"-?\d+(?:\.\d+)?", body)]

def main():
    src = sys.argv[1]
    out_path = sys.argv[2] if len(sys.argv) > 2 else "src/apps/lib/audio/gen/opustab.h"
    celt = lambda f: os.path.join(src, "celt", f)
    t = {
        "eband5ms": array(celt("modes.c"), "eband5ms"),
        "band_allocation": array(celt("modes.c"), "band_allocation"),
        "logN400": array(celt("static_modes_float.h"), "logN400"),
        "cache_index50": array(celt("static_modes_float.h"), "cache_index50"),
        "cache_bits50": array(celt("static_modes_float.h"), "cache_bits50"),
        "cache_caps50": array(celt("static_modes_float.h"), "cache_caps50"),
        "e_prob_model": array(celt("quant_bands.c"), "e_prob_model"),
        "tf_select_table": array(celt("celt.c"), "tf_select_table"),
        "LOG2_FRAC_TABLE": array(celt("rate.c"), "LOG2_FRAC_TABLE"),
        "gains": array(celt("celt.c"), "gains"),
        "trim_icdf": array(celt("celt.h"), "trim_icdf"),
        "spread_icdf": array(celt("celt.h"), "spread_icdf"),
        "tapset_icdf": array(celt("celt.h"), "tapset_icdf"),
    }
    # eMeans: the fixed-point build's Q4 integers (the float build's are these / 16)
    em = open(celt("quant_bands.c")).read()
    em = em[em.index("const signed char eMeans"):]
    t["eMeans"] = [int(x) for x in re.findall(r"-?\d+", em[em.index("{"):em.index("}")])]
    sizes = {"eband5ms": 22, "band_allocation": 231, "logN400": 21, "cache_index50": 105, "cache_bits50": 392,
             "cache_caps50": 168, "e_prob_model": 336, "tf_select_table": 32, "LOG2_FRAC_TABLE": 24, "gains": 9,
             "trim_icdf": 11, "spread_icdf": 4, "tapset_icdf": 3, "eMeans": 25}
    for k, n in sizes.items():
        if len(t[k]) != n: sys.exit("FAILED: %s has %d numbers, not %d" % (k, len(t[k]), n))

    out = []
    w = out.append
    w("/* opustab.h -- made by tools/build/mkopustab.py from libopus 1.4's tables\n")
    w(" * (BSD licence, Copyright 2001-2011 Xiph.Org, Skype Limited, Octasic,\n")
    w(" * Jean-Marc Valin, Timothy B. Terriberry, CSIRO, Gregory Maxwell, Mark\n")
    w(" * Borgerding, Erik de Castro Lopo; see licenses/THIRD-PARTY.txt).  Only the\n")
    w(" * numbers RFC 6716 fixes; do not edit by hand. */\n\n")

    def emit(ctype, name, vals, comment, per=16, fmt="%d"):
        w("/* %s */\n" % comment)
        w("static const %s %s[%d] = {\n" % (ctype, name, len(vals)))
        for i in range(0, len(vals), per):
            w("    " + ", ".join(fmt % v for v in vals[i:i + per]) + ",\n")
        w("};\n\n")

    emit("unsigned char", "OPUS_EBANDS", t["eband5ms"], "Where the 21 bands start, in units of 200 Hz (of 2.5 ms of 48 kHz): 22 edges")
    emit("unsigned char", "OPUS_ALLOC", t["band_allocation"], "Bits a sample, in 1/32 bit, for 11 quality levels x 21 bands", 21)
    emit("unsigned char", "OPUS_LOGN", t["logN400"], "log2 of each band's width, in 1/8 bit")
    emit("short", "OPUS_CACHE_INDEX", t["cache_index50"], "The pulse cache: where each band and frame size starts in OPUS_CACHE_BITS (5 sizes x 21 bands)", 21)
    emit("unsigned char", "OPUS_CACHE_BITS", t["cache_bits50"], "The pulse cache: bits (1/8) needed for each number of pulses")
    emit("unsigned char", "OPUS_CACHE_CAPS", t["cache_caps50"], "The most bits a band can use, by frame size, mono or stereo, and band", 21)
    emit("unsigned char", "OPUS_EPROB", t["e_prob_model"], "Coarse energy's Laplace model: frame size x (inter, intra) x 21 bands x (probability, decay)", 42)
    emit("signed char", "OPUS_TF_SELECT", t["tf_select_table"], "Time/frequency changes: frame size x (transient, select, flag)", 8)
    emit("unsigned char", "OPUS_LOG2_FRAC", t["LOG2_FRAC_TABLE"], "log2 of 1..24, in 1/8 bit, rounded (the intensity stereo reservation)", 24)
    emit("float", "OPUS_COMB_GAINS", t["gains"], "The pitch post-filter's three tap sets", 3, "%.10ff")
    emit("unsigned char", "OPUS_TRIM_ICDF", t["trim_icdf"], "The allocation trim")
    emit("unsigned char", "OPUS_SPREAD_ICDF", t["spread_icdf"], "Spreading")
    emit("unsigned char", "OPUS_TAPSET_ICDF", t["tapset_icdf"], "The post-filter's tap set")
    emit("signed char", "OPUS_EMEANS", t["eMeans"], "Each band's mean energy, in 1/16 of log2", 25)
    open(out_path, "w").write("".join(out))
    print("   %s: %d tables" % (out_path, len(sizes)))

if __name__ == "__main__":
    main()
