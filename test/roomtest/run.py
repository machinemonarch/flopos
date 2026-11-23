#!/usr/bin/env python3
"""The script room (src/kernel/room.c, src/room/), tried for real in the emulator.

    test/roomtest/run.py [IMAGE]

First with the real ROOM.BIN: a script runs, a script that never ends is
stopped, a script that fills its memory is stopped.  Then with a rogue
room in its place (rogueroom.c) that breaks one rule per task: each time
it must be stopped with the right sentence, the app that drove it must
carry on with its memory untouched, and FLOPOS must still answer.
Each task runs on its own machine, all at once.  Prints ok or FAILED for each.
"""
import os, shutil, subprocess, sys, tempfile
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
REAL = [(0, r"answer 0 "),
        (1, r"answer 3 it ran out of time"),
        (2, r"answer 4 it ran out of room")]
ROGUE = [(10, "stopped: it reached outside its own memory"),
         (11, "stopped: it reached outside its own memory"),
         (12, "stopped: it reached outside its own memory"),
         (13, "stopped: it tried something only the system may do"),
         (14, "stopped: it tried something only the system may do"),
         (15, "stopped: it reached outside its own memory"),
         (16, "stopped: it ran past its time, and the system stopped it"),
         (17, "stopped: it pointed the door outside its own memory"),
         (18, "stopped: it answered a call nobody made"),
         (19, "stopped: it asked for something that does not exist"),
         (20, "answer 0 pong")]

def run(img, cases, tmp, tag):
    steps = []
    for n, want in cases:
        # the answer, then the app's own word that it is still in one piece
        steps.append("type:roomt %d\\n=>\\[roomt\\] %s[^\\n]*\\n(?:.*\\n)*?\\[roomt\\] still here\\r?\\n" % (n, want))
    steps.append(r"type:version\n=>FLOPOS \d")
    r = subprocess.run([sys.executable, os.path.join(ROOT, "test", "bench", "run.py"), img, os.path.join(tmp, tag)] + steps,
                       capture_output=True, text=True)
    lines = [l for l in r.stdout.splitlines() if l.startswith(("ok", "FAILED"))]
    return lines, len(steps)

def main():
    img = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "dist", "flopos.img")
    r = subprocess.run(["make", "-s", "-C", ROOT, "build/roomtest/roomt.app", "build/roomtest/rogueroom.bin"], capture_output=True, text=True)
    if r.returncode: print("FAILED  the test programs do not build\n" + r.stderr[-600:]); sys.exit(1)
    tmp = tempfile.mkdtemp(prefix="flopos-roomtest.")
    ok = True
    try:
        imgs = {}
        for tag, room in (("real", None), ("rogue", "rogueroom.bin")):
            work = imgs[tag] = os.path.join(tmp, tag + ".img")
            shutil.copy(img, work)
            subprocess.run(["mcopy", "-i", work, os.path.join(ROOT, "build", "roomtest", "roomt.app"), "::ROOMT.APP"], check=True)
            if room:
                subprocess.run(["mcopy", "-o", "-i", work, os.path.join(ROOT, "build", "roomtest", room), "::ROOM.BIN"], check=True)
        def one(job):
            tag, case = job
            name = "%s%d" % (tag, case[0])
            lines, want = run(imgs[tag], [case], tmp, name)
            good = len(lines) == want and all(l.startswith("ok") for l in lines)
            log = os.path.join(tmp, name, "serial.log")
            if os.path.exists(log) and b"still running after breaking a rule" in open(log, "rb").read():
                lines.append("FAILED  the rogue room got past a wall (task %d)" % case[0]); good = False
            if os.path.exists(log) and b"its memory was changed" in open(log, "rb").read():
                lines.append("FAILED  the room changed the app's memory (task %d)" % case[0]); good = False
            return lines, good
        jobs = [("real", c) for c in REAL] + [("rogue", c) for c in ROGUE]
        with ThreadPoolExecutor(len(jobs)) as pool:
            for lines, good in pool.map(one, jobs):
                print("\n".join(lines)); ok = ok and good
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    sys.exit(0 if ok else 1)

if __name__ == "__main__":
    main()
