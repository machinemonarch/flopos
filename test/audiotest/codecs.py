#!/usr/bin/env python3
"""FLOPOS's decoders against GStreamer's, on files GStreamer makes.

    test/audiotest/codecs.py [CASE ...]        (no CASE: all of them)

Each case encodes a few seconds of a source recording with GStreamer (an
encoder and its settings), decodes the file with FLOPOS's own decoders
(decode.c, built here) and with GStreamer's, brings the reference to
FLOPOS's form (mono, at FLOPOS's rate), lines the two up and prints the
correlation (median over half-second windows) and the gain.  ok needs a
correlation of 0.98 or more and a gain within 0.5-1.0 (FLOPOS leaves 3 dB of
headroom: about 0.7).  Opus is checked against libopus itself (through
ctypes), decoding in mono as FLOPOS does; its speech-only files must be
refused.

The source is ~/.cache/flopos-fuzz/audio/seed*.mp3 (the bench site's); the
files made are kept in build/audiotest/codecs/.
"""
import glob, os, struct, subprocess, sys
import numpy as np

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
OUT = os.path.join(ROOT, "build", "audiotest", "codecs")
DEC = os.path.join(ROOT, "build", "audiotest", "decode")

# name: (encoder pipeline after the raw source, file name, reference decoder)
CASES = {
    "mp2-twolame-192": ("audio/x-raw,rate=44100,channels=2 ! twolamemp2enc bitrate=192", "a.mp2", "mpegaudioparse ! mpg123audiodec"),
    "mp2-twolame-64-mono": ("audio/x-raw,rate=48000,channels=1 ! twolamemp2enc bitrate=64 mode=mono", "b.mp2", "mpegaudioparse ! mpg123audiodec"),
    "mp2-twolame-32k-joint": ("audio/x-raw,rate=32000,channels=2 ! twolamemp2enc bitrate=64 mode=joint", "c.mp2", "mpegaudioparse ! mpg123audiodec"),
    "mp2-ffmpeg-384": ("audio/x-raw,rate=48000,channels=2 ! avenc_mp2 bitrate=384000", "d.mp2", "mpegaudioparse ! mpg123audiodec"),
    "mp2-ffmpeg-lsf-24k": ("audio/x-raw,rate=24000,channels=2 ! avenc_mp2 bitrate=96000", "e.mp2", "mpegaudioparse ! mpg123audiodec"),
    "mp2-twolame-96":  ("audio/x-raw,rate=44100,channels=2 ! twolamemp2enc bitrate=96", "p.mp2", "mpegaudioparse ! mpg123audiodec"),
    "mp2-ffmpeg-80-joint": ("audio/x-raw,rate=44100,channels=2 ! avenc_mp2 bitrate=80000", "f.mp2", "mpegaudioparse ! mpg123audiodec"),
    "mp3": ("audio/x-raw,rate=44100,channels=2 ! lamemp3enc target=bitrate bitrate=128 cbr=true", "g.mp3", "mpegaudioparse ! mpg123audiodec"),
    "m4a": ("audio/x-raw,rate=44100,channels=2 ! avenc_aac bitrate=96000 ! aacparse ! qtmux", "h.m4a", "qtdemux ! aacparse ! avdec_aac"),
    "m4a-faststart": ("audio/x-raw,rate=48000,channels=2 ! avenc_aac bitrate=128000 ! aacparse ! qtmux faststart=true", "i.m4a", "qtdemux ! aacparse ! avdec_aac"),
    "opus-128": ("audio/x-raw,rate=48000,channels=2 ! opusenc bitrate=128000 ! oggmux", "j.opus", "oggdemux ! opusdec ! audio/x-raw,channels=1"),
    "opus-64-mono": ("audio/x-raw,rate=48000,channels=1 ! opusenc bitrate=64000 ! oggmux", "k.opus", "oggdemux ! opusdec ! audio/x-raw,channels=1"),
    "opus-32-voice": ("audio/x-raw,rate=48000,channels=2 ! opusenc bitrate=32000 audio-type=voice ! oggmux", "l.opus", "oggdemux ! opusdec ! audio/x-raw,channels=1"),
    "opus-24-hybrid": ("audio/x-raw,rate=48000,channels=2 ! opusenc bitrate=24000 bandwidth=superwideband ! oggmux", "m.opus", "oggdemux ! opusdec ! audio/x-raw,channels=1"),
    "opus-12-silk": ("audio/x-raw,rate=48000,channels=1 ! opusenc bitrate=12000 bandwidth=wideband audio-type=voice ! oggmux", "n.opus", "oggdemux ! opusdec ! audio/x-raw,channels=1"),
    "opus-96-short": ("audio/x-raw,rate=48000,channels=2 ! opusenc bitrate=96000 frame-size=5 ! oggmux", "o.opus", "oggdemux ! opusdec ! audio/x-raw,channels=1"),
}

# Opus's speech layers (SILK, hybrid) are not read: a file of nothing else must be refused.
REFUSED = {"opus-12-silk"}

GST = """import gi, sys; gi.require_version('Gst','1.0'); from gi.repository import Gst; Gst.init(None)
p = Gst.parse_launch(sys.argv[1]); p.set_state(Gst.State.PLAYING)
m = p.get_bus().timed_pop_filtered(300 * Gst.SECOND, Gst.MessageType.EOS | Gst.MessageType.ERROR)
p.set_state(Gst.State.NULL)
sys.exit(0 if m and m.type == Gst.MessageType.EOS else 1)
"""

def gst(pipeline):
    return subprocess.run([sys.executable, "-c", GST, pipeline], capture_output=True).returncode == 0

def build():
    src = "test/audiotest/decode.c src/apps/lib/audio/aac.c src/apps/lib/audio/decode.c src/apps/lib/audio/flac.c " \
          "src/apps/lib/audio/mp3.c src/apps/lib/audio/mp4.c src/apps/lib/audio/vorbis.c src/apps/lib/audio/opus.c src/apps/lib/bits/prefix.c " \
          "src/apps/lib/dsp/dct.c src/apps/lib/dsp/mdct.c"
    src = " ".join(f for f in src.split() if os.path.exists(os.path.join(ROOT, f)))
    os.makedirs(os.path.dirname(DEC), exist_ok=True)
    r = subprocess.run("gcc -O2 -g -w -Isrc/apps/sdk -Isrc/apps/lib/audio -Isrc/apps/lib/bits -Isrc/apps/lib/dsp %s -lm -o '%s'"
                       % (src, DEC), shell=True, cwd=ROOT, capture_output=True, text=True)
    if r.returncode: print(r.stderr); sys.exit("FAILED: decode.c did not build")

def source():
    for f in sorted(glob.glob(os.path.expanduser("~/.cache/flopos-fuzz/audio/seed*.mp3"))):
        # the seeds can have broken tags: keep only whole MPEG-1 layer III frames
        b, i, out = open(f, "rb").read(), 0, bytearray()
        rates = (0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320)
        while i + 4 <= len(b):
            if b[i] != 0xFF or (b[i + 1] & 0xFE) != 0xFA or (b[i + 2] >> 4) in (0, 15) or (b[i + 2] >> 2) & 3 == 3: i += 1; continue
            n = 144000 * rates[b[i + 2] >> 4] // (44100, 48000, 32000)[(b[i + 2] >> 2) & 3] + ((b[i + 2] >> 1) & 1)
            if i + n > len(b): break
            out += b[i:i + n]; i += n
        if len(out) > 20000:
            p = os.path.join(OUT, "source.mp3")
            out = bytes(out) * (1 + 400000 // len(out))          # repeated to about 25 s
            open(p, "wb").write(out[:400000])
            return p
    sys.exit("FAILED: no source in ~/.cache/flopos-fuzz/audio/seed*.mp3")

def flopos(path):
    r = subprocess.run([DEC, path], capture_output=True)
    info = r.stderr.decode().strip()
    rate = int(info.split(", ")[1].split()[0]) if " Hz" in info else 0
    return np.frombuffer(r.stdout, dtype="<i2").astype(float), rate, info

def opus_reference(path, rate):
    """Opus decoded by the system's libopus, in mono (as FLOPOS plays it:
    with stereo phase inversion off, which GStreamer's opusdec, decoding
    in stereo, keeps), at 48 kHz, then brought to rate."""
    import ctypes
    try: lib = ctypes.CDLL("libopus.so.0")
    except OSError: return None
    err = ctypes.c_int()
    dec = ctypes.c_void_p(lib.opus_decoder_create(48000, 1, ctypes.byref(err)))
    lib.opus_decoder_create.restype = ctypes.c_void_p
    b, i, pkt, n, out = open(path, "rb").read(), 0, b"", 0, []
    pcm = (ctypes.c_float * 5760)()
    skip = 0
    while i + 27 <= len(b) and b[i:i + 4] == b"OggS":
        ns = b[i + 26]; lace = b[i + 27:i + 27 + ns]; at = i + 27 + ns
        for l in lace:
            pkt += b[at:at + l]; at += l
            if l < 255:
                n += 1
                if n == 1: skip = pkt[10] | pkt[11] << 8
                elif n > 2:
                    k = lib.opus_decode_float(dec, pkt, len(pkt), pcm, 5760, 0)
                    if k > 0: out.append(np.frombuffer(pcm, dtype=np.float32, count=k).copy())
                pkt = b""
        i = at
    x = np.concatenate(out)[skip:] * 32768.0
    if rate == 48000: return x
    raw = path + ".ref48.raw"
    x.astype("<i2").tofile(raw)
    out_raw = path + ".ref.raw"
    ok = gst('filesrc location="%s" ! rawaudioparse format=pcm pcm-format=s16le sample-rate=48000 num-channels=1 ! audioconvert '
             '! audioresample quality=10 ! audio/x-raw,format=S16LE,channels=1,rate=%d ! filesink location="%s"' % (raw, rate, out_raw))
    return np.fromfile(out_raw, dtype="<i2").astype(float) if ok else None

def reference(path, how, rate):
    if "opusdec" in how: return opus_reference(path, rate)
    raw = path + ".ref.raw"
    ok = gst('filesrc location="%s" ! %s ! audioconvert ! audioresample quality=10 ! audio/x-raw,format=S16LE,channels=1,rate=%d '
             '! filesink location="%s"' % (path, how, rate, raw))
    return np.fromfile(raw, dtype="<i2").astype(float) if ok else None

def compare(a, b, rate):
    """Line b up with a (a lag of up to a second either way), then the
    correlation in half-second windows (median) and the gain a/b."""
    n = min(len(a), len(b))
    a, b = a[:n], b[:n]
    L = 1 << int(np.ceil(np.log2(2 * n)))
    c = np.fft.irfft(np.fft.rfft(a, L) * np.conj(np.fft.rfft(b, L)), L)
    lags = np.concatenate([np.arange(0, rate), np.arange(L - rate, L)])
    k = int(lags[np.argmax(c[lags])])
    lag = k if k < L // 2 else k - L
    if lag >= 0: a, b = a[lag:], b[:len(b) - lag]
    else: a, b = a[:len(a) + lag], b[-lag:]
    w, cs = rate // 2, []
    for i in range(0, len(a) - w, w):
        x, y = a[i:i + w], b[i:i + w]
        if np.std(y) < 50: continue                       # quiet: says nothing
        cs.append(np.corrcoef(x, y)[0, 1])
    gain = np.sqrt((a ** 2).mean() / max((b ** 2).mean(), 1e-9))
    return (float(np.median(cs)) if cs else 0.0), gain, lag

def main():
    names = sys.argv[1:] or list(CASES)
    os.makedirs(OUT, exist_ok=True)
    build()
    src = source()
    failed = 0
    for name in names:
        enc, fname, how = CASES[name]
        path = os.path.join(OUT, fname)
        if not os.path.exists(path) and not gst('filesrc location="%s" ! mpegaudioparse ! mpg123audiodec ! audioconvert ! audioresample ! %s '
                                                 '! filesink location="%s"' % (src, enc, path)):
            print("FAILED  %s: GStreamer could not make it" % name); failed += 1; continue
        a, rate, info = flopos(path)
        if name in REFUSED:
            good = "not something FLOPOS plays" in info
            failed += not good
            print("%-7s %-22s refused, as it should be" % ("ok", name) if good else "FAILED  %s: played (%s)" % (name, info))
            continue
        if not rate or not len(a):
            print("FAILED  %s: %s" % (name, info or "nothing came out")); failed += 1; continue
        b = reference(path, how, rate)
        if b is None:
            print("FAILED  %s: GStreamer could not decode it" % name); failed += 1; continue
        cor, gain, lag = compare(a, b, rate)
        good = cor >= 0.98 and 0.5 <= gain <= 1.0
        failed += not good
        print("%-7s %-22s correlation %.4f  gain %.2f  (%s)" % ("ok" if good else "FAILED", name, cor, gain, info))
    sys.exit(1 if failed else 0)

if __name__ == "__main__":
    main()
