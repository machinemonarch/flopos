#!/usr/bin/env python3
"""Talk to the laptop over the serial line, and keep it all in one log.

    tools/laptop/wire.py                 find the adapter, connect at 115200
    tools/laptop/wire.py /dev/ttyUSB0    name the port yourself
    make wire                            the same

Type to drive the machine; Ctrl+] to quit. Without a keyboard (started in
the background) it only listens.

Everything in both directions is added to logs/serial.log, one file for
every session, each starting with a dated line. No pyserial needed; this
talks to the tty directly. The port needs the dialout group (or sudo).
"""
import argparse, glob, os, select, sys, termios, time, tty

BAUD = termios.B115200
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LOG = os.path.join(ROOT, "logs", "serial.log")


def find_port():
    for pattern in ("/dev/ttyUSB*", "/dev/ttyACM*", "/dev/ttyS[0-9]"):
        hits = sorted(glob.glob(pattern))
        if hits:
            return hits[0]
    return None


def configure(fd):
    """115200 8N1, no flow control, raw."""
    cc = list(termios.tcgetattr(fd)[6])
    cc[termios.VMIN] = 0
    cc[termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW,
                      [0, 0, termios.CS8 | termios.CREAD | termios.CLOCAL, 0, BAUD, BAUD, cc])
    termios.tcflush(fd, termios.TCIOFLUSH)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port", nargs="?")
    args = ap.parse_args()

    port = args.port or find_port()
    if not port:
        sys.exit("No serial adapter found. Plug it in, or name the port:\n"
                 "  tools/laptop/wire.py /dev/ttyUSB0")
    if not os.access(port, os.R_OK | os.W_OK):
        sys.exit("Cannot open %s. Add yourself to the dialout group once and\n"
                 "log back in:  sudo usermod -aG dialout $USER" % port)

    os.makedirs(os.path.dirname(LOG), exist_ok=True)
    log = open(LOG, "a", buffering=1, errors="replace")
    log.write("\n=== %s, %s ===\n" % (time.strftime("%Y-%m-%d %H:%M:%S"), port))

    fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    configure(fd)

    keys = sys.stdin.isatty()
    print("connected to %s at 115200, adding to %s" % (port, os.path.relpath(LOG)))
    if keys:
        print("Ctrl+] to quit. Press Enter if nothing appears -- FLOPOS only")
        print("speaks when spoken to, unless it is still booting.\n")
    stdin_fd = sys.stdin.fileno()
    saved = termios.tcgetattr(stdin_fd) if keys else None
    if keys:
        tty.setraw(stdin_fd)
    watch = [fd, stdin_fd] if keys else [fd]

    try:
        while True:
            ready, _, _ = select.select(watch, [], [], 0.2)
            if fd in ready:
                try:
                    data = os.read(fd, 4096)
                except (BlockingIOError, OSError):
                    data = b""
                if data:
                    text = data.decode("latin1")
                    sys.stdout.write(text)
                    sys.stdout.flush()
                    log.write(text)
            if keys and stdin_fd in ready:
                ch = os.read(stdin_fd, 64)
                if b"\x1d" in ch:          # Ctrl+]
                    break
                ch = ch.replace(b"\n", b"\r")   # the far end wants CR
                os.write(fd, ch)
                log.write(ch.decode("latin1").replace("\r", "\n"))
    except KeyboardInterrupt:
        pass
    finally:
        if keys:
            termios.tcsetattr(stdin_fd, termios.TCSADRAIN, saved)
        os.close(fd)
        log.close()
        print("\n\ndisconnected.")


if __name__ == "__main__":
    main()
