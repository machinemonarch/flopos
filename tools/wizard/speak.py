#!/usr/bin/env python3
"""speak.py MODEL [N] [SEED] -- the wizard says N lines.

The same sums FLOPOS will do, written plainly so the engine there can be
checked against it: weights are bytes with one scale a row, everything else
is 32-bit floating point.  He starts from an empty line and stops at its
end.  '@' is left in place; FLOPOS puts the typed word there.
"""
import struct, sys
import numpy as np

TEMP, TOPK = 0.8, 30


class Wizard:
    def __init__(s, path):
        b = open(path, "rb").read()
        assert b[:4] == b"WIZ1"
        s.V, s.d, s.L, s.H, s.ff, s.ctx = struct.unpack_from("<6H", b, 4)
        s.b, s.o = b, 16
        s.vocab = []
        for _ in range(s.V):
            n = b[s.o]
            s.vocab.append(b[s.o + 1:s.o + 1 + n].decode())
            s.o += 1 + n
        s.tok, s.pos = s.rows(s.V, s.d), s.rows(s.ctx, s.d)
        s.blocks = []
        for _ in range(s.L):
            k = {}
            k["g1"], k["b1"] = s.vec(s.d), s.vec(s.d)
            for n in ("wq", "wk", "wv", "wo"):
                k[n] = s.rows(s.d, s.d)
            k["g2"], k["b2"] = s.vec(s.d), s.vec(s.d)
            k["w1"], k["c1"] = s.rows(s.ff, s.d), s.vec(s.ff)
            k["w2"], k["c2"] = s.rows(s.d, s.ff), s.vec(s.d)
            s.blocks.append(k)
        s.g, s.bb = s.vec(s.d), s.vec(s.d)
        assert s.o == len(b)

    def vec(s, n):
        v = np.frombuffer(s.b, "<f4", n, s.o); s.o += 4 * n
        return v

    def rows(s, r, c):
        sc = s.vec(r)
        w = np.frombuffer(s.b, np.int8, r * c, s.o).reshape(r, c); s.o += r * c
        return w.astype(np.float32) * sc[:, None]

    @staticmethod
    def norm(x, g, b):
        x = x - x.mean(-1, keepdims=True)
        return x / np.sqrt((x * x).mean(-1, keepdims=True) + 1e-5) * g + b

    def logits(s, ids):
        T, hd = len(ids), s.d // s.H
        x = s.tok[ids] + s.pos[:T]
        mask = np.triu(np.full((T, T), -1e9, np.float32), 1)
        for k in s.blocks:
            h = s.norm(x, k["g1"], k["b1"])
            sp = lambda w: (h @ w.T).reshape(T, s.H, hd).transpose(1, 0, 2)
            qq, kk, vv = sp(k["wq"]), sp(k["wk"]), sp(k["wv"])
            a = qq @ kk.transpose(0, 2, 1) / np.sqrt(hd) + mask
            a = np.exp(a - a.max(-1, keepdims=True))
            a /= a.sum(-1, keepdims=True)
            x = x + (a @ vv).transpose(1, 0, 2).reshape(T, s.d) @ k["wo"].T
            h = s.norm(x, k["g2"], k["b2"])
            x = x + np.maximum(h @ k["w1"].T + k["c1"], 0) @ k["w2"].T + k["c2"]
        return s.norm(x[-1], s.g, s.bb) @ s.tok.T

    def say(s, rng):
        ids = [0]
        while len(ids) < s.ctx:
            z = s.logits(ids) / TEMP
            top = np.argsort(z)[-TOPK:]
            pr = np.exp(z[top] - z[top].max())
            t = int(rng.choice(top, p=pr / pr.sum()))
            if t == 0:
                break
            ids.append(t)
        return "".join(s.vocab[i] for i in ids[1:]), len(ids) < s.ctx


if __name__ == "__main__":
    w = Wizard(sys.argv[1])
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 10
    rng = np.random.default_rng(int(sys.argv[3]) if len(sys.argv) > 3 else 1)
    for _ in range(n):
        print(w.say(rng)[0])
