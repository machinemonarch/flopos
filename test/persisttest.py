#!/usr/bin/env python3
"""Prove FLOPOS can write to its own disk, and that what it wrote survives.

Boots a throwaway copy of the image, types a word it does not know over the serial
line, shuts QEMU down, and then checks the result from outside with
mtools -- because the OS claiming it saved something is not evidence.
Then boots the same disk again to see whether it remembers.
"""
import os, re, shutil, subprocess, sys, time

img = "build/persisttest/test.img"
os.makedirs(os.path.dirname(img), exist_ok=True)
shutil.copyfile(sys.argv[1] if len(sys.argv) > 1 else "dist/flopos.img", img)


def boot(seconds_before=22.0):
    q = subprocess.Popen(
        ["qemu-system-i386", "-machine", "pc", "-cpu", "pentium", "-m", "16",
         "-drive", "file=%s,if=floppy,format=raw" % img, "-boot", "a",
         "-vga", "std", "-display", "none", "-monitor", "none",
         "-serial", "pty"],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    pts = None
    for _ in range(100):
        line = q.stdout.readline().decode(errors="replace")
        if not line:
            break
        m = re.search(r"(/dev/pts/\d+)", line)
        if m:
            pts = m.group(1); break
    if not pts:
        q.kill(); sys.exit("no pty")
    # Open the line at once and keep reading it. Leaving a pty unread
    # while the guest writes to it loses the output and can stall QEMU
    # against a full buffer, which looks exactly like the OS hanging.
    fd = os.open(pts, os.O_RDWR | os.O_NONBLOCK)
    end = time.time() + seconds_before
    early = b""
    while time.time() < end:
        try:
            c = os.read(fd, 4096)
            if c:
                early += c
        except (BlockingIOError, OSError):
            pass
        time.sleep(0.05)
    return q, fd, early.decode("latin1")


def drain(fd, seconds=2.0):
    out = b""
    end = time.time() + seconds
    while time.time() < end:
        try:
            c = os.read(fd, 4096)
            if c:
                out += c; end = time.time() + 0.4
        except (BlockingIOError, OSError):
            time.sleep(0.05)
    return out.decode("latin1")


def send(fd, text):
    os.write(fd, text.encode()); time.sleep(0.2)


print("=== first boot: a word it does not know ===")
q, fd, early = boot(38.0)
print("[boot said]", early.strip()[-320:])
send(fd, 'a turnip\r')                  # the wizard's riddle: anything will do
drain(fd, 6.0)
send(fd, 'blargwotsit\r')
drain(fd, 2.0)
send(fd, 'remember\r')
print(drain(fd, 4.0).strip())
os.close(fd); q.terminate(); q.wait(timeout=10)

print("\n=== what is actually on the disk now ===")
j = subprocess.run(["mtype", "-i", img, "::JOURNAL.TXT"],
                   capture_output=True, text=True).stdout
j = j.replace("\x00", "").replace("\x1a", "")
print(j.strip())

print("\n=== second boot: does it remember? ===")
q, fd, early = boot(38.0)
print("[boot said]", early.strip()[-320:])
send(fd, 'a turnip\r')
drain(fd, 6.0)
send(fd, 'journal\r')
print(drain(fd, 3.0).strip())
os.close(fd); q.terminate(); q.wait(timeout=10)
