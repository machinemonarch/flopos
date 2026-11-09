# Philosophy

## A place, not a directory

Most command lines put you in a folder. FLOPOS puts you somewhere. The
prompt is `here>`, and `here` is a space, not a path. Ask it who you are and
it does not print a username:

    here> whoami
    You're alright.

It is not a demo and it is not a Linux. It is a small operating system
written from nothing: a boot sector, a protected-mode kernel, its own
drivers for the screen, keyboard, sound, disk, serial line and network card,
its own network stack and encryption, and a shell. All of it fits on one
floppy disk with most of the disk left over.

## Who makes it

FLOPOS is an art project. Max directs it: what it is called, how it looks,
how it feels to use, and what it says. Claude writes the machine. Max uses
it on the real laptop, which Claude cannot touch, and the laptop reports
back in two ways:

- **The journal.** `JOURNAL.TXT` on the floppy is plain text. Among other
  things it lists every word typed that FLOPOS did not know. That list is
  the to-do list for new commands.
- **The report.** `report` sends everything FLOPOS knows about the machine
  down the serial cable in one go, for diagnosing problems.

## The rules it is built by

**The constraints are the point.** One floppy. One laptop from 1996: a
120MHz Pentium, 32MB of memory, an 800×600 screen. Everything FLOPOS does,
it does on that hardware alone. No proxy servers doing the hard work
somewhere else and no helper machines. This rule stands until Max says
otherwise.

**No JavaScript.** A JavaScript engine is large, and it is the main way web
pages attack the computer reading them. browzer reads the web without one
and shows each page's no-JavaScript version where the page has one.

**Ask before anything risky.** Features that open the machine up to the
outside world are discussed before they are built.

**Make the nice things the default.** If one way is clearly better (the
sharper 800×600 screen, the smoother text), it is simply what you get. You
shouldn't have to know a command to get the good version.

**Move fast, but never freeze.** It is fine for a feature to arrive rough
and improve. It is not fine for the machine to hang. Every freeze found so
far was traced to its cause and fixed where it started, and the clock has a
watchdog that restarts it if it ever stops.

**Say what is true.** When something fails, FLOPOS says why in a sentence.
When something is not secure, it says that on the screen. Even the boot
messages are true. "Aligning the realm to the beat of the crystal..." is
the timer being started, which really does run off a crystal. "Flippely-
Flopping" is the floppy being read. They are just phrased in a dialect of
their own.

**Everything it keeps is readable anywhere.** The floppy is a normal DOS
disk. The journal is plain text, and documents are Markdown. Put the disk
in any computer and you can read what is on it.

**Clean provenance.** Everything is written from scratch against published
standards. Toshiba's manuals and drivers were consulted for facts such as
port numbers and chip names, but none of their code, firmware or text is in
FLOPOS. See [References](references.md).

**Every release is numbered, kept and described.** Each image gets a new
version number and an archived copy, and nothing is released without a
changelog entry. See [Building and releasing](building.md).

## The voice

FLOPOS speaks in a warm deadpan. It is polite, a little dry, and never says
more than it needs to.

    here> how are you
    Still running. Kind of you to ask.

    here> are you alive
    No. But I'm here.

    here> i love you
    That's kind. I'm a floppy disk.

Every sentence it says lives in one file, `kernel/voice.c`, so the whole
register can be changed in one place. Command names say what they do for
you, not how they work: `goaway` clears the screen, `remember` saves,
`bye` switches the machine off.

## What it is made of

- A **moving background** behind the text. It is generated, kept dark so
  the text always wins, and breathes with the music.
- **Music**: a step sequencer playing rhythms on the laptop's FM sound chip.
- **A banana** in the corner, in wireframe, tumbling on two axes.
- **Saffran** (a cat) on the boot screen. She is the first thing read off
  the floppy and appears as fast as the drive delivers her. The machine
  then tells her, step by step, everything it is really doing to get
  ready.
- **whaaat**: the whole screen pulled through a hole in itself and turned
  inside out.
