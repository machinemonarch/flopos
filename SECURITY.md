# Security policy

## Reporting a problem

Write to **hello@machinemonarch.com**. Please say:

- what you found, and what it lets someone do
- which version (`version` at the prompt says, as does the top line of
  every program)
- how to make it happen again

Please give it a chance to be fixed before telling anyone else. FLOPOS is
an art project made by one person and an AI, with no bounty and no
promise of how quickly an answer comes, but every report is read.

## Which versions

Only the newest release is fixed. Older images are kept in `releases/` as
a record, not as something to run.

## What is protected, and what is not

- **The network.** Secure pages use TLS 1.3 (or TLS 1.2 in its modern
  form) with post-quantum key exchange, and every site must prove who it
  is. Nothing listens: no outside machine can start a conversation.
  For users: [How private it is](docs/user-guide/network.md#how-private-it-is).
  How it is built: [Security](docs/design/security.md).
- **Programs.** Each app runs behind a wall: it can touch only its own
  memory and must ask the system for everything else.
  [Making apps](docs/development/making-apps.md#the-wall).
- **Tapes.** maelcum can encrypt a backup with a passphrase.
  [maelcum](docs/user-guide/maelcum.md#encryption).
- **Not the floppy.** Your files are plain FAT12 files that any computer
  can read. That is on purpose: whoever holds the floppy holds your
  files.

Weak spots that are known and not yet fixed are listed in the
[roadmap](docs/project/roadmap.md#security-waiting-for-a-decision).
