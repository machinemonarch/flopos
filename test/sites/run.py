#!/usr/bin/env python3
"""The scoreboard: browzer's scripts on common sites, graded.

    test/sites/run.py [IMAGE] [--slow] [--only WORD,WORD] [--jobs N]

Boots the image once (dist/flopos.img unless named), and for each site in
sites.txt: opens it, allows its scripts (j), and reads the serial wire.
Each site gets:
  runs      every script it fetched ran; none stopped, none too big
  works     its check (sites.txt) turned up, or, without one, the page came
  seconds   from j to the scripts' summary, on the bench's clock (--slow:
            the laptop's speed, for whether it is bearable)
Prints a line a site and the totals; build/sites/scoreboard.txt keeps it.
--jobs N (default: twice the PC's cores; each mostly waits) boots N machines
at once, each with its share of the sites.  Needs the network.
"""
import os, re, subprocess, sys, time, threading, importlib.util

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
    jobs = int(args[args.index("--jobs") + 1]) if "--jobs" in args else 2 * (os.cpu_count() or 2)   # each machine mostly waits on the network: ~20% of a core, 75 MB
    image = next((a for a in args if a.endswith(".img")), os.path.join(ROOT, "dist", "flopos.img"))
    out = os.path.join(ROOT, "build", "sites"); os.makedirs(out, exist_ok=True)
    cmd = ["qemu-system-i386", "-icount", "shift=4,sleep=on,align=" + ("on" if slow else "off"), "-machine", "pc", "-cpu", "pentium", "-m", "32",
           "-drive", "file=%s,if=floppy,format=raw,snapshot=on" % image, "-boot", "a", "-vga", "std", "-display", "none",
           "-monitor", "stdio", "-audiodev", "none,id=snd1", "-device", "sb16,audiodev=snd1",
           "-netdev", "user,id=n0", "-device", "ne2k_isa,netdev=n0,iobase=0x300,irq=9"]
    boots, lock, deaf = [], threading.Lock(), [0]
    def boot(w):
        """A fresh machine at browzer's door: its own serial log (serial-W-N.log)."""
        with lock: boots.append(None); log = os.path.join(out, "serial-%d-%d.log" % (w, len(boots)))
        if os.path.exists(log): os.remove(log)
        q = subprocess.Popen(cmd + ["-serial", "file:" + log], stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        with lock: boots[boots.index(None)] = q
        mon, wire = bench.Monitor(q), bench.Wire(log)
        if not wire.wait(r"your answer:", 90): sys.exit("FAILED: no riddle on the serial wire")
        bench.type_text(mon, "a turnip\n")
        if not wire.wait(r"here> ", 60): sys.exit("FAILED: no prompt")
        bench.type_text(mon, "net\n"); wire.wait(r"names\s", 60)
        return q, mon, wire
    rows = []
    board = open(os.path.join(out, "scoreboard.txt"), "w")   # written as it goes: a cut-short run keeps what it had
    board.write(time.strftime("%Y-%m-%d %H:%M") + ("  (laptop speed)" if slow else "") + "\n"); board.flush()
    def show(r): return "%-44s runs %-4s works %-3s %5.1f s  %s" % ((r[0][:44],) + tuple(r[1:]))
    def row(r):
        with lock:
            rows.append(r)
            print(show(r), flush=True); board.write(show(r) + "\n"); board.flush()
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
        wire.wait(r"Scripts: \d+ ran[^\n]*|none of them run|no scripts|\[page\] [45]\d\d", 300)   # an error page has no summary
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
        if re.search(r"\[page\] 4\d\d, \d+ links: (Just a moment|Attention Required|Blocked)|AwsWafIntegration", page): why = "bot check: out of reach"
        return (url, runs, works, secs, why)
    def worker(w, mine):
        q, mon, wire = boot(w); first, stuck = True, False
        for url, want in mine:
            if stuck: q.terminate(); q, mon, wire = boot(w); first = True   # the last page hung it: a fresh one
            r = visit(mon, wire, url, want, first); first, stuck = False, False
            if r is None and re.search(r"Nobody knows", wire.text()[-400:]):
                # The network can go deaf for good (a fault of its own, not the
                # scripts'): try the site once more on a fresh machine.
                with lock: deaf[0] += 1
                q.terminate(); q, mon, wire = boot(w)
                r = visit(mon, wire, url, want, True)
            if r is None: r, stuck = (url, "-", "no", 0, "the page did not come"), True
            row(r)
        q.terminate()                                   # done: its memory back at once
    todo = [(u, w) for u, w, outside in sites() if not outside and not (only and not any(o in u for o in only.split(",")))]
    jobs = max(1, min(jobs, len(todo)))
    threads = [threading.Thread(target=worker, args=(i + 1, todo[i::jobs])) for i in range(jobs)]
    try:
        for t in threads: t.start()
        for t in threads: t.join()
    finally:
        for b in boots:
            if b: b.terminate()
    order = {u: i for i, (u, w) in enumerate(todo)}
    rows.sort(key=lambda r: order[r[0]])
    board.close(); board = open(os.path.join(out, "scoreboard.txt"), "w")   # again, in the list's order
    board.write(time.strftime("%Y-%m-%d %H:%M") + ("  (laptop speed)" if slow else "") + ", %d at once\n" % jobs)
    for r in rows: board.write(show(r) + "\n")
    graded = [r for r in rows]
    if len(boots) > jobs:
        t = "(%d fresh boots: %d after the network went deaf, %d after a page that did not come)" % (len(boots) - jobs, deaf[0], len(boots) - jobs - deaf[0])
        print(t); board.write(t + "\n")
    bots = sum(r[4].startswith("bot check") for r in rows)
    line = "%d sites (%d behind a bot check): scripts all run on %d, partly on %d, none run on %d; works on %d" % (
        len(graded), bots, sum(r[1] == "yes" for r in graded), sum(r[1] == "part" for r in graded), sum(r[1] == "none" for r in graded), sum(r[2] == "yes" for r in graded))
    print(line)
    board.write(line + "\n"); board.close()

if __name__ == "__main__":
    main()
