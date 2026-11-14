# Contributing

FLOPOS is an art project. Max directs it and Claude builds it, so what
goes in is Max's call. Ideas and bug reports are welcome; a report from a
real 430CDT (or any machine of its age) is worth the most.

## Before changing anything

Read [the rules it is built by](docs/project/philosophy.md#the-rules-it-is-built-by).
The ones that matter most for code:

- Everything runs on the laptop's own hardware: no helper machines.
- No JavaScript.
- Nothing copied from Toshiba or anyone else's code; everything written
  from published standards, and sources credited in
  [References](docs/project/references.md).
- Plain words on the screen, and a sentence saying why when something
  fails.

## Where to start

| Page | For |
|---|---|
| [Building](docs/development/building.md) | Building the image and running it in the emulator |
| [Testing](docs/development/testing.md) | The tests, and the tools in `tools/` that run them |
| [Making apps](docs/development/making-apps.md) | Programs that live on the floppy beside the system |
| [Releasing](docs/development/releasing.md) | Version numbers, the changelog, and making a release |
| [Architecture](docs/design/architecture.md) | How the system works inside |

Every change that someone using FLOPOS would notice gets a line under
`## Unreleased` in [CHANGELOG.md](CHANGELOG.md).

Security problems: see [SECURITY.md](SECURITY.md), not a public report.

What you contribute is under the same [MIT License](LICENSE) as the rest.
