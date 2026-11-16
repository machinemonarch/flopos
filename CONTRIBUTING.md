# Contributing

FLOPOS is an art project and it is Max's: he directs it and Claude builds
it, so what goes into this repository is Max's call. Ideas and bug reports
are welcome; a report from a real 430CDT (or any machine of its age) is
worth the most. And since it is under the MIT License, you are free to
fork it and take it wherever you like.

## The rules it is built by

**The constraints are the point.** One floppy. One laptop from 1996: a
120MHz Pentium, 32MB of memory, an 800×600 screen. No outside machine
does FLOPOS's own computing for it.

**JavaScript only where it is allowed, and walled off.** A JavaScript
engine is large, and it is the main way web pages attack the computer
reading them. So browzer runs none until a site is allowed, never runs
another site's scripts, and runs the rest in a room of its own that can
reach nothing but the page.

**Ask before anything risky.** Features that open the machine up to the
outside world are discussed before they are built.

**Make the nice things the default.** If one way is clearly better (the
sharper 800×600 screen, the smoother text), it is simply what you get. You
shouldn't have to know a command to get the good version.

**Move fast, but never freeze.** It is fine for a feature to arrive rough
and improve. It is not fine for the machine to hang. Every freeze found so
far was traced to its cause and fixed where it started, and the clock has
a watchdog that restarts it if it ever stops.

**Say what is true.** Plain words on the screen, when not whimsical, and a
sentence saying why when something fails. When something is not secure,
FLOPOS says so on the screen.

**Everything it keeps is readable anywhere.** The floppy is a normal DOS
disk, the journal is plain text, and documents are Markdown. Put the disk
in any computer and you can read what is on it. The one exception is what
you choose to encrypt, such as a cassette backup with a passphrase.

**Clean provenance.** Nothing copied from Toshiba or anyone else's code.
Everything is written from scratch against published standards, and the
standards, and the sources of inspiration, are credited in the
references[^1].

**Every release is numbered, kept and described.** Each image gets a new
version number and an archived copy, and every change is described in
CHANGELOG.md[^2].

## Where to start

- [Building](docs/development/building.md): building the image and running it in the emulator
- [Testing](docs/development/testing.md): the tests, and the tools in `tools/` that run them
- [Making apps](docs/development/making-apps.md): programs that live on the floppy beside the system
- [Releasing](docs/development/releasing.md): version numbers, the changelog, and making a release
- [Architecture](docs/design/architecture.md): how the system works inside

Security problems: see SECURITY.md[^3].

What you contribute to this repository is under the same MIT License[^4]
as the rest.

[^1]: [References](docs/project/references.md)
[^2]: [CHANGELOG.md](CHANGELOG.md), and [Releasing](docs/development/releasing.md)
[^3]: [SECURITY.md](SECURITY.md)
[^4]: [LICENSE](LICENSE)
