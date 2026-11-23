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
    cmd = ["qemu-system-i386", "-icount", "shift=4,sleep=on,align=" + ("on" if slow else "off"), "-machine", "pc", "-cpu", "pentium", "-m", "32",
           "-drive", "file=%s,if=floppy,format=raw,snapshot=on" % image, "-boot", "a", "-vga", "std", "-display", "none",
           "-monitor", "stdio", "-audiodev", "none,id=snd1", "-device", "sb16,audiodev=snd1",
           "-netdev", "user,id=n0", "-device", "ne2k_isa,netdev=n0,iobase=0x300,irq=9"]
    boots = []
    def boot():
        """A fresh machine at browzer's door: its own serial log (serial-N.log)."""
        log = os.path.join(out, "serial-%d.log" % (len(boots) + 1))
        if os.path.exists(log): os.remove(log)
        q = subprocess.Popen(cmd + ["-serial", "file:" + log], stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        boots.append(q)
        mon, wire = bench.Monitor(q), bench.Wire(log)
        if not wire.wait(r"your answer:", 90): sys.exit("FAILED: no riddle on the serial wire")
        bench.type_text(mon, "a turnip\n")
        if not wire.wait(r"here> ", 60): sys.exit("FAILED: no prompt")
        bench.type_text(mon, "net\n"); wire.wait(r"names\s", 60)
        return q, mon, wire
    rows = []
    board = open(os.path.join(out, "scoreboard.txt"), "w")   # written as it goes: a cut-short run keeps what it had
    board.write(time.strftime("%Y-%m-%d %H:%M") + ("  (laptop speed)" if slow else "") + "\n"); board.flush()
    def row(r):
        rows.append(r)
        t = "%-44s runs %-4s works %-3s %5.1f s  %s" % ((r[0][:44],) + tuple(r[1:]))
        print(t, flush=True); board.write(t + "\n"); board.flush()
    def visit(mon, wire, url, want, first):
        """None if the page did not come; else the site's row."""
        if first: bench.type_text(mon, "browzer ")
        else: mon.cmd("sendkey g"); mon.cmd("sendkey ctrl-a")
        at = len(wire.text())
        bench.address(mon, url); mon.cmd("sendkey ret")
        if not wire.wait(r"\[page\] \d+|It keeps sending|could not|cannot|Nobody knows", 240) or not re.search(r"\[page\] \d+", wire.text()[at:]):
            return None
        wire.quiet(2.0, 30)
        start, t0 = len(wire.text()), time.time()
        mon.cmd("sendkey j")
        wire.wait(r"Scripts: \d+ ran[^\n]*|none of them run|no scripts", 300)
        secs = time.time() - t0
        wire.quiet(3.0, 60)
        seg, page = wire.text()[start:], wire.text()[at:]
        ran = len(re.findall(r"\[script\] ran ", seg))
        stopped = len(re.findall(r"\[script\] stopped: ", seg))
        big = len(re.findall(r"\[script\] too big: ", seg))
        other = len(re.findall(r"not fetched, another site", seg))
        runs = "none" if not ran and not stopped and not big else "yes" if not stopped and not big else "part" if ran else "no"
        works = "yes" if (re.search(want, page) if want else re.search(r"\[page\] 200", page)) else "no"
        why = ""
        m = re.search(r"\[script\] stopped: ([^\n]*)", seg) or re.search(r"\[script\] too big: ([^\n]*)", seg)
        if m: why = m.group(1)[:70]
        elif other: why = "%d from another site not fetched" % other
        if not why and re.search(r"too big to build", page): why = "page too big: scripts not run"
        return (url, runs, works, secs, why)
    try:
        q, mon, wire = boot(); first = True
        for url, want, outside in sites():
            if outside or (only and only not in url): continue
            r = visit(mon, wire, url, want, first); first = False
            if r is None:
                # The network can go deaf for good (a fault of its own, not the
                # scripts'): try the site once more on a fresh machine.
                q.terminate(); q, mon, wire = boot()
                r = visit(mon, wire, url, want, True)
                if r is None: r = (url, "-", "no", 0, "the page did not come")
            row(r)
    finally:
        for b in boots: b.terminate()
    graded = [r for r in rows]
    if len(boots) > 1: print("(%d fresh boots: the network went deaf)" % (len(boots) - 1)); board.write("(%d fresh boots: the network went deaf)\n" % (len(boots) - 1))
    line = "%d sites: scripts all run on %d, partly on %d, none run on %d; works on %d" % (
        len(graded), sum(r[1] == "yes" for r in graded), sum(r[1] == "part" for r in graded), sum(r[1] == "none" for r in graded), sum(r[2] == "yes" for r in graded))
    print(line)
    board.write(line + "\n"); board.close()

if __name__ == "__main__":
    main()
