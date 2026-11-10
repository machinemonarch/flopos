#!/usr/bin/env python3
"""Hold renders of FLOPOS's tracker up against libopenmpt's.

    tools/audiotest/compare.py SONG RENDER.u8 [SONG RENDER.u8 ...]

For each pair: the median correlation of half-second windows (each allowed
to slip a little, since tick lengths round differently), the loudness of
both, and how much of ours clipped.  libopenmpt is the reference: linear
interpolation and no volume ramping, as FLOPOS mixes.  It needs
libopenmpt.so.0 and numpy.  0.8 and up is the same music; 0.95 and up,
the same in nearly every detail.
"""
import ctypes as C, sys
import numpy as np

def reference(path, secs=40, rate=22050):
    mpt = C.CDLL("libopenmpt.so.0")
    mpt.openmpt_module_create_from_memory.restype = C.c_void_p
    mpt.openmpt_module_create_from_memory.argtypes = [C.c_void_p, C.c_size_t, C.c_void_p, C.c_void_p, C.c_void_p]
    mpt.openmpt_module_read_mono.argtypes = [C.c_void_p, C.c_int32, C.c_size_t, C.POINTER(C.c_int16)]
    mpt.openmpt_module_read_mono.restype = C.c_size_t
    mpt.openmpt_module_set_render_param.argtypes = [C.c_void_p, C.c_int, C.c_int32]
    d = open(path, "rb").read()
    m = mpt.openmpt_module_create_from_memory(d, len(d), None, None, None)
    if not m: return None
    mpt.openmpt_module_set_render_param(m, 3, 2)    # interpolation: linear
    mpt.openmpt_module_set_render_param(m, 4, 0)    # volume ramping: none
    out = (C.c_int16 * (rate * secs))()
    n = mpt.openmpt_module_read_mono(m, rate, rate * secs, out)
    return np.frombuffer(out, dtype=np.int16)[:n].astype(float)

def ours(path):
    return np.fromfile(path, dtype=np.uint8).astype(float) - 128

def score(song, render):
    r, o, w, lag, cs = reference(song), ours(render), 11025, 0, []
    for s in range(0, min(len(r), len(o)) - w - 1200, w):
        a = o[s:s + w]
        if np.std(a) < 0.5 or np.std(r[s:s + w]) < 30: continue
        best, bl = -1, lag
        for L in range(lag - 600, lag + 601, 3):
            if s + L < 0 or s + L + w > len(r): continue
            c = np.corrcoef(a, r[s + L:s + L + w])[0, 1]
            if c > best: best, bl = c, L
        cs.append(best); lag = bl
    return (np.median(cs) if cs else float("nan"), len(cs), np.std(o), np.std(r) / 256,
            100 * np.mean((o >= 127) | (o <= -128)))

if __name__ == "__main__":
    a = sys.argv[1:]
    if not a or len(a) % 2: sys.exit(__doc__)
    for song, render in zip(a[0::2], a[1::2]):
        m, k, lo, lr, clip = score(song, render)
        print("%-28s match %.3f over %2d windows, loudness ours %5.1f ref %5.1f (of 128), clipped %.2f%%"
              % (song.split("/")[-1][-28:], m, k, lo, lr, clip))
