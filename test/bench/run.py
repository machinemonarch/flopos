#!/usr/bin/env python3
"""Boot FLOPOS in QEMU, open pages in browzer, and keep what came out.

    test/bench/run.py IMAGE OUT [--slow] [--shots] [--listen S] [--site SONGS] STEP ...

STEP is ADDRESS, or ADDRESS=>PATTERN: then the step passes if PATTERN (a
regular expression) turns up on the serial wire during it.  browzer says
there, one line each, which page came ("[page] 200, 12 links: title") and
the links it made itself.  Only those lines, or ok/FAILED, are printed.

ADDRESS is a full http:// address, a path on the local test site, or
key:a,b,... to press keys (QEMU's names) instead, such as key:h for home,
type:TEXT to type (\\n for enter), or shot: for one screenshot.
A path is on the local test site
(test/bench/site.py, started here on port 8150 if it isn't running).
The first one starts browzer; each later one is typed over the last.
After each, once the serial wire has been quiet a moment (and, with
--listen, S more seconds), --shots takes OUT/page-N.png.  Then:
  OUT/sound.wav    everything the Sound Blaster played
  OUT/serial.log   everything FLOPOS said on its serial wire

Waits are on FLOPOS's own messages on the serial wire, not fixed pauses.
The emulated machine always runs at about the laptop's speed.  By default
its clock may run ahead of the wall's -- about ten times as fast here --
which tests behaviour quickly; --slow keeps the two together, which any
timing (the [audio] reports) needs.
"""
import argparse, os, re, socket, subprocess, sys, time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import shots
from shots import Monitor, type_text

# Keys for an address on FLOPOS's Swedish layout, by QEMU's (US) key names.
SE = {":": "shift-dot", "/": "shift-7", "?": "shift-minus", "=": "shift-0", "&": "shift-6",
      "-": "slash", "_": "shift-slash", ".": "dot"}

class Wire:
    """The serial log as it grows."""
    def __init__(self, path): self.path, self.seen = path, 0
    def text(self):
        try: return open(self.path, "rb").read().decode("latin-1")
        except OSError: return ""
    def wait(self, pattern, timeout):
        end = time.time() + timeout
        while time.time() < end:
            t = self.text()
            m = re.search(pattern, t[self.seen:])
            if m: self.seen += m.end(); return True
            time.sleep(0.2)
        return False
    def quiet(self, secs, timeout):
        """Until nothing new has come for secs, at most timeout."""
        end, last, since = time.time() + timeout, -1, time.time()
        while time.time() < end:
            n = len(self.text())
            if n != last: last, since = n, time.time()
            elif time.time() - since >= secs: break
            time.sleep(0.2)
        self.seen = len(self.text())

def address(mon, a):
    if not a.startswith("http"): a = "http://10.0.2.2:8150/" + a.lstrip("/")
    for ch in a:
        if ch in SE: mon.cmd("sendkey " + SE[ch], settle=0.05)
        else: type_text(mon, ch)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image"); ap.add_argument("out"); ap.add_argument("addresses", nargs="+")
    ap.add_argument("--slow", action="store_true"); ap.add_argument("--listen", type=float, default=0)
    ap.add_argument("--shots", action="store_true")
    ap.add_argument("--site", default=os.path.expanduser("~/.cache/flopos-songs"))
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    shots.OUT = a.out
    site = None
    with socket.socket() as s:
        if s.connect_ex(("127.0.0.1", 8150)):
            site = subprocess.Popen([sys.executable, "-I", os.path.join(os.path.dirname(os.path.abspath(__file__)), "site.py"),
                                     "8150", a.site], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            time.sleep(0.5)
    log = os.path.join(a.out, "serial.log")
    if os.path.exists(log): os.remove(log)
    cmd = ["qemu-system-i386", "-machine", "pc", "-cpu", "pentium", "-m", "32",
           "-drive", "file=%s,if=floppy,format=raw,snapshot=on" % a.image, "-boot", "a", "-vga", "std", "-display", "none",
           "-monitor", "stdio", "-audiodev", "wav,id=snd1,path=%s" % os.path.join(a.out, "sound.wav"),
           "-machine", "pcspk-audiodev=snd1", "-device", "sb16,audiodev=snd1", "-device", "adlib,audiodev=snd1", "-serial", "file:" + log,
           "-netdev", "user,id=n0", "-device", "ne2k_isa,netdev=n0,iobase=0x300,irq=9"]
    cmd[1:1] = ["-icount", "shift=4,sleep=on,align=" + ("on" if a.slow else "off")]
    q = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    mon, wire = Monitor(q), Wire(log)
    try:
        if not wire.wait(r"your answer:", 90): sys.exit("no riddle on the serial wire")
        type_text(mon, "a turnip\n")
        if not wire.wait(r"here> ", 60): sys.exit("no prompt")
        type_text(mon, "net\n")
        if not wire.wait(r"names\s", 60): print("   (no network address)")
        failed = 0
        for i, step in enumerate(a.addresses):
            addr, _, want = step.partition("=>")
            start, began = len(wire.text()), time.time()
            if addr.startswith("key:"):                 # keys instead of an address
                for k in addr[4:].split(","): mon.cmd("sendkey " + k)
            elif addr.startswith("type:"):              # text, typed; \n is enter
                for line_no, part in enumerate(addr[5:].split("\\n")):
                    if line_no: mon.cmd("sendkey ret")
                    for ch in part:
                        if ch in SE: mon.cmd("sendkey " + SE[ch], settle=0.05)
                        else: type_text(mon, ch)
            elif addr.startswith("shot:"):              # a picture, only where the look is the test
                wire.quiet(1.0, 10); mon.shot("shot-%d" % i); print("shot   %s/shot-%d.png" % (a.out, i)); continue
            else:
                if i == 0: type_text(mon, "browzer ")
                else: mon.cmd("sendkey g"); mon.cmd("sendkey ctrl-a")
                address(mon, addr); mon.cmd("sendkey ret")
            if want:                                    # wait for what the step should say, however long it takes
                end = time.time() + 900
                while time.time() < end and not re.search(want, wire.text()[start:]): time.sleep(0.3)
                wire.quiet(1.0, 5)
            else: wire.quiet(2.5, 20)
            if a.listen: time.sleep(a.listen)
            if a.shots: mon.shot("page-%d" % i)
            said = [l for l in wire.text()[start:].splitlines() if l.startswith("[page]")]
            if want:
                ok = re.search(want, wire.text()[start:]) is not None
                failed += not ok
                print("%s  %s  (%.1f s)" % ("ok    " if ok else "FAILED", addr, time.time() - began) + ("" if ok else "\n        " + "\n        ".join(said[-6:])))
            else:
                print("%s\n        %s" % (addr, "\n        ".join(said[-6:])))
        if failed: print("%d failed" % failed)
    finally:
        q.terminate()
        if site: site.terminate()

if __name__ == "__main__":
    main()
