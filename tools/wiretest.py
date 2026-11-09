#!/usr/bin/env python3
"""Boot FLOPOS in QEMU with a serial port and talk to it over the wire.

No keyboard, no monitor, no screenshots -- if this works, the only channel
in or out is the serial line, which is exactly the thing being tested.
"""
import os, re, subprocess, sys, time

img = sys.argv[1] if len(sys.argv) > 1 else "dist/flopos.img"

qemu = subprocess.Popen(
    ["qemu-system-i386", "-machine", "pc", "-cpu", "pentium", "-m", "16",
     "-drive", "file=%s,if=floppy,format=raw" % img, "-boot", "a",
     "-vga", "std", "-display", "none", "-monitor", "none",
     "-serial", "pty"],
    stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

pts = None
for _ in range(100):
    line = qemu.stdout.readline().decode(errors="replace")
    if not line:
        break
    m = re.search(r"(/dev/pts/\d+)", line)
    if m:
        pts = m.group(1)
        break
if not pts:
    qemu.kill()
    sys.exit("QEMU did not give us a pty")

print("serial line is %s" % pts)
fd = os.open(pts, os.O_RDWR | os.O_NONBLOCK)


def read_all(seconds):
    out = b""
    end = time.time() + seconds
    while time.time() < end:
        try:
            chunk = os.read(fd, 4096)
            if chunk:
                out += chunk
                end = time.time() + 0.35
        except BlockingIOError:
            time.sleep(0.05)
        except OSError:
            break
    return out.decode("latin1")


def send(text):
    os.write(fd, text.encode())
    time.sleep(0.15)


try:
    boot = read_all(5.0)
    print("\n--- what came down the wire while it booted ---")
    print(boot.strip() or "(nothing)")

    # Every boot stops at the wizard's riddle first.  Any answer will do,
    # and the way into the shell takes a few seconds.
    for cmd, wait in [("a turnip\r", 6.0), ("disk\r", 2.0)]:
        send(cmd)
        reply = read_all(wait)
        print("\n--- after sending %r ---" % cmd)
        print(reply.strip() or "(nothing)")
finally:
    os.close(fd)
    qemu.terminate()
    try:
        qemu.wait(timeout=5)
    except subprocess.TimeoutExpired:
        qemu.kill()
