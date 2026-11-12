#!/usr/bin/env python3
"""corpus.py DIR -- fetch and clean the wizard's reading.

Two public-domain texts from Project Gutenberg, kept in DIR (outside the
repository, like the test songs): the complete Shakespeare (#100) and the
Jargon File 4.2.2 (#3008).  Each becomes a file of single sentences, one a
line, in plain ASCII: DIR/shakespeare.lines and DIR/jargon.lines.

Prints ok or FAILED.
"""
import os, re, sys, urllib.request
from collections import Counter

SRC = {
    "shakespeare": "https://www.gutenberg.org/cache/epub/100/pg100.txt",
    "jargon":      "https://www.gutenberg.org/cache/epub/3008/pg3008.txt",
}
LO, HI = 25, 110                       # a sentence's length, in characters
OKCH = set("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
           " .,;:!?'-")
FIX = {"’": "'", "‘": "'", "“": '', "”": '',
       "—": ", ", "–": "-", "æ": "ae", "Æ": "Ae",
       "œ": "oe", "è": "e", "é": "e", "à": "a",
       "â": "a", "ê": "e", "ç": "c", "ï": "i", '"': ''}


def body(path):
    t = open(path, encoding="utf-8").read().replace("\r", "")
    a = t.index("*** START OF")
    a = t.index("\n", a) + 1
    return t[a:t.index("*** END OF")]


def plain(s):
    for k, v in FIX.items():
        s = s.replace(k, v)
    s = re.sub(r"\s+", " ", s).strip()
    s = re.sub(r" ([,.;:!?])", r"\1", s)
    return s


def sentences(par):
    for s in re.split(r"(?<=[.!?]) +(?=[A-Z])", par):
        s = s.strip()
        if not (LO <= len(s) <= HI):          continue
        if not s[0].isupper():                continue
        if s[-1] not in ".!?":                continue
        if set(s) - OKCH:                     continue
        if re.search(r"\b[A-Z]{2,}\b", s):    continue   # names, acronyms
        if len(s.split()) < 5:                continue
        yield s


def shakespeare(text):
    lines = text.split("\n")
    # Verse begins every line with a capital.  A word is put back in small
    # letters when it is mostly written so in the middle of lines.
    low, cap = Counter(), Counter()
    for ln in lines:
        for w in re.findall(r"[A-Za-z']+", ln)[1:]:
            (cap if w[0].isupper() else low)[w.lower()] += 1
    out, par, open_ = [], [], False
    def flush():
        if par:
            out.extend(sentences(plain(" ".join(par))))
            par.clear()
    for ln in lines:
        s = ln.strip()
        if (not s or s.startswith("[") or s.endswith("]") or "_" in s
                or re.fullmatch(r"[A-Z][A-Z .,'&-]*\.?", s)
                or re.match(r"(ACT|SCENE|Enter|Exit|Exeunt|Re-enter)\b", s)):
            flush(); open_ = False
            continue
        if open_:
            m = re.match(r"[A-Za-z']+", s)
            if m and m.group() != "I" and not m.group().startswith("I'"):
                w = m.group().lower()
                if low[w] > cap[w]:
                    s = s[0].lower() + s[1:]
        par.append(s)
        open_ = s[-1] not in ".!?"
    flush()
    return out


def jargon(text):
    text = text[text.index("Node:"):]
    out = []
    for par in re.split(r"\n\s*\n", text):
        if "Node:" in par or "http" in par or "@" in par:
            continue
        p = " ".join(par.split("\n"))
        if re.search(r"/ ?(n|v|vi|vt|adj|adv|excl|pref|suff|interj)\b", p[:80]):
            continue                              # the headword line
        if re.match(r"\S+( \S+){0,3} (n|v|vi|vt|adj|adv|excl)\.", p):
            continue
        p = re.sub(r"\[[^\]]*\]", "", p)
        p = re.sub(r"\([^)]*\)", "", p)
        p = re.sub(r"\b\d+\. ", "", p)
        p = p.replace("_", "").replace("`", "'")
        out.extend(sentences(plain(p)))
    return out


def main():
    d = sys.argv[1]
    os.makedirs(d, exist_ok=True)
    ok = True
    for name, url in SRC.items():
        raw = os.path.join(d, name + ".txt")
        if not os.path.exists(raw):
            urllib.request.urlretrieve(url, raw)
        got = (shakespeare if name == "shakespeare" else jargon)(body(raw))
        got = list(dict.fromkeys(got))
        open(os.path.join(d, name + ".lines"), "w").write("\n".join(got) + "\n")
        ok &= len(got) > 3000
    print("ok" if ok else "FAILED")


if __name__ == "__main__":
    main()
