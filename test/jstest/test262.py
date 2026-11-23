#!/usr/bin/env python3
"""The script room against test262, the official JavaScript test suite.

    test/jstest/test262.py [--only PATH-PART] [--jobs N]

test262 is fetched once into ~/.cache/flopos-test262 (git, shallow).  The
engine is built for this PC without sanitizers (build/jstest/t262, fast),
and the tests run in batches on every core.  Each test gets the suite's
harness (assert.js, sta.js, its own includes), in strict mode only when it
must be.  Left out: intl402 (no Intl in the room) and staging.
Writes build/test262/report.txt (pass rates by feature, worst first) and
results.json (every test: passed or why not); prints the totals.
"""
import json, os, re, subprocess, sys, tempfile, threading
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SUITE = os.path.expanduser("~/.cache/flopos-test262")
OUT = os.path.join(ROOT, "build", "test262")
BIN = os.path.join(ROOT, "build", "jstest", "t262")
BATCH = 60


def build():
    src = [os.path.join(ROOT, "src", "room", f) for f in sorted(os.listdir(os.path.join(ROOT, "src", "room")))
           if f.endswith(".c") and f not in ("room.c", "dom.c")]
    os.makedirs(os.path.dirname(BIN), exist_ok=True)
    subprocess.run(["gcc", "-O2", "-w", "-o", BIN, os.path.join(ROOT, "test", "jstest", "host.c")] + src + ["-lm"], check=True)


def meta(text):
    m = re.search(r"/\*---(.*?)---\*/", text, re.S)
    y = m.group(1) if m else ""
    def lst(key):
        m = re.search(r"^\s*%s:\s*\[(.*?)\]" % key, y, re.M)
        if m: return [x.strip().strip("'\"") for x in m.group(1).split(",") if x.strip()]
        m = re.search(r"^\s*%s:\s*\n((?:\s+-\s*.*\n?)+)" % key, y, re.M)
        return [l.strip()[1:].strip().strip("'\"") for l in m.group(1).splitlines() if l.strip()] if m else []
    neg = re.search(r"negative:\s*\n\s*phase:\s*(\w+)\s*\n\s*type:\s*(\w+)", y)
    return {"includes": lst("includes"), "flags": lst("flags"), "features": lst("features"),
            "negative": (neg.group(1), neg.group(2)) if neg else None}


def tests(only):
    for top in ("built-ins", "language", "annexB"):
        for d, _, fs in os.walk(os.path.join(SUITE, "test", top)):
            for f in sorted(fs):
                if f.endswith(".js") and "_FIXTURE" not in f:
                    p = os.path.join(d, f)
                    if not only or any(o in p for o in only.split(",")): yield p


def compose(path, tmp, harness):
    text = open(path, encoding="utf-8", errors="replace").read()
    m = meta(text)
    flags = m["flags"]
    if "raw" in flags: body = text
    else:
        inc = ["assert.js", "sta.js"] + (["doneprintHandle.js"] if "async" in flags else []) + m["includes"]
        body = "".join(harness.get(i, "") + "\n" for i in inc) + text
    if "onlyStrict" in flags: body = '"use strict";\n' + body
    if "module" in flags:                       # beside the test, so its fixtures are found
        run = path[:-3] + ".flopos-t262.mjs"
    else:
        run = os.path.join(tmp, "%d.js" % abs(hash(path)))
    open(run, "w", encoding="utf-8").write(body)
    return run, m


def judge(m, r, err, asyn):
    neg = m["negative"]
    if neg: return r != 0 and neg[1] in err, "should stop with %s; %s" % (neg[1], err or "it ran")
    if "async" in m["flags"]: return r == 0 and asyn == "complete", err or "async: %s" % (asyn or "never finished")
    return r == 0, err


def main():
    args = sys.argv[1:]
    only = args[args.index("--only") + 1] if "--only" in args else None
    jobs = int(args[args.index("--jobs") + 1]) if "--jobs" in args else os.cpu_count() or 2
    if not os.path.isdir(SUITE):
        subprocess.run(["git", "clone", "-q", "--depth", "1", "https://github.com/tc39/test262", SUITE], check=True)
    global BIN
    if os.environ.get("FLOPOS_T262_BIN"): BIN = os.environ["FLOPOS_T262_BIN"]   # e.g. the memory-checked build/jstest/jstest
    else: build()
    harness = {f: open(os.path.join(SUITE, "harness", f), encoding="utf-8").read() for f in os.listdir(os.path.join(SUITE, "harness")) if f.endswith(".js")}
    os.makedirs(OUT, exist_ok=True)
    if os.path.exists(os.path.join(OUT, "memory.txt")): os.remove(os.path.join(OUT, "memory.txt"))
    tmp = tempfile.mkdtemp(prefix="flopos-t262.")
    todo = list(tests(only))
    results, lock = {}, threading.Lock()
    batches = [todo[i:i + BATCH] for i in range(0, len(todo), BATCH)]

    def run_batch(paths):
        made = [compose(p, tmp, harness) for p in paths]
        by_run = {run: (p, m) for p, (run, m) in zip(paths, made)}
        try:
            rr = subprocess.run([BIN, "-q", "-t262"] + [r for r, _ in made], capture_output=True, text=True,
                                errors="replace", timeout=60 + 5 * len(paths))
            out = rr.stdout
            if "AddressSanitizer" in rr.stderr or "runtime error" in rr.stderr:   # the memory checker's finds, kept
                with lock: open(os.path.join(OUT, "memory.txt"), "a").write(rr.stderr[-4000:] + "\n----\n")
        except subprocess.TimeoutExpired as e:
            out = (e.stdout or b"").decode("utf-8", "replace") if isinstance(e.stdout, bytes) else (e.stdout or "")
        got = {}
        for line in out.splitlines():
            if line.startswith("T262\t"):
                _, run, r, err, asyn = (line.split("\t") + ["", ""])[:5]
                got[run] = (int(r), err, asyn)
        missing = [run for run, _ in made if run not in got]
        if missing and len(paths) > 1:           # a crash or a hang took the batch: each alone
            for run in missing: run_batch([by_run[run][0]])
            missing = []
        with lock:
            for run, (p, m) in by_run.items():
                if run in got:
                    ok, why = judge(m, *got[run])
                    results[p] = {"ok": ok, "why": "" if ok else why[:200], "features": m["features"]}
                elif run in missing:
                    results[p] = {"ok": False, "why": "crashed or hung the engine", "features": m["features"]}
        for run, _ in made:
            if run.endswith(".flopos-t262.mjs"):
                try: os.remove(run)
                except OSError: pass

    def worker(mine):
        for b in mine: run_batch(b)
    th = [threading.Thread(target=worker, args=(batches[i::jobs],)) for i in range(jobs)]
    for t in th: t.start()
    for t in th: t.join()
    import shutil; shutil.rmtree(tmp, ignore_errors=True)

    # by feature: test262's own tags, or, untagged, the folder (built-ins/Array, language/statements/for-of)
    groups = defaultdict(lambda: [0, 0])
    for p, r in results.items():
        rel = os.path.relpath(p, os.path.join(SUITE, "test")).split(os.sep)
        keys = r["features"] or ["/".join(rel[:2] if rel[0] == "built-ins" else rel[:3])]
        for k in keys:
            groups[k][0] += r["ok"]; groups[k][1] += 1
    passed = sum(r["ok"] for r in results.values())
    lines = ["test262: %d of %d pass (%.1f%%)" % (passed, len(results), 100.0 * passed / max(1, len(results))), "",
             "%-48s %6s %6s %6s" % ("feature", "pass", "tests", "%")]
    for k, (ok, n) in sorted(groups.items(), key=lambda kv: (kv[1][0] / kv[1][1], -kv[1][1])):
        lines.append("%-48s %6d %6d %5.0f%%" % (k[:48], ok, n, 100.0 * ok / n))
    open(os.path.join(OUT, "report.txt"), "w").write("\n".join(lines) + "\n")
    json.dump({os.path.relpath(p, SUITE): r for p, r in results.items()}, open(os.path.join(OUT, "results.json"), "w"))
    print(lines[0])
    if os.path.exists(os.path.join(OUT, "memory.txt")): print("FAILED  the memory checker found something: %s" % os.path.join(OUT, "memory.txt"))
    print("   by feature, worst first: %s" % os.path.join(OUT, "report.txt"))


if __name__ == "__main__":
    main()
