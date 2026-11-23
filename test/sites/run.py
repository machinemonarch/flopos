#!/usr/bin/env python3
"""The scoreboard: browzer's scripts on common sites, graded.

    test/sites/run.py [IMAGE] [--slow] [--only WORD]

Boots the image once (dist/flopos.img unless named), and for each site in
sites.txt: opens it, allows its scripts (j), and reads the serial wire.
Each site gets:
  runs      every script it fetched ran; none stopped, none too big
  works     its check (sites.txt) turned up, or, without one, the page came
  seconds   from j to the scripts' summary, on the bench's clock (--slow:
            the laptop's speed, for whether it is bearable)
Prints a line a site and the totals; build/sites/scoreboard.txt keeps it.
Needs the network.
"""
import os, re, subprocess, sys, time, importlib.util

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
spec = importlib.util.spec_from_file_location("benchrun", os.path.join(ROOT, "test", "bench", "run.py"))
bench = importlib.util.module_from_spec(spec); spec.loader.exec_module(bench)

def sites():
    for line in open(os.path.join(HERE, "sites.txt")):
        line = line.strip()
        if not line or line.startswith("#"): continue
        out = line.startswith("out ")
        if out: line = line[4:]
        url, _, want = line.partition(" ")
        yield url, want.strip(), out

def main():
    args = [a for a in sys.argv[1:]]
    slow = "--slow" in args
    only = args[args.index("--only") + 1] if "--only" in args else None
    image = next((a for a in args if a.endswith(".img")), os.path.join(ROOT, "dist", "flopos.img"))
    out = os.path.join(ROOT, "build", "sites"); os.makedirs(out, exist_ok=True)
    log = os.path.join(out, "serial.log")
    if os.path.exists(log): os.remove(log)
    cmd = ["qemu-system-i386", "-icount", "shift=4,sleep=on,align=" + ("on" if slow else "off"), "-machine", "pc", "-cpu", "pentium", "-m", "32",
           "-drive", "file=%s,if=floppy,format=raw,snapshot=on" % image, "-boot", "a", "-vga", "std", "-display", "none",
           "-monitor", "stdio", "-serial", "file:" + log, "-audiodev", "none,id=snd1", "-device", "sb16,audiodev=snd1",
           "-netdev", "user,id=n0", "-device", "ne2k_isa,netdev=n0,iobase=0x300,irq=9"]
    q = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    mon, wire = bench.Monitor(q), bench.Wire(log)
    rows, first = [], True
    board = open(os.path.join(out, "scoreboard.txt"), "w")   # written as it goes: a cut-short run keeps what it had
    board.write(time.strftime("%Y-%m-%d %H:%M") + ("  (laptop speed)" if slow else "") + "\n"); board.flush()
    def row(r):
        rows.append(r)
        t = "%-44s runs %-4s works %-3s %5.1f s  %s" % (r[0][:44],) + r[1:]
        print(t, flush=True); board.write(t + "\n"); board.flush()
    try:
        if not wire.wait(r"your answer:", 90): sys.exit("FAILED: no riddle on the serial wire")
        bench.type_text(mon, "a turnip\n")
        if not wire.wait(r"here> ", 60): sys.exit("FAILED: no prompt")
        bench.type_text(mon, "net\n"); wire.wait(r"names\s", 60)
        for url, want, outside in sites():
            if outside or (only and only not in url): continue
            if first: bench.type_text(mon, "browzer "); first = False
            else: mon.cmd("sendkey g"); mon.cmd("sendkey ctrl-a")
            bench.address(mon, url); mon.cmd("sendkey ret")
            if not wire.wait(r"\[page\] \d+|It keeps sending|could not|cannot", 240):
                row((url, "-", "no", 0, "the page did not come")); continue
            wire.quiet(2.0, 30)
            start, t0 = len(wire.text()), time.time()
            mon.cmd("sendkey j")
            wire.wait(r"Scripts: \d+ ran[^\n]*|none of them run|no scripts", 300)
            secs = time.time() - t0
            wire.quiet(3.0, 60)
            seg = wire.text()[start:]
            ran = len(re.findall(r"\[script\] ran ", seg))
            stopped = len(re.findall(r"\[script\] stopped: ", seg))
            big = len(re.findall(r"\[script\] too big: ", seg))
            other = len(re.findall(r"not fetched, another site", seg))
            runs = "none" if not ran and not stopped and not big else "yes" if not stopped and not big else "part" if ran else "no"
            works = "yes" if (re.search(want, seg) if want else re.search(r"\[page\] 200", seg)) else "no"
            why = ""
            m = re.search(r"\[script\] stopped: ([^\n]*)", seg) or re.search(r"\[script\] too big: ([^\n]*)", seg)
            if m: why = m.group(1)[:70]
            elif other: why = "%d from another site not fetched" % other
            if not why and re.search(r"too big to build", seg): why = "page too big: scripts not run"
            row((url, runs, works, secs, why))
    finally:
        q.terminate()
    graded = [r for r in rows]
    line = "%d sites: scripts all run on %d, partly on %d; works on %d" % (
        len(graded), sum(r[1] in ("yes", "none") for r in graded), sum(r[1] == "part" for r in graded), sum(r[2] == "yes" for r in graded))
    print(line)
    board.write(line + "\n"); board.close()

if __name__ == "__main__":
    main()
