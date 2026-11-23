#!/usr/bin/env python3
"""How fast FLOPOS boots to its riddle under each QEMU clock, side by side.

    test/bench/clockcheck.py [IMAGE]

Boots the image once per clock setting, all at the same time, and prints
the seconds each took to reach the riddle (the first thing that waits on a
person).  For choosing the clock the test tools run with.
"""
import os, subprocess, sys, tempfile, threading, time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CLOCKS = {
    "real clock (no icount)": [],
    "icount shift=5, sleep=on": ["-icount", "shift=5,sleep=on,align=off"],
    "icount shift=6, sleep=on": ["-icount", "shift=6,sleep=on,align=off"],
    "icount shift=7, sleep=on": ["-icount", "shift=7,sleep=on,align=off"],
    "icount shift=4, sleep=on, align=off (the tools now)": ["-icount", "shift=4,sleep=on,align=off"],
    "icount shift=4, sleep=off": ["-icount", "shift=4,sleep=off"],
    "icount shift=2, sleep=on": ["-icount", "shift=2,sleep=on,align=off"],
    "icount shift=auto, sleep=on": ["-icount", "shift=auto,sleep=on,align=off"],
}

def main():
    img = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "dist", "flopos.img")
    tmp = tempfile.mkdtemp(prefix="flopos-clock.")
    out = {}
    def one(name, flags, i):
        log = os.path.join(tmp, "s%d.log" % i)
        q = subprocess.Popen(["qemu-system-i386"] + flags + ["-machine", "pc", "-cpu", "pentium", "-m", "32",
                              "-drive", "file=%s,if=floppy,format=raw,snapshot=on" % img, "-boot", "a", "-display", "none",
                              "-serial", "file:" + log, "-monitor", "none"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        t0 = time.time()
        while time.time() - t0 < 120:
            try:
                if b"your answer:" in open(log, "rb").read(): out[name] = time.time() - t0; break
            except OSError: pass
            time.sleep(0.1)
        q.terminate()
    th = [threading.Thread(target=one, args=(n, f, i)) for i, (n, f) in enumerate(CLOCKS.items())]
    for t in th: t.start()
    for t in th: t.join()
    for n in CLOCKS: print("%6s s  %s" % ("%.1f" % out[n] if n in out else "never", n))
    import shutil; shutil.rmtree(tmp, ignore_errors=True)

if __name__ == "__main__":
    main()
