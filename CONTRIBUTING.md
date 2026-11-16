# Contributing

FLOPOS is an art project and it is Max's: he directs it and Claude builds
it, so what goes into this repository is Max's call. Ideas and bug
reports are welcome; a report from a real 430CDT (or any machine of its
age) is worth the most. It is under the MIT License, so you are free to
fork it.

## The rules it is built by

- **The constraints are the point.** One floppy, one 1996 laptop. No outside machine does FLOPOS's computing for it.
- **JavaScript only where allowed, and walled off.** None runs until a site is allowed, other sites' scripts never run (except libraries from four well-known hosts), and the rest runs in a room that can reach nothing but the page.
- **Ask before anything risky.** Features that open the machine to the outside world are discussed first.
- **The nice things are the default.** You shouldn't need a command to get the good version.
- **Never freeze.** A feature may arrive rough; the machine may not hang.
- **Say what is true.** Plain words, a reason when something fails, and FLOPOS says so when something is not secure.
- **Everything it keeps is readable anywhere.** A normal DOS disk, plain-text journal, Markdown documents. The exception is what you choose to encrypt.
- **Clean provenance.** Nothing copied from Toshiba or anyone else. Written from published standards, credited in the references[^1].
- **Every release is numbered, kept and described** in CHANGELOG.md[^2].

## Where to start

- [Building](docs/development/building.md): Building and running in the emulator, and where everything lives.
- [Testing](docs/development/testing.md): The tests, all in `test/`.
- [Making apps](docs/development/making-apps.md): Programs that live on the floppy.
- [Releasing](docs/development/releasing.md): Versions, the changelog, a release.
- [Architecture](docs/design/architecture.md): Which file does what.

Security problems: SECURITY.md[^3]. Contributions are under the same MIT
License[^4].

[^1]: [References](docs/project/references.md)
[^2]: [CHANGELOG.md](CHANGELOG.md)
[^3]: [SECURITY.md](SECURITY.md)
[^4]: [LICENSE](LICENSE)
