#!/usr/bin/env python3
"""train.py DIR OUT [--steps N] -- teach the wizard to talk.

Reads every DIR/*.lines (corpus.py and voice.py make them), learns a small
vocabulary of word pieces, trains a very small transformer on a mix of the
sources, and writes it to OUT in the form FLOPOS will read: weights
as single bytes, one scale a row.  speak.py reads the same file.

Run it with the Python that has PyTorch (see docs/building.md).
Prints one line: ok or FAILED, the loss and the size.
"""
import argparse, glob, os, random, re, struct, sys
from collections import Counter

import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F

p = argparse.ArgumentParser()
p.add_argument("dir"); p.add_argument("out")
p.add_argument("--steps", type=int, default=30000)
p.add_argument("--vocab", type=int, default=512)
p.add_argument("--d", type=int, default=96)
p.add_argument("--layers", type=int, default=3)
p.add_argument("--heads", type=int, default=4)
p.add_argument("--ff", type=int, default=128)
p.add_argument("--ctx", type=int, default=64)
p.add_argument("--batch", type=int, default=256)
p.add_argument("--books", type=float, default=0.05,
               help="share of the training given to the two books together")
p.add_argument("--limit", type=int, default=262144, help="largest file, bytes")
A = p.parse_args()
random.seed(430); torch.manual_seed(430)
dev = "cuda" if torch.cuda.is_available() else "cpu"

# ---- the reading ------------------------------------------------------------
src = {}
for f in sorted(glob.glob(os.path.join(A.dir, "*.lines"))):
    ls = [l for l in open(f).read().split("\n") if l]
    random.shuffle(ls)
    src[os.path.basename(f)[:-6]] = ls

# ---- word pieces: byte-pair merges over words, counted evenly by source ----
def words(l):
    return re.findall(r" ?[^ ]+", l)

freq = Counter()
for ls in src.values():
    c = Counter(w for l in ls for w in words(l))
    tot = sum(c.values())
    for w, n in c.items():
        freq[w] += n / tot
chars = sorted({ch for w in freq for ch in w})
vocab = ["\n"] + chars
split = {w: list(w) for w in freq}
while len(vocab) < A.vocab:
    pairs = Counter()
    for w, s in split.items():
        for a, b in zip(s, s[1:]):
            pairs[a, b] += freq[w]
    (a, b), _ = pairs.most_common(1)[0]
    vocab.append(a + b)
    for w, s in split.items():
        if a in s:
            i, o = 0, []
            while i < len(s):
                if i + 1 < len(s) and s[i] == a and s[i + 1] == b:
                    o.append(a + b); i += 2
                else:
                    o.append(s[i]); i += 1
            split[w] = o
tid = {t: i for i, t in enumerate(vocab)}
enc = {w: [tid[t] for t in s] for w, s in split.items()}

def stream(ls):
    o = [0]
    for l in ls:
        for w in words(l):
            o += enc[w]
        o.append(0)
    return torch.tensor(o, dtype=torch.long, device=dev)

train, val = {}, {}
for k, ls in src.items():
    n = max(50, len(ls) // 50)
    val[k], train[k] = stream(ls[:n]), stream(ls[n:])
names = sorted(train)

BOOKS = ("shakespeare", "jargon")
share = {k: A.books / 2 if k in BOOKS else
         (1 - A.books) / (len(names) - len(BOOKS)) for k in names}

def batch(d, n):
    """n windows of text.  His own flavours share evenly; the two books
    get a small share between them, as he can't learn them well."""
    xs = []
    ar = torch.arange(A.ctx + 1, device=dev)
    for k in names:
        per = max(1, round(n * share[k]))
        o = torch.randint(0, len(d[k]) - A.ctx - 1, (per, 1), device=dev)
        xs.append(d[k][o + ar])
    x = torch.cat(xs)
    return x[:, :-1], x[:, 1:]

# ---- the model --------------------------------------------------------------
QAT = False
def q(w):
    """Weights as FLOPOS will hold them: a byte each, one scale a row."""
    if not QAT:
        return w
    s = w.abs().amax(dim=1, keepdim=True).clamp_min(1e-8) / 127
    return w + ((w / s).round() * s - w).detach()

class Block(nn.Module):
    def __init__(s):
        super().__init__()
        d = A.d
        s.ln1, s.ln2 = nn.LayerNorm(d), nn.LayerNorm(d)
        s.wq, s.wk, s.wv, s.wo = (nn.Parameter(torch.randn(d, d) * 0.02)
                                  for _ in range(4))
        s.w1 = nn.Parameter(torch.randn(A.ff, d) * 0.02)
        s.b1 = nn.Parameter(torch.zeros(A.ff))
        s.w2 = nn.Parameter(torch.randn(d, A.ff) * 0.02)
        s.b2 = nn.Parameter(torch.zeros(d))
    def forward(s, x):
        B, T, d = x.shape
        h = s.ln1(x)
        sp = lambda t: t.view(B, T, A.heads, d // A.heads).transpose(1, 2)
        a = F.scaled_dot_product_attention(sp(h @ q(s.wq).T), sp(h @ q(s.wk).T),
                                           sp(h @ q(s.wv).T), is_causal=True)
        x = x + a.transpose(1, 2).reshape(B, T, d) @ q(s.wo).T
        h = s.ln2(x)
        return x + F.relu(h @ q(s.w1).T + s.b1) @ q(s.w2).T + s.b2

class Wizard(nn.Module):
    def __init__(s):
        super().__init__()
        s.tok = nn.Parameter(torch.randn(A.vocab, A.d) * 0.02)
        s.pos = nn.Parameter(torch.randn(A.ctx, A.d) * 0.02)
        s.blocks = nn.ModuleList(Block() for _ in range(A.layers))
        s.ln = nn.LayerNorm(A.d)
    def forward(s, x):
        tok = q(s.tok)
        h = tok[x] + q(s.pos)[:x.shape[1]]
        for b in s.blocks:
            h = b(h)
        return s.ln(h) @ tok.T

m = Wizard().to(dev)
opt = torch.optim.AdamW(m.parameters(), lr=3e-3, weight_decay=0.01,
                        betas=(0.9, 0.98))
sched = torch.optim.lr_scheduler.OneCycleLR(opt, 3e-3, total_steps=A.steps,
                                            pct_start=0.03)
for step in range(A.steps):
    QAT = step >= A.steps * 0.8          # the last fifth, as bytes
    x, y = batch(train, A.batch)
    loss = F.cross_entropy(m(x).reshape(-1, A.vocab), y.reshape(-1))
    opt.zero_grad(set_to_none=True)
    loss.backward()
    nn.utils.clip_grad_norm_(m.parameters(), 1.0)
    opt.step(); sched.step()

QAT = True
m.eval()
with torch.no_grad():
    vl = sum(F.cross_entropy(m(x).reshape(-1, A.vocab), y.reshape(-1)).item()
             for x, y in (batch(val, 512) for _ in range(20))) / 20

# ---- the file ---------------------------------------------------------------
def rows(w):
    w = w.detach().cpu().numpy().astype(np.float32)
    s = np.maximum(np.abs(w).max(axis=1, keepdims=True), 1e-8) / 127
    return s.astype("<f4").tobytes() + np.round(w / s).astype(np.int8).tobytes()

def vec(v):
    return v.detach().cpu().numpy().astype("<f4").tobytes()

out = b"WIZ1" + struct.pack("<6H", A.vocab, A.d, A.layers, A.heads, A.ff, A.ctx)
for t in vocab:
    b = t.encode("ascii")
    out += bytes([len(b)]) + b
out += rows(m.tok) + rows(m.pos)
for b in m.blocks:
    out += vec(b.ln1.weight) + vec(b.ln1.bias)
    out += rows(b.wq) + rows(b.wk) + rows(b.wv) + rows(b.wo)
    out += vec(b.ln2.weight) + vec(b.ln2.bias)
    out += rows(b.w1) + vec(b.b1) + rows(b.w2) + vec(b.b2)
out += vec(m.ln.weight) + vec(m.ln.bias)
open(A.out, "wb").write(out)
good = len(out) <= A.limit and vl == vl
print("%s loss %.3f, %d bytes" % ("ok" if good else "FAILED", vl, len(out)))
sys.exit(0 if good else 1)
