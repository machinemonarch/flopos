#!/usr/bin/env python3
"""Which web features browzer's scripts have, checked inside browzer.

    test/bench/webprobe.py [IMAGE] [--again]

From the web-features data (fetched once into ~/.cache/flopos-webfeatures:
every web feature, and whether it is Baseline -- "widely available" when
every major browser has had it for 2.5 years, "newly available" when all
have it), takes what a script can look for: interfaces and methods
(api.*) and JavaScript's built-ins (javascript.builtins.*).  A page on the
test site (/js/probe) looks for each of them in browzer's script room and
sends back what it found.  A feature counts as there when its interface is
and most of its members are.
Writes build/webprobe/report.txt: the features missing, ranked by how many
of the scoreboard's sites use them (their scripts, fetched by
test/sites/corpus.py into ~/.cache/flopos-corpus), with each one's
Baseline status and how many of its keys browzer lacks.
"""
import json, os, re, subprocess, sys, urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DATA = os.path.expanduser("~/.cache/flopos-webfeatures/data.json")
OUT = os.path.join(ROOT, "build", "webprobe")


def keys_of(f):
    out = []
    for k in f.get("compat_features") or []:
        p = k.split(".")
        if p[0] == "api" and len(p) <= 3 and "_" not in k: out.append(k)
        elif k.startswith("javascript.builtins.") and len(p) <= 4 and "_" not in k: out.append(k)
    return out


def main():
    img = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "dist", "flopos.img")
    if not os.path.exists(DATA):
        os.makedirs(os.path.dirname(DATA), exist_ok=True)
        urllib.request.urlretrieve("https://unpkg.com/web-features/data.json", DATA)
    feats = json.load(open(DATA))["features"]
    keys = sorted({k for f in feats.values() for k in keys_of(f)})
    os.makedirs(OUT, exist_ok=True)
    json.dump(keys, open(os.path.join(OUT, "keys.json"), "w"))
    got_path = os.path.join(OUT, "got.txt")
    if "--again" not in sys.argv:                 # --again: rank the last probe's answers, no emulator
        if os.path.exists(got_path): os.remove(got_path)
        r = subprocess.run([sys.executable, os.path.join(ROOT, "test", "bench", "run.py"), img, os.path.join(OUT, "run"),
                            "js/probe", r"key:j=>probe: (done|failed)"], capture_output=True, text=True)
        if not os.path.exists(got_path): sys.exit("FAILED  the probe sent nothing back\n" + r.stdout[-600:])
    got = open(got_path).read().strip()
    have = {k: got[i] == "1" for i, k in enumerate(keys) if i < len(got)}
    rows = []
    for fid, f in feats.items():
        ks = keys_of(f)
        if not ks: continue
        miss = [k for k in ks if not have.get(k)]
        if len(miss) * 2 <= len(ks) and have.get(ks[0], False): continue      # mostly there
        b = (f.get("status") or {}).get("baseline")
        rank = {"high": 0, "low": 1}.get(b, 2)
        rows.append((rank, -len(ks), fid, f.get("name", fid), len(miss), len(ks), b))
    # how many sites use each: its interfaces' names, and its members' longer names
    corpus = os.path.expanduser("~/.cache/flopos-corpus")
    names = {}
    for site in (os.listdir(corpus) if os.path.isdir(corpus) else []):
        seen = set()
        for f in os.listdir(os.path.join(corpus, site)):
            seen.update(re.findall(r"[A-Za-z_$][\w$]*", open(os.path.join(corpus, site, f), errors="replace").read()))
        if seen: names[site] = seen
    generic = {"Window", "Document", "Element", "HTMLElement", "Node", "EventTarget", "Event", "Object", "Array", "String",
               "Number", "Math", "Date", "RegExp", "Function", "Promise", "JSON", "Error", "Boolean"}
    def used(ks):
        """Sites naming one of its interfaces; a feature that only adds members
        to common ones (Element, Window) counts by its long member names."""
        ifaces, members = set(), set()
        for k in ks:
            p = k.split(".")
            iface, member = (p[2], p[3] if len(p) > 3 else None) if p[0] == "javascript" else (p[1], p[2] if len(p) > 2 else None)
            if iface not in generic: ifaces.add(iface)
            elif member and len(member) >= 10: members.add(member)
        toks = ifaces or members
        return sum(1 for seen in names.values() if toks & seen)
    label = {0: "widely available", 1: "newly available", 2: "not Baseline"}
    rows = [(-used([k for k in keys_of(feats[fid]) if not have.get(k)]), rank, fid, name, nm, nk) for rank, _, fid, name, nm, nk, b in rows]
    rows.sort()
    lines = ["Web features browzer's scripts lack: %d of the %d a script can look for." % (len(rows), sum(1 for f in feats.values() if keys_of(f))),
             "Ranked by how many of %d sites' scripts use them." % len(names), "",
             "%-34s %-18s %5s %s" % ("feature", "Baseline", "sites", "missing keys")]
    for u, rank, fid, name, nm, nk in rows:
        lines.append("%-34s %-18s %5d %d of %d" % (fid[:34], label[rank], -u, nm, nk))
    open(os.path.join(OUT, "report.txt"), "w").write("\n".join(lines) + "\n")
    print("ok  %d features missing, %d of them widely available: %s" % (len(rows), sum(r[1] == 0 for r in rows), os.path.join(OUT, "report.txt")))


if __name__ == "__main__":
    main()
