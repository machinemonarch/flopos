#!/usr/bin/env python3
"""Talk to FLOPOS over the serial line, and keep a transcript.

    test/wire.py                 find the adapter, connect at 115200
    test/wire.py /dev/ttyUSB0    name the port yourself
    test/wire.py --log session   write the transcript somewhere specific

Type to drive the machine. Ctrl+] to quit.

Everything in both directions is written to a log file, so the whole
session can be handed to someone who was not in the room -- which is the
point. No pyserial needed; this talks to the tty directly.
"""
import argparse, glob, os, select, sys, termios, time, tty

BAUD = termios.B115200


def find_port():
    for pattern in ("/dev/ttyUSB*", "/dev/ttyACM*", "/dev/ttyS[0-9]"):
        hits = sorted(glob.glob(pattern))
        if hits:
            return hits[0]
    return None


def configure(fd):
    """115200 8N1, no flow control, raw."""
    attrs = termios.tcgetattr(fd)
    iflag, oflag, cflag, lflag, ispeed, ospeed, cc = attrs

    iflag = 0                              # no translation, no XON/XOFF
    oflag = 0
    lflag = 0                              # raw: no echo, no line editing
    cflag = termios.CS8 | termios.CREAD | termios.CLOCAL
    cc = list(cc)
    cc[termios.VMIN] = 0
    cc[termios.VTIME] = 0

    termios.tcsetattr(fd, termios.TCSANOW,
                      [iflag, oflag, cflag, lflag, BAUD, BAUD, cc])
    termios.tcflush(fd, termios.TCIOFLUSH)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port", nargs="?")
    ap.add_argument("--log", default=None)
    args = ap.parse_args()

    port = args.port or find_port()
    if not port:
        sys.exit("No serial adapter found. Plug it in, or name the port:\n"
                 "  test/wire.py /dev/ttyUSB0")
    if not os.access(port, os.R_OK | os.W_OK):
        sys.exit("Cannot open %s. Either run with sudo, or add yourself to\n"
                 "the dialout group once and log back in:\n"
                 "  sudo usermod -aG dialout $USER" % port)

    logname = args.log or ("dist/wire-%s.log" % time.strftime("%Y%m%d-%H%M%S"))
    os.makedirs(os.path.dirname(logname) or ".", exist_ok=True)
    log = open(logname, "w", buffering=1, errors="replace")

    fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    configure(fd)

    print("connected to %s at 115200" % port)
    print("logging to %s" % logname)
    print("Ctrl+] to quit. Press Enter if nothing appears -- FLOPOS only")
    print("speaks when spoken to, unless it is still booting.\n")

    stdin_fd = sys.stdin.fileno()
    saved = termios.tcgetattr(stdin_fd)
    tty.setraw(stdin_fd)

    try:
        while True:
            ready, _, _ = select.select([fd, stdin_fd], [], [], 0.2)

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

            if stdin_fd in ready:
                ch = os.read(stdin_fd, 64)
                if b"\x1d" in ch:          # Ctrl+]
                    break
                ch = ch.replace(b"\n", b"\r")   # the far end wants CR
                os.write(fd, ch)
                log.write(ch.decode("latin1").replace("\r", "\n"))
    finally:
        termios.tcsetattr(stdin_fd, termios.TCSADRAIN, saved)
        os.close(fd)
        log.close()
        print("\n\ndisconnected. transcript: %s" % logname)


if __name__ == "__main__":
    main()
