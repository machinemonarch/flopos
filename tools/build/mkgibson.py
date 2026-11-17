#!/usr/bin/env python3
"""Make data/gibson.raw, the key hit `gibson on` plays, and hudon.raw and
hudoff.raw beside it, the HUD's chimes as it opens and closes.

    tools/build/mkgibson.py [data/gibson.raw] [--preview OUT.wav]

Each hit is a pair of telephone (DTMF) tones two octaves down, a thump that
glides down in pitch as a kick drum's does, and a dull click, layered and
run through a low-pass filter, so they are heard as one sound.  Every key
makes the same one.  8-bit unsigned mono at HIT_RATE, as the Sound Blaster
plays it, rounded with noise shaping so the 8 bits stay clean; the kernel
(src/kernel/audio.c) mixes it into whatever is playing.  --preview also
writes it over and over at typing speed, to listen to on the PC.
"""
import math, os, random, sys, wave

HIT_RATE = 22050
HIT_LEN = 2700                                  # samples: about 122 ms
LOW, HIGH = 697, 1209                           # the telephone's 1 key
DOWN = 4                                        # two octaves below the telephone


def lowpass(x, hz):
    """Two one-pole low-passes in a row: a gentle 12 dB an octave."""
    a = math.exp(-2 * math.pi * hz / HIT_RATE)
    for _ in range(2):
        y, out = 0.0, []
        for v in x:
            y = (1 - a) * v + a * y
            out.append(y)
        x = out
    return x


def hit(lo, hi, rnd):
    """One key: KYLLYCAT(h)LUNK, in about 120 ms.  K, a sharp click made of
    a tone (no noise: no hiss); YLLY, a warble flicking between two phone
    pairs every 6 ms; CAT, a short higher pair; LUNK, a low thump gliding
    down.  Each piece stops short, so no quiet tail is left for 8 bits to
    turn into a whisper."""
    n = HIT_LEN
    out = [0.0] * n
    def tone(start, length, freqs, amp, fall):
        for i in range(length):
            if start + i >= n: break
            s = i / HIT_RATE
            env = min(1.0, s / 0.0006) * math.exp(-s / fall) * min(1.0, (length - i) / (0.0015 * HIT_RATE))
            out[start + i] += amp * env * sum(math.sin(2 * math.pi * f * s) for f in freqs) / len(freqs)
    ms = lambda x: int(HIT_RATE * x / 1000)
    tone(0, ms(4), (1633 / 2, 1477), 0.9, 0.0015)                       # K
    pairs = ((697 / 2, 1209 / 2), (852 / 2, 1477 / 2))
    for k in range(6):                                                   # Y-LL-Y
        tone(ms(4) + k * ms(6), ms(6), pairs[k & 1], 0.75, 0.02)
    tone(ms(40), ms(12), (941 / 2, 1633 / 2), 0.8, 0.006)               # CAT
    ph = 0.0                                                             # (h)LUNK
    for i in range(ms(70)):
        if ms(52) + i >= n: break
        s = i / HIT_RATE
        f = 55 + 95 * math.exp(-s / 0.010)
        ph += 2 * math.pi * f / HIT_RATE
        env = min(1.0, s / 0.0008) * math.exp(-s / 0.022) * min(1.0, (ms(70) - i) / ms(4))
        out[ms(52) + i] += 0.8 * env * math.sin(ph)
    return lowpass(out, 2200)


def chime(up):
    """The HUD's chime: two quick glassy blips a fifth apart, rising as it
    opens (up) and falling as it closes, over a soft sweep of data-chirp,
    with a fast square-saw arpeggio on top; through a low-pass.  All of it
    seven semitones below where it was first made (K)."""
    K = 2 ** (-7 / 12)
    n = int(HIT_RATE * 0.16)
    notes = (988 * K, 1480 * K) if up else (1480 * K, 988 * K)
    out = [0.0] * n
    for k, f in enumerate(notes):
        start = int(HIT_RATE * 0.045 * k)
        for i in range(n - start):
            s = i / HIT_RATE
            env = min(1.0, s / 0.002) * math.exp(-s / 0.03)
            out[start + i] += env * (math.sin(2 * math.pi * f * s) + 0.3 * math.sin(2 * math.pi * f * 2.01 * s))
    ph = 0.0
    for i in range(n):
        s = i / HIT_RATE
        f = ((2400 - 9000 * s) if up else (900 + 9000 * s)) * K   # the chirp, quick and faint
        ph += 2 * math.pi * f / HIT_RATE
        out[i] += 0.25 * math.exp(-s / 0.02) * math.sin(ph)
    # and over it, a very fast arpeggio of four square-saw notes (half
    # square, half saw: hollow and buzzy), 15 ms each, up or down
    arp = tuple(x * K for x in ((659, 831, 988, 1319) if up else (1319, 988, 831, 659)))
    each = int(HIT_RATE * 0.015)
    for k, f in enumerate(arp):
        for i in range(each):
            s = i / HIT_RATE
            ph = (f * s) % 1.0
            sq = 1.0 if ph < 0.5 else -1.0
            saw = 2.0 * ph - 1.0
            env = min(1.0, s / 0.0008) * math.exp(-s / 0.012)
            out[k * each + i] += 0.45 * env * (0.5 * sq + 0.5 * saw)
    return lowpass(out, 3200)


def loudness(h):
    """Root-mean-square over the part that sounds: how loud it seems."""
    on = [v for v in h if abs(v) > 1e-4] or [0.0]
    return math.sqrt(sum(v * v for v in on) / len(on))


def to8(h, scale):
    """8 bits at the given scale; once it has died to under a step, silence."""
    loud = max(i for i, v in enumerate(h) if abs(v * scale) >= 1.0)
    return bytearray(max(0, min(255, int(round(128 + v * scale)))) if i <= loud else 128
                     for i, v in enumerate(h))


def main():
    out = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith("--") else "data/gibson.raw"
    preview = sys.argv[sys.argv.index("--preview") + 1] if "--preview" in sys.argv else None
    sounds = [hit(LOW, HIGH, random.Random(1995)), chime(True), chime(False)]
    # all three equally loud to the ear: the same loudness, as loud as the
    # one with the least headroom allows without clipping
    target = min(124 * loudness(h) / max(abs(v) for v in h) for h in sounds)
    raw, on, off = (to8(h, target / loudness(h)) for h in sounds)
    open(out, "wb").write(raw)
    d = os.path.dirname(out) or "."
    open(os.path.join(d, "hudon.raw"), "wb").write(on)
    open(os.path.join(d, "hudoff.raw"), "wb").write(off)
    print("wrote %s and the HUD's two chimes, at %d Hz" % (out, HIT_RATE))
    if preview:                                  # the two chimes, a second apart
        gap = bytes([128]) * int(HIT_RATE * 0.8)
        w = wave.open(preview, "wb"); w.setnchannels(1); w.setsampwidth(1); w.setframerate(HIT_RATE)
        w.writeframes(bytes(gap + on + gap + gap + off + gap)); w.close()

main()
