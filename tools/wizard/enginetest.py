#!/usr/bin/env python3
"""enginetest.py MODEL -- does FLOPOS's engine do the same sums as speak.py?

Builds src/kernel/wizsay.c for this PC (into build/wizard/), has both work through the
same lines piece by piece, and compares every score.  Prints ok or FAILED.
"""
import os, subprocess, sys
import numpy as np
here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
from speak import Wizard, TOPK

root = os.path.dirname(os.path.dirname(here))
exe = os.path.join(root, "build", "wizard", "wiztest")
os.makedirs(os.path.dirname(exe), exist_ok=True)
subprocess.run(["gcc", "-O2", "-o", exe, os.path.join(here, "enginetest.c")],
               check=True)
w = Wizard(sys.argv[1])
rng = np.random.default_rng(11)
worst, bad = 0.0, 0
for _ in range(6):
    ids = [0]
    while len(ids) < w.ctx:               # a line of his, and past its end
        z = w.logits(ids)
        top = np.argsort(z)[-TOPK:]
        ids.append(int(rng.choice(top)))
    out = subprocess.run([exe, sys.argv[1]] + [str(i) for i in ids],
                         capture_output=True, text=True).stdout.split("\n")
    for t in range(len(ids)):
        nums, picks = out[t].split("|")
        c = np.array(nums.split(), dtype=np.float32)
        z = w.logits(ids[:t + 1])
        worst = max(worst, float(np.abs(c - z).max()))
        first, last = (int(x) for x in picks.split())
        order = np.argsort(z)
        bad += first != order[-1] and abs(z[first] - z[order[-1]]) > 1e-3
        bad += z[last] < z[order[-TOPK]] - 1e-3
ok = worst < 0.02 and not bad
print("ok" if ok else "FAILED: scores differ by %.4f, %d wrong picks" % (worst, bad))
sys.exit(0 if ok else 1)
