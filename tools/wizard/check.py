#!/usr/bin/env python3
"""check.py MODEL DIR [N] [-v] -- is the wizard fit to speak?

Has him say N lines (300) through speak.py, from the file FLOPOS will read,
and measures them against what he read in DIR/*.lines:

  size      the file fits its share of the floppy
  ends      lines that end by themselves, on a full stop, ! or ?
  words     words that are real (met in the reading)
  pairs     neighbouring words that were met together (a rough grammar)
  distinct  lines that differ from each other
  stutter   lines that repeat a word three times or a four-word run
  books     lines taken whole from Shakespeare or the Jargon File
  new       lines that are in none of the reading (most of the rest are
            lines of his own from voice.py, which he may repeat)

Prints ok or FAILED and what failed; -v prints every figure.
"""
import glob, os, re, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from speak import Wizard

LIMIT = 262144
NEED = {"ends": (">=", 0.95), "words": (">=", 0.98), "pairs": (">=", 0.90),
        "distinct": (">=", 0.95), "stutter": ("<=", 0.03),
        "books": ("<=", 0.05), "new": (">=", 0.30)}


def toks(l):
    return re.findall(r"[a-z']+|@", l.lower())


def main():
    args = [a for a in sys.argv[1:] if a != "-v"]
    model, d = args[0], args[1]
    n = int(args[2]) if len(args) > 2 else 300
    words, pairs, lines, books = set(), set(), set(), set()
    for f in glob.glob(os.path.join(d, "*.lines")):
        book = os.path.basename(f) in ("shakespeare.lines", "jargon.lines")
        for l in open(f).read().split("\n"):
            t = toks(l)
            words.update(t); pairs.update(zip(t, t[1:])); lines.add(l)
            if book:
                books.add(l)
    w = Wizard(model)
    rng = np.random.default_rng(7)
    said = [w.say(rng) for _ in range(n)]
    nw = np_ = okw = okp = stut = 0
    for l, _ in said:
        t = toks(l)
        nw += len(t); okw += sum(x in words for x in t)
        pp = list(zip(t, t[1:]))
        np_ += len(pp); okp += sum(x in pairs for x in pp)
        four = [tuple(t[i:i + 4]) for i in range(len(t) - 3)]
        stut += (any(a == b == c for a, b, c in zip(t, t[1:], t[2:]))
                 or len(four) != len(set(four)))
    got = {
        "ends": sum(e and l[-1:] in ".!?" for l, e in said) / n,
        "words": okw / max(nw, 1), "pairs": okp / max(np_, 1),
        "distinct": len({l for l, _ in said}) / n, "stutter": stut / n,
        "books": sum(l in books for l, _ in said) / n,
        "new": sum(l not in lines for l, _ in said) / n,
    }
    size = os.path.getsize(model)
    bad = ["size %d" % size] if size > LIMIT else []
    for k, (op, v) in NEED.items():
        if (got[k] < v) if op == ">=" else (got[k] > v):
            bad.append("%s %.2f" % (k, got[k]))
    if "-v" in sys.argv:
        print("size %d" % size)
        for k, v in got.items():
            print("%-8s %.3f (%s %.2f)" % (k, v, *NEED[k]))
    print("ok" if not bad else "FAILED: " + ", ".join(bad))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
