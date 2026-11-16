#!/usr/bin/env python3
"""Boot the image in headless QEMU, type at it, and save PNG frames.

The point is to see the actual pixels. A build that compiles and an OS that
renders a legible prompt are different claims, and only one of them can be
checked by looking.

    test/shots.py dist/flopos.img                  the default script
    test/shots.py dist/flopos.img -m 4             with 4MB of RAM
    test/shots.py dist/flopos.img --cpu 486        as a 486

QEMU's monitor is driven over a stdio pipe: 'sendkey' presses keys by name
and 'screendump' writes a PPM, which ImageMagick turns into a PNG.
"""
import argparse
import os
import shutil
import subprocess
import sys
import time

OUT = "dist/shots"


class Monitor:
    """QEMU's HMP monitor, over the process's own stdin.

    A unix socket would be tidier but is not available here, and stdio does
    the same job: commands go in, and the only reply that matters arrives as
    a file on disk.
    """

    def __init__(self, proc):
        self.proc = proc
        time.sleep(0.4)

    def cmd(self, line, settle=0.25):
        try:
            self.proc.stdin.write((line + "\n").encode())
            self.proc.stdin.flush()
        except (BrokenPipeError, OSError):
            sys.exit("QEMU exited early; monitor pipe is closed")
        time.sleep(settle)

    def shot(self, name):
        # A relative path on purpose: the monitor's screendump splits its
        # arguments on whitespace, so an absolute path containing a space
        # is read as a device name and fails.  QEMU inherits our working
        # directory, so this lands in the right place either way.
        ppm = os.path.join(OUT, name + ".ppm")
        if os.path.exists(ppm):
            os.remove(ppm)
        self.cmd("screendump %s" % ppm, settle=0.5)
        for _ in range(60):
            if os.path.exists(ppm) and os.path.getsize(ppm) > 0:
                size = os.path.getsize(ppm)
                time.sleep(0.1)
                if os.path.getsize(ppm) == size:
                    break
            time.sleep(0.1)
        else:
            print("   !! no frame captured for %s" % name)
            return
        png = ppm[:-4] + ".png"
        if shutil.which("convert"):
            subprocess.run(["convert", ppm, png], check=False)
            os.remove(ppm)
            print("   %s" % png)
        else:
            print("   %s" % ppm)


# Characters QEMU's sendkey needs spelled out, and the Swedish keys we want
# to exercise. The names are QEMU's US-layout key names: FLOPOS reads raw
# scancodes, so pressing the physically-correct key is what matters, and on
# a Swedish board that key has a different symbol printed on it.
NAMED = {
    " ": "spc", "-": "minus", "=": "equal", "[": "bracket_left",
    "]": "bracket_right", ";": "semicolon", "'": "apostrophe",
    ",": "comma", ".": "dot", "/": "slash", "\\": "backslash",
    "\n": "ret",
}


def answer_the_wizard(mon, settle=5.5):
    """Every boot stops at the wizard's riddle.  Any answer will do; then
    the jingle and the way into the shell take a few seconds."""
    type_text(mon, "a turnip\n")
    time.sleep(settle)


def type_text(mon, text, settle=0.06):
    for ch in text:
        if ch in NAMED:
            key = NAMED[ch]
        elif ch.isdigit():
            key = ch
        elif ch.isalpha():
            key = ch.lower()
        else:
            continue
        mon.cmd("sendkey " + key, settle=settle)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("-m", default="16", help="RAM in MB")
    ap.add_argument("--cpu", default="pentium")
    ap.add_argument("--tag", default="")
    ap.add_argument("--device", action="append", default=[],
                    help="extra -device for QEMU, e.g. adlib")
    ap.add_argument("--talk", action="store_true",
                    help="run the conversation script instead")
    ap.add_argument("--grid", action="store_true",
                    help="both text grids and the frame test card")
    args = ap.parse_args()

    os.makedirs(OUT, exist_ok=True)

    qemu = subprocess.Popen([
        "qemu-system-i386",
        "-machine", "pc",
        "-cpu", args.cpu,
        "-m", args.m,
        "-drive", "file=%s,if=floppy,format=raw" % args.image,
        "-boot", "a",
        "-vga", "std",
        "-display", "none",
        "-monitor", "stdio",
        # A null audio backend: the PC speaker and any OPL2 we add still
        # get driven, we just do not need a sound server to listen.
        "-audiodev", "none,id=snd0",
        "-machine", "pcspk-audiodev=snd0",
    ] + [a for d in args.device for a in ("-device", d)],
       stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
       stderr=subprocess.PIPE)

    tag = ("-" + args.tag) if args.tag else ""
    try:
        mon = Monitor(qemu)

        if args.grid:
            time.sleep(12.0)             # through the boot
            answer_the_wizard(mon)
            for name, keys in (
                ("grid-1-close-frames", ["frames\n"]),
                ("grid-2-close-help",   ["goaway\n", "help\n"]),
                ("grid-3-close-sys",    ["goaway\n", "sys\n", "whoami\n",
                                         "look\n", "version\n"]),
                ("grid-4-card",         ["goaway\n", "card\n"]),
                ("grid-5-roomy-frames", ["goaway\n", "squint\n",
                                         "frames\n"]),
                ("grid-6-roomy-help",   ["goaway\n", "help\n"]),
            ):
                for k in keys:
                    type_text(mon, k)
                    time.sleep(0.5)
                mon.shot(name + tag)
            return

        if args.talk:
            # The phrases that were actually typed at the real machine,
            # plus the ones it is obviously going to be asked next.
            time.sleep(12.0)
            answer_the_wizard(mon)
            script = [
                (["whoareyou", "\n"], None),
                (["who are you", "?", "\n"], None),
                (["moodier", "\n"], None),
                (["there", "\n"], "talk-1"),
                (["clear", "\n"], None),
                (["hello", "\n"], None),
                (["are you alive", "?", "\n"], None),
                (["how are you", "?", "\n"], None),
                (["what time is it", "?", "\n"], "talk-2"),
                (["clear", "\n"], None),
                (["thanks", "\n"], None),
                (["what can you do", "?", "\n"], "talk-3"),
                (["clear", "\n"], None),
                (["blargh", "\n"], None),
                (["what is the point of all this", "?", "\n"], None),
                (["whereami", "\n"], "talk-4"),
            ]
            for parts, shot in script:
                for part in parts:
                    if part == "?":
                        mon.cmd("sendkey shift-minus")   # '?' on a SE board
                    else:
                        type_text(mon, part)
                time.sleep(0.45)
                if shot:
                    mon.shot(shot + tag)
            return

        time.sleep(2.2)
        mon.shot("00a-arrive" + tag)
        time.sleep(1.6)
        mon.shot("00b-arrive" + tag)
        time.sleep(2.2)
        mon.shot("00c-arrive" + tag)
        time.sleep(4.0)
        mon.shot("01-wizard" + tag)        # the riddle at the gate
        type_text(mon, "a turnip\n")
        time.sleep(1.6)
        mon.shot("01b-cyberdelia" + tag)   # rippling on the way in
        time.sleep(4.0)
        mon.shot("02-prompt" + tag)

        type_text(mon, "whoami\n")
        time.sleep(0.4)
        mon.shot("03-whoami" + tag)

        type_text(mon, "help\n")
        time.sleep(0.5)
        mon.shot("04-help" + tag)

        type_text(mon, "clear\nsys\n")
        time.sleep(0.5)
        mon.shot("05-sys" + tag)

        type_text(mon, "clear\nspeed\n")
        time.sleep(1.5)
        mon.shot("05b-speed" + tag)

        type_text(mon, "clear\nmood deep\n")
        time.sleep(1.2)
        mon.shot("06-mood-deep" + tag)

        type_text(mon, "mood void\n")
        time.sleep(1.2)
        mon.shot("07-mood-void" + tag)

        # The symbol keys a Swedish keyboard moves, plus the three letters
        # it adds. On a Swedish board these physical keys print " / @ and
        # the A-ring, so this is checking the keymap, not the ASCII.
        type_text(mon, "clear\n")
        mon.cmd("sendkey e")
        mon.cmd("sendkey c")
        mon.cmd("sendkey h")
        mon.cmd("sendkey o")
        mon.cmd("sendkey spc")
        mon.cmd("sendkey shift-2")          # " on a Swedish keyboard
        mon.cmd("sendkey shift-7")          # /
        mon.cmd("sendkey bracket_left")     # the A-ring key
        mon.cmd("sendkey semicolon")        # the O-diaeresis key
        mon.cmd("sendkey apostrophe")       # the A-diaeresis key
        mon.cmd("sendkey slash")            # - on a Swedish keyboard
        mon.cmd("sendkey alt_r-2")          # AltGr-2 gives @
        mon.cmd("sendkey alt_r-7")          # AltGr-7 gives {
        mon.cmd("sendkey ret")
        time.sleep(0.5)
        mon.shot("08-swedish" + tag)

        type_text(mon, "clear\nfrobnicate\n")
        time.sleep(0.4)
        mon.shot("09-unknown" + tag)

        type_text(mon, "time\n")
        time.sleep(0.4)
        mon.shot("10-time" + tag)
    finally:
        qemu.terminate()
        try:
            qemu.wait(timeout=5)
        except subprocess.TimeoutExpired:
            qemu.kill()
        err = qemu.stderr.read().decode(errors="replace").strip()
        if err:
            print("qemu said:\n" + err)


if __name__ == "__main__":
    main()
