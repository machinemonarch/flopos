#!/usr/bin/env python3
"""The wall between apps and the system, tried by an app that breaks the rules.

    test/walltest/run.py [IMAGE]

Builds test/walltest/rogue.c as an app, puts it on a scratch copy of the
image (dist/flopos.img unless named) and runs it in the emulator once for
each rule it breaks.  Each time the app must be closed with the right
sentence, and FLOPOS must still answer afterwards.  Prints ok or FAILED
for each.
"""
import os, shutil, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CASES = [(0, r"\[rogue\] behaving"),
         (1, "closed: it reached outside its own memory"),
         (2, "closed: it reached outside its own memory"),
         (3, "closed: it tried something only the system may do"),
         (4, "closed: it tried something only the system may do"),
         (5, "closed: it handed over memory that is not its own"),
         (6, "closed: it handed over memory that is not its own"),
         (7, "closed: it handed over memory that is not its own"),
         (8, "closed: it reached outside its own memory"),
         (9, "closed: it answered a call nobody made"),
         (10, r"\[rogue\] refused"),
         (11, "closed: it reached outside its own memory"),
         (12, "closed: it handed over memory that is not its own")]

def main():
    img = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "dist", "flopos.img")
    r = subprocess.run(["make", "-s", "-C", ROOT, "dist/test/rogue.app"], capture_output=True, text=True)
    if r.returncode: print("FAILED  the rogue app does not build\n" + r.stderr[-400:]); sys.exit(1)
    tmp = tempfile.mkdtemp(prefix="flopos-walltest.")
    try:
        work = os.path.join(tmp, "t.img")
        shutil.copy(img, work)
        subprocess.run(["mcopy", "-i", work, os.path.join(ROOT, "dist", "test", "rogue.app"), "::ROGUE.APP"], check=True)
        steps = []
        for n, want in CASES:
            steps.append("type:rogue %d\\n=>%s" % (n, want))
        steps.append(r"type:version\n=>FLOPOS \d")
        steps.append(r"type:inventory\n=>\[inventory\] / \d+ things")
        r = subprocess.run([sys.executable, os.path.join(ROOT, "test", "bench", "run.py"), work, os.path.join(tmp, "out")] + steps,
                           capture_output=True, text=True)
        lines = [l for l in r.stdout.splitlines() if l.startswith(("ok", "FAILED"))]
        print("\n".join(lines))
        log = os.path.join(tmp, "out", "serial.log")
        if os.path.exists(log) and b"still here" in open(log, "rb").read():
            print("FAILED  the app got past the wall"); sys.exit(1)
        sys.exit(0 if lines and all(l.startswith("ok") for l in lines) and len(lines) == len(steps) else 1)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

if __name__ == "__main__":
    main()
