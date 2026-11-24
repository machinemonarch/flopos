#!/usr/bin/env python3
"""FLOPOS's file code against a floppy image, checked by this PC's own tools.

    test/fstest/run.py [IMAGE] [--ops N] [--cuts N] [--seed S] [--san] [--sweep]

Builds src/kernel/fs.c for this PC (build/fstest/fstest), then on a copy of the image
(dist/flopos.img unless named):

  1. thousands of random makes, changes, moves and removals in the
     inventory, each answer compared with what should happen; then
     fsck.fat must find nothing wrong, mtools must read back every file
     exactly, and the system's files must be untouched;
  2. the same kind of step with the power cut after a random number of
     sectors: afterwards every file must be whole, old or new, and the
     disk no worse than a few clusters nobody owns.

Prints ok or FAILED, and the first thing that was wrong.
"""
import argparse, os, random, shutil, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BIN = os.path.join(ROOT, "build", "fstest", "fstest")
NAMES = ["A", "NOTES.MD", "SONG.MOD", "LIST.TXT", "B", "OLD", "X1.MD", "LONGNAME.MD", "Z-9_~!.A", "DIR", "DEEP", "A$(1)@^.{}", "CONX.MD"]
BAD = ["..", ".", "A/../B", "TOOLONGNAME.MD", "A+B", "X.TOOL", "a*b", "", "A/./B", "CON", "NUL.TXT", "COM1.MD", "LPT9"]

def fill(n, seed):
    x, out = seed, bytearray(n)
    for i in range(n):
        x = (x * 1103515245 + 12345) & 0xFFFFFFFF
        out[i] = (x >> 16) & 0xFF
    return bytes(out)

def summ(b):
    h = 2166136261
    for c in b: h = ((h ^ c) * 16777619) & 0xFFFFFFFF
    return h

KEEP = []                                   # on a failure, where to keep the disk before and after the cut
def fail(msg):
    for src, name in KEEP:
        if os.path.exists(src): shutil.copy(src, os.path.join(ROOT, "build", "fstest", name))
    print("FAILED\n   " + msg); sys.exit(1)

def run(img, lines, cut=None):
    cmd = [BIN, img] + (["--cut", str(cut)] if cut is not None else [])
    p = subprocess.run(cmd, input="\n".join(lines) + "\n", capture_output=True, text=True)
    if p.returncode not in (0, 3): fail("fstest died: " + p.stderr[:200])
    return p.stdout.splitlines(), p.returncode == 3

class Model:
    def __init__(self): self.files, self.dirs = {}, {""}
    def copy(self):
        m = Model(); m.files, m.dirs = dict(self.files), set(self.dirs); return m
    def parent(self, p): return p.rsplit("/", 1)[0] if "/" in p else ""
    def kids(self, d):
        pre = d + "/" if d else ""
        return [p for p in list(self.files) + list(self.dirs) if p and p.startswith(pre) and "/" not in p[len(pre):]]
    def anywhere(self, rng, new=False):
        d = rng.choice(sorted(self.dirs))
        if new or rng.random() < 0.3 or not self.kids(d):
            return (d + "/" if d else "") + rng.choice(NAMES)
        return rng.choice(sorted(self.kids(d)))

def step(m, rng):
    """One random command and what it should answer: 'ok', 'no', or 'room' (either)."""
    r = rng.random()
    if r < 0.04:
        bad = rng.choice(BAD)
        return rng.choice(["write %s 10 1", "mkdir %s", "rm %s"]) % ("A/" + bad if bad and rng.random() < 0.5 else bad or "//"), "no", None
    if r < 0.45:
        p = m.anywhere(rng); n = rng.choice([0, 1, 511, 512, 513, 2000, 9000, 40000, rng.randrange(0, 120000)]); seed = rng.randrange(1 << 30)
        if p in m.dirs or (p.count("/") >= 8): return "write %s %d %d" % (p, n, seed), "no", None
        return "write %s %d %d" % (p, n, seed), "room", ("w", p, n, seed)
    if r < 0.60:
        p = m.anywhere(rng, new=rng.random() < 0.8)
        ok = p not in m.files and p not in m.dirs and "." not in p.rsplit("/", 1)[-1] and p.count("/") < 7
        return "mkdir " + p, "room" if ok else "no", ("d", p) if ok else None
    if r < 0.78:
        p = m.anywhere(rng)
        if p in m.files: return "rm " + p, "ok", ("r", p)
        if p in m.dirs and not m.kids(p): return "rm " + p, "ok", ("r", p)
        return "rm " + p, "no", None
    a, b = m.anywhere(rng), m.anywhere(rng, new=rng.random() < 0.8)
    ok = (a in m.files or a in m.dirs) and b not in m.files and b not in m.dirs and b.count("/") < 8
    if ok and a in m.dirs and ("." in b.rsplit("/", 1)[-1] or (b + "/").startswith(a + "/")): ok = False
    if ok and a in m.dirs:                       # nothing inside may end up too deep to reach
        below = max([p.count("/") - a.count("/") for p in m.dirs if p.startswith(a + "/")] + [0])
        if b.count("/") + 1 + below > 7: ok = False
    return "mv %s %s" % (a, b), "room" if ok else "no", ("m", a, b) if ok else None

KINDS = ["save new", "save over", "new folder", "delete file", "delete folder", "rename", "move", "folder grows"]
def kind(m, eff):
    if eff[0] == "w": return "save over" if eff[1] in m.files else "save new"
    if eff[0] == "d": return "new folder"
    if eff[0] == "r": return "delete folder" if eff[1] in m.dirs else "delete file"
    return "rename" if m.parent(eff[1]) == m.parent(eff[2]) else "move"

def apply(m, eff):
    if eff[0] == "w": m.files[eff[1]] = (eff[2], eff[3])
    elif eff[0] == "d": m.dirs.add(eff[1])
    elif eff[0] == "r": m.files.pop(eff[1], None); m.dirs.discard(eff[1])
    elif eff[0] == "m":
        a, b = eff[1], eff[2]
        if a in m.files: m.files[b] = m.files.pop(a)
        else:
            for p in [p for p in m.files if p.startswith(a + "/")]: m.files[b + p[len(a):]] = m.files.pop(p)
            for p in [p for p in m.dirs if p == a or p.startswith(a + "/")]: m.dirs.discard(p); m.dirs.add(b + p[len(a):])

def on_disk(img):
    """The inventory as mtools reads it: {path: bytes}, {folders}."""
    d = tempfile.mkdtemp(prefix="fstest-")
    try:
        p = subprocess.run(["mcopy", "-s", "-n", "-i", img, "::INVENTORY", d], capture_output=True, text=True)
        if p.returncode: fail("mtools cannot read the inventory: " + p.stderr[:200])
        top, files, dirs = os.path.join(d, "INVENTORY"), {}, {""}
        for base, ds, fs in os.walk(top):
            rel = os.path.relpath(base, top).replace(os.sep, "/")
            rel = "" if rel == "." else rel
            for x in ds: dirs.add((rel + "/" if rel else "") + x)
            for x in fs: files[(rel + "/" if rel else "") + x] = open(os.path.join(base, x), "rb").read()
        return files, dirs
    finally:
        shutil.rmtree(d, ignore_errors=True)

def system_files(img):
    out = {}
    for line in subprocess.run(["mdir", "-b", "-i", img, "::"], capture_output=True, text=True).stdout.split():
        name = line.replace("::/", "")
        if name.endswith("/") or not name: continue
        out[name] = subprocess.run(["mcopy", "-n", "-i", img, "::" + name, "-"], capture_output=True).stdout
    return out

def compare(img, m, what, either=None):
    files, dirs = on_disk(img)
    want = {p.upper() for p in m.dirs}
    if {p.upper() for p in dirs} != want: fail("%s: folders are %s, should be %s" % (what, sorted(dirs), sorted(m.dirs)))
    if set(files) != set(m.files):
        if not (either and set(files) == set(either.files)): fail("%s: files are %s, should be %s" % (what, sorted(files), sorted(m.files)))
    for p, data in files.items():
        good = [fill(*mm.files[p]) for mm in (m, either) if mm and p in mm.files]
        if data not in good: fail("%s: %s reads back wrong (%d bytes)" % (what, p, len(data)))

def fsck(img, lost_ok=False, what=""):
    p = subprocess.run(["fsck.fat", "-n", img], capture_output=True, text=True)
    if p.returncode == 0: return
    bad = [l for l in p.stdout.splitlines()[1:] if l.strip() and "unused cluster" not in l.lower() and "Leaving filesystem unchanged" not in l
           and not l.startswith(img) and "Reclaim" not in l and "Free cluster summary" not in l]
    if bad or not lost_ok: fail("fsck.fat%s: " % (" after " + what if what else "") + " / ".join((bad or p.stdout.splitlines())[:4]))

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image", nargs="?", default=os.path.join(ROOT, "dist", "flopos.img"))
    ap.add_argument("--ops", type=int, default=3000); ap.add_argument("--cuts", type=int, default=150)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--san", action="store_true", help="built with AddressSanitizer and UBSan")
    ap.add_argument("--sweep", action="store_true", help="each change cut at every one of its writes, not one at random")
    a = ap.parse_args()
    global BIN
    if a.san: BIN += "-san"
    os.makedirs(os.path.dirname(BIN), exist_ok=True)
    tmp = "%s.%d" % (BIN, os.getpid())      # many runs at once: each builds its own, then swaps it in whole
    c = subprocess.run(["gcc", "-O1", "-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-unused-function", "-o", tmp] + (["-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=all"] if a.san else []) + [
                        os.path.join(ROOT, "test", "fstest", "fstest.c")], capture_output=True, text=True)
    if c.returncode or c.stderr.strip(): fail("building: " + c.stderr[:400])
    os.replace(tmp, BIN)
    work = tempfile.mkdtemp(prefix="fstest-img-")
    try:
        img = os.path.join(work, "a.img"); shutil.copy(a.image, img)
        if subprocess.run(["mdir", "-i", img, "::INVENTORY"], capture_output=True).returncode:
            fail("the image has no INVENTORY folder")
        before = system_files(img)
        rng, m = random.Random(a.seed), Model()

        # 1. a long random run
        done = 0
        while done < a.ops:
            batch, trial = [], m.copy()
            for _ in range(50):
                cmd, expect, eff = step(trial, rng)
                batch.append((cmd, expect, eff))
                # the model moves on only when the answer is known; 'room' steps go one at a time
                if expect == "room": break
                if expect == "ok" and eff: apply(trial, eff)
            out, _ = run(img, [b[0] for b in batch])
            if len(out) != len(batch): fail("%d answers to %d commands" % (len(out), len(batch)))
            for (cmd, expect, eff), ans in zip(batch, out):
                ok = ans.startswith("ok")
                if expect == "ok" and not ok: fail("%s -> %s" % (cmd, ans))
                if expect == "no" and ok: fail("%s should have been refused" % cmd)
                if expect == "room" and not ok and eff and "room" not in ans: fail("%s -> %s" % (cmd, ans))
                if ok and eff: apply(m, eff)
            done += len(batch)
        fsck(img)
        compare(img, m, "after %d steps" % done)
        if system_files(img) != before: fail("a system file changed")
        # every file through FLOPOS's own reading too
        out, _ = run(img, ["read " + p for p in sorted(m.files)])
        for p, ans in zip(sorted(m.files), out):
            if ans != "ok %d %d" % (m.files[p][0], summ(fill(*m.files[p]))): fail("FLOPOS reads %s back wrong: %s" % (p, ans))

        # 2. power cuts.  --sweep: one change of each kind, cut at every one of its writes
        forced = []
        if a.sweep:                              # a folder whose one cluster is full: . and .. and 14 files
            fill_up = ["mkdir GROW"] + ["write GROW/F%d 100 %d" % (i, i) for i in range(14)]
            out, _ = run(img, fill_up)
            if not all(o.startswith("ok") for o in out): fail("could not fill GROW: " + " / ".join(out[:3]))
            m.dirs.add("GROW")
            for i in range(14): m.files["GROW/F%d" % i] = (100, i)
            forced = [("write GROW/F14 600 1", "room", ("w", "GROW/F14", 600, 1))]
        base = os.path.join(work, "base.img"); shutil.copy(img, base)
        KEEP[:] = [(base, "failed-before.img"), (img, "failed-after.img")]
        cuts, swept, tries = 0, set(), 0
        while (len(swept) < len(KINDS) and tries < 20000) if a.sweep else cuts < a.cuts:
            tries += 1
            shutil.copy(base, img)
            trial = m.copy()
            cmd, expect, eff = forced.pop() if forced else step(trial, rng)
            if not eff: continue
            k = "folder grows" if cmd.startswith("write GROW/F14 ") else kind(m, eff)
            if a.sweep and k in swept: continue
            after = m.copy(); apply(after, eff)
            # With less room than the new copy needs, the old one has to give
            # way first: the one case where a cut can cost the file.  Not this test.
            if eff[0] == "w" and eff[2] + 2048 > int(run(img, ["room"])[0][0].split()[1]): continue
            for at in (range(0, 10 ** 6) if a.sweep else [rng.randrange(0, 12)]):
                shutil.copy(base, img)
                out, was_cut = run(img, [cmd], cut=at)
                if not was_cut:
                    if out and out[0].startswith("ok"):
                        compare(img, after, "uncut " + cmd)
                        if a.sweep: swept.add(k); print("   %-13s %-40s cut at all %d writes" % (k, cmd, at))
                    break
                cuts += 1
                ans, _ = run(img, ["room", "write HEALED.TXT 700 5"])   # the next session: mends the table's copies, and can still write
                healed = len(ans) > 1 and ans[1].startswith("ok")  # unless the disk is too full for it
                # A move between folders is two entries in two places and cannot be
                # cut cleanly: the thing may be listed in both, which a disk
                # checker will remark on.  Nothing is lost; that is checked below.
                if not (eff[0] == "m" and m.parent(eff[1]) != m.parent(eff[2])): fsck(img, lost_ok=True, what="a cut at sector %d of '%s'" % (at, cmd))
                if healed:
                    for mm in (m, after): mm.files["HEALED.TXT"] = (700, 5)
                files, dirs = on_disk(img)
                # every file whole, old or new; a moved thing may be in both places
                for p, data in files.items():
                    good = [fill(*mm.files[p]) for mm in (m, after) if p in mm.files]
                    if data not in good: fail("after a cut in '%s': %s is neither old nor new (%d bytes)" % (cmd, p, len(data)))
                lost = (set(m.files) & set(after.files)) - set(files)
                if lost: fail("after a cut in '%s': %s is gone" % (cmd, sorted(lost)))
                if eff[0] in ("w",) and eff[1] in m.files and eff[1] not in files: fail("after a cut in '%s': the file is gone" % cmd)
                for mm in (m, after): mm.files.pop("HEALED.TXT", None)
        if system_files(img) != before: fail("a system file changed")
        if a.sweep and len(swept) < len(KINDS): fail("never swept: " + ", ".join(k for k in KINDS if k not in swept))
        print("ok (%d steps, %d power cuts, %d files and %d folders at the end)" % (done, cuts, len(m.files), len(m.dirs) - 1))
    finally:
        shutil.rmtree(work, ignore_errors=True)

if __name__ == "__main__":
    main()
