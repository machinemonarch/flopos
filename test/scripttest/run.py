#!/usr/bin/env python3
"""Scripts and styles in browzer, tried for real in the emulator.

    test/scripttest/run.py [IMAGE]

On a scratch copy of the image (dist/flopos.img unless named), against the
test site's pages (test/bench/site.py, /js/... and /css/basic): scripts
are off until the site is allowed with j; then a page's own scripts run,
another site's are not fetched, clicks, fetches and timers work, the
tripwires stop a script that fetches where it may not, a form may point
anywhere, a script that never ends is stopped, and styles hide what they should.  browzer
says what each page shows on the serial wire (for this test site only),
and nothing it must never show may be there.  Prints ok or FAILED.

The steps run in pieces, each on its own machine, all at once: a piece
starts at each page, and a page of scripts (js/) is opened, then allowed
with j -- but the first, which tries them off.  The image is a copy
that also trusts the test site's https (test/bench/testca.py).
"""
import os, shutil, socket, subprocess, sys, tempfile, threading, time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
STEPS = [
    r"js/basic=>none of them run",                         # off until allowed
    r"key:j=>ran app\.js(.|\n)*not fetched, another site(.|\n)*\[shown\] \* item three",
    r"js/click=>Scripts: 1 ran",
    r"key:tab",
    r"key:ret=>\[shown\] clicked 1",
    r"js/fetch=>\[shown\] \* fetched beta",
    r"js/timer=>\[shown\] tick 3",
    r"js/elsewhere=>\[shown\] fetching",
    r"js/cors=>\[shown\] a: allowed yes, cookies none, from http://10\.0\.2\.2:8150(.|\n)*\[shown\] b: refused(.|\n)*\[shown\] c: put hi",
    r"http://10.0.2.2.nip.io:8150/js/cors-local",
    r"key:j=>refused: a fetch into the local network(.|\n)*local refused",
    r"js/form=>\[shown\] form points elsewhere",
    r"js/loop=>stopped: it ran out of time",
    r"css/basic=>\[shown\] +Centred title(.|\n)*\[shown\] The end\.",
    r"radio/player=>\[shown\] Listen Live\r?\n\[shown\] \[listen\](.|\n)*sound it names: https://example\.test/listen/main/radio\.mp3",
    r"js/pieces=>\[shown\] pieces: aborted signal instanceof a=1&b=y string 2 clone utf8 any",
    r"js/async=>\[shown\] \* async alpha\r?\n\[shown\] \* async beta",
    r"key:tab",
    r"key:ret=>\[shown\] clicked, after a wait",
    r"js/modules=>\[shown\] modules: abab 1 string(.|\n)*\[shown\] inline abab, late(.|\n)*\[shown\] current script",
    r"js/big=>\[shown\] big 2600(.|\n)*\[shown\] after the huge one: undefined",
    r"https://10.0.2.2.nip.io:8151/https/form=>\[page\] 200, \d+ links: https form",
    r"key:tab",
    r"key:tab",
    r"key:ret=>asks first: this form would go unencrypted",
    r"key:y=>\[page\] 200, \d+ links: collected",
    r"https://10.0.2.2.nip.io:8151/https/form=>\[page\] 200, \d+ links: https form",
    r"key:tab",
    r"key:tab",
    r"key:tab",
    r"key:tab",
    r"key:ret=>\A(?![\s\S]*asks first)[\s\S]*links: collected",
    r"js/bigfetch=>\[shown\] bigfetch 3000 item 02999 yes",
    r"js/vite=>\[shown\] vite: modern",
    r"table/basic=>\[shown\] Name +City +Kind\r?\n\[shown\] Radio One +Sofia +Talk",
    r"radio/feed=>\[shown\] MAIN\r?\n\[shown\] Listen live",
    r"type:qy",
    r"type:version\n=>FLOPOS \d",
]
NEVER = ["EVIL RAN", "NOMODULE RAN", "SECRET LEAKED", "LOCAL REACHED", "HUGE RAN", "STOLE", "AN ADVERT", "SUBSCRIBE NOW", "SCREEN READER ONLY",
         "PHONE MENU", "HOVER MENU", "HIDDEN ATTR", "the script did not run"]

def pieces():
    """The steps cut at each page.  The first piece keeps its own j; later
    ones open their page, then j, and look for what the page said after it."""
    out, cur = [], []
    for st in STEPS:
        if not st.startswith(("key:", "type:", "shot:")) and cur: out.append(cur); cur = []
        cur.append(st)
    out.append(cur)
    for p in out[1:]:
        if (len(p) > 1 and p[1].startswith("key:j")) or not p[0].startswith("js/"): continue   # pages without scripts: j would not reload them
        page, _, want = p[0].partition("=>")
        p[0:1] = [page, "key:j" + ("=>" + want if want else "")]
    return out

def main():
    img = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "dist", "flopos.img")
    tmp = tempfile.mkdtemp(prefix="flopos-scripttest.")
    sys.path.insert(0, os.path.join(ROOT, "test", "bench"))
    import testca                                  # a copy that trusts the test site's https too
    img = testca.image(img, os.path.join(tmp, "t.img"))
    ok, site, results = True, None, {}
    with socket.socket() as s:                     # one test site for all of them
        if s.connect_ex(("127.0.0.1", 8150)):
            site = subprocess.Popen([sys.executable, "-I", os.path.join(ROOT, "test", "bench", "site.py"), "8150",
                                     os.path.expanduser("~/.cache/flopos-songs")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            time.sleep(0.5)
    def one(i, steps):
        out = os.path.join(tmp, "out%d" % i)
        r = subprocess.run([sys.executable, os.path.join(ROOT, "test", "bench", "run.py"), img, out] + steps,
                           capture_output=True, text=True)
        log = os.path.join(out, "serial.log")
        keep, after_fail = [], False                # ok and FAILED, and what a failure said
        for l in r.stdout.splitlines():
            if l.startswith(("ok", "FAILED")): after_fail = l.startswith("FAILED"); keep.append(l)
            elif after_fail and l.startswith(" "): keep.append(l)
        results[i] = (keep,
                      open(log, "rb").read().decode("latin-1") if os.path.exists(log) else "")
    try:
        ps = pieces()
        th = [threading.Thread(target=one, args=(i, p)) for i, p in enumerate(ps)]
        for t in th: t.start()
        for t in th: t.join()
        lines = [l for i in range(len(ps)) for l in results[i][0]]
        print("\n".join(lines))
        ok = sum(l.startswith("ok") for l in lines) == sum(1 for s in STEPS if "=>" in s) and not any(l.startswith("FAILED") for l in lines)
        text = "\n".join(results[i][1] for i in range(len(ps)))
        shown = "\n".join(l for l in text.splitlines() if l.startswith("[shown]") and "/js/basic" not in l)
        after_allowed = shown.split("item three", 1)[-1] if "item three" in shown else shown
        for bad in NEVER:
            hay = after_allowed if bad == "the script did not run" else shown
            if bad in hay:
                print("FAILED  shown, and must never be: %s" % bad); ok = False
    finally:
        if site: site.terminate()
        shutil.rmtree(tmp, ignore_errors=True)
    sys.exit(0 if ok else 1)

if __name__ == "__main__":
    main()
