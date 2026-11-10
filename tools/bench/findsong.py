#!/usr/bin/env python3
"""Find a host render of a song in what the Sound Blaster played.

    tools/bench/findsong.py OUT/sound.wav RENDER.u8 [RENDER.u8 ...]

Compares loudness over time (25 ms steps), which survives QEMU's
resampling and the card's rate (22,222 Hz for 22,050 asked).  0.9 and up:
it is that song, playing as the host played it.
"""
import sys, wave
import numpy as np

def env(a, hop=551):
    n = len(a) // hop * hop
    return np.sqrt(np.mean(a[:n].reshape(-1, hop) ** 2, axis=1))

w = wave.open(sys.argv[1])
x = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(float).reshape(-1, w.getnchannels()).mean(axis=1)
if w.getframerate() == 44100: x = (x[0::2][:len(x) // 2] + x[1::2][:len(x) // 2]) / 2
er = env(x)
for path in sys.argv[2:]:
    eo = env(np.fromfile(path, dtype=np.uint8).astype(float) - 128)[:1100]
    best = max((np.corrcoef(er[L:L + len(eo)], eo)[0, 1], L) for L in range(0, len(er) - len(eo), 2))
    print("%-30s %.3f, at %.1f s" % (path.split("/")[-1], best[0], best[1] * 551 / 22050))
