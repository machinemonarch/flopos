# Security policy

## First, what FLOPOS is

FLOPOS is not meant to be an everyday operating system. It is an art
project: a test of what an AI can build, of what a 1996 Toshiba laptop can
still do, and of its author's creativity. It is a cyber-psychedelic
sandbox, a playground for ideas on how to bridge the old with the new.

Security is taken seriously in it, but it is not what the project is
about. FLOPOS comes with no guarantee that it is fit for any purpose, and
that goes for its security too. Don't trust it with anything that
matters. Having said that:

## Reporting a problem

Either write to **hello@machinemonarch.com** with a subject that starts
with **FLOPOS**, or open a pull request with the suggested fix.

Please say:

- What you found, and what it lets someone do.
- Which version (`version` at the prompt says, as does the top line of
  every program).
- How to make it happen again.

I intend to read every report but keep in mind that FLOPOS is made by one person
and an AI, with no bounty and no promise of how quickly an answer comes.

## Which versions

Only the newest release is fixed. Older images are kept in `releases/` as
a record, not as something to run.

## What is protected, and what is not

- **The network.** Secure pages use TLS 1.3 (or TLS 1.2 in its modern
  form) with post-quantum key exchange, and every site must prove who it
  is. How
  private it is for the user[^1], and how it is built[^2].
- **Programs.** Each app runs behind a wall: it can touch only its own
  memory and must ask the system for everything else[^3].
- **Tapes.** maelcum can encrypt and read back a backup with a passphrase[^4].
- **Not the floppy.** Your files are plain FAT12 files that any computer
  can read. That is on purpose: whoever holds the floppy holds your
  files.

Weak spots that are known and not yet fixed are listed in the roadmap[^5].

[^1]: [The network: how private it is](docs/user-guide/network.md#how-private-it-is)
[^2]: [Security](docs/design/security.md)
[^3]: [Making apps: the wall](docs/development/making-apps.md#the-wall)
[^4]: [maelcum: encryption](docs/user-guide/maelcum.md#encryption)
[^5]: [Roadmap: security, waiting for a decision](docs/project/roadmap.md#security-waiting-for-a-decision)
