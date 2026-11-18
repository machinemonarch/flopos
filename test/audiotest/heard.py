#!/usr/bin/env python3
"""What the bench recorded, against the file FLOPOS was playing.

    test/audiotest/heard.py SOUND.WAV FILE

SOUND.WAV is test/bench/run.py's recording of the Sound Blaster (run it
with --listen S); FILE is what browzer played.  FILE is decoded on this PC
by FLOPOS's own decoders (build/audiotest/decode, which codecs.py builds),
and each second of the recording is looked for in it.  Prints how many
seconds had sound, how many matched the file (correlation over 0.9), and
for each second where in the file it matched best.  The first minute or
so of a recording is noise from the emulator's start, before any sound.
"""
import sys, wave, subprocess, numpy as np
wav, ref_file = sys.argv[1], sys.argv[2]
import os
r = subprocess.run([os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "build", "audiotest", "decode"), ref_file], capture_output=True)
rate = int(r.stderr.decode().split(", ")[1].split()[0])
ref = np.frombuffer(r.stdout, dtype="<i2").astype(float)
w = wave.open(wav); R = w.getframerate()
a = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").astype(float).reshape(-1, w.getnchannels()).mean(1)
idx = (np.arange(int(len(a) * rate / R)) * R / rate).astype(int)
a = a[idx]
best = []
for start in range(0, len(a) - 2 * rate, rate):
    seg = a[start:start + rate] - a[start:start + rate].mean()
    if seg.std() < 30: continue
    n = len(seg); L = 1 << int(np.ceil(np.log2(len(ref) + n)))
    c = np.fft.irfft(np.fft.rfft(ref, L) * np.conj(np.fft.rfft(seg, L)), L)[:len(ref) - n]
    cs2 = np.concatenate([[0], np.cumsum(ref ** 2)]); e = np.sqrt((cs2[n:] - cs2[:-n])[:len(c)]) * np.sqrt((seg ** 2).sum()) + 1e-9
    k = int(np.argmax(c / e)); best.append((start / rate, k / rate, (c / e)[k]))
good = [b for b in best if b[2] > 0.9]
print("seconds with sound: %d; matching the file (>0.9): %d" % (len(best), len(good)))
print(" ".join("%.0f:%.1f:%.2f" % b for b in best))
