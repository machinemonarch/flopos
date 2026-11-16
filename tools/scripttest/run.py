#!/usr/bin/env python3
"""Scripts and styles in browzer, tried for real in the emulator.

    tools/scripttest/run.py [IMAGE]

On a scratch copy of the image (dist/flopos.img unless named), against the
test site's pages (tools/bench/site.py, /js/... and /css/basic): scripts
are off until the site is allowed with j; then a page's own scripts run,
another site's are not fetched, clicks, fetches and timers work, the
tripwires stop a script that sends a form or fetches elsewhere, a script
that never ends is stopped, and styles hide what they should.  browzer
says what each page shows on the serial wire (for this test site only),
and nothing it must never show may be there.  Prints ok or FAILED.
"""
import os, shutil, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
STEPS = [
    r"js/basic=>none of them run",                         # off until allowed
    r"key:j=>ran app\.js(.|\n)*not fetched, another site(.|\n)*\[shown\] \* item three",
    r"js/click=>Scripts: 1 ran",
    r"key:tab",
    r"key:ret=>\[shown\] clicked 1",
    r"js/fetch=>\[shown\] \* fetched beta",
    r"js/timer=>\[shown\] tick 3",
    r"js/elsewhere=>refused: it tried to fetch from another site",
    r"js/form=>refused: it tried to send a form to another site",
    r"js/loop=>stopped: it ran out of time",
    r"css/basic=>\[shown\] +Centred title(.|\n)*\[shown\] The end\.",
    r"radio/player=>\[shown\] Listen Live\r?\n\[shown\] \[listen\](.|\n)*sound it names: https://example\.test/listen/main/radio\.mp3",
    r"js/pieces=>\[shown\] pieces: aborted signal instanceof a=1&b=y string 2 clone utf8 any",
    r"js/async=>\[shown\] \* async alpha\r?\n\[shown\] \* async beta",
    r"key:tab",
    r"key:ret=>\[shown\] clicked, after a wait",
    r"table/basic=>\[shown\] Name +City +Kind\r?\n\[shown\] Radio One +Sofia +Talk",
    r"radio/feed=>\[shown\] MAIN\r?\n\[shown\] Listen live",
    r"type:qy",
    r"type:version\n=>FLOPOS \d",
]
NEVER = ["EVIL RAN", "STOLE", "STILL RUNNING", "AN ADVERT", "SUBSCRIBE NOW", "SCREEN READER ONLY",
         "PHONE MENU", "HOVER MENU", "HIDDEN ATTR", "the script did not run"]

def main():
    img = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "dist", "flopos.img")
    tmp = tempfile.mkdtemp(prefix="flopos-scripttest.")
    ok = True
    try:
        work = os.path.join(tmp, "t.img")
        shutil.copy(img, work)
        r = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "bench", "run.py"), work, os.path.join(tmp, "out")] + STEPS,
                           capture_output=True, text=True)
        lines = [l for l in r.stdout.splitlines() if l.startswith(("ok", "FAILED"))]
        print("\n".join(lines))
        ok = len(lines) == sum(1 for s in STEPS if "=>" in s) and all(l.startswith("ok") for l in lines)
        log = os.path.join(tmp, "out", "serial.log")
        text = open(log, "rb").read().decode("latin-1") if os.path.exists(log) else ""
        shown = "\n".join(l for l in text.splitlines() if l.startswith("[shown]") and "/js/basic" not in l)
        after_allowed = shown.split("item three", 1)[-1] if "item three" in shown else shown
        for bad in NEVER:
            hay = after_allowed if bad == "the script did not run" else shown
            if bad in hay:
                print("FAILED  shown, and must never be: %s" % bad); ok = False
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    sys.exit(0 if ok else 1)

if __name__ == "__main__":
    main()
