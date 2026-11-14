# Roadmap

Ideas waiting their turn. None is started until it is picked, and each
gets a short plan with a rough cost first. What is already done is in
the CHANGELOG, and what FLOPOS can do today is listed in the README. What was fixed in the security
review is in Security[^1].

## Waiting to be picked

### A chat app

Something like Discord, IRC, WhatsApp or Signal. Which one, and whether
there is something better to talk on, is still open: IRC is the easiest for
this machine, if there were a channel worth being in.

### A mystical protocol for sharing data

A client and a server, one on the Toshiba and one on another machine, to do
what SFTP, FTP, stunnel, NFS and SMB do, without simply reinventing netcat.
Plain text at first, standard ciphers later. It should feel mysterious,
obscure and foreign, dark and enticing, like a forgotten metro-2 tunnel, a
liminal space, another dimension. That goes all the way down: how it checks
its data, how a session begins, every layer it touches. What it is for
(talking, sharing files, looking after a server) is not decided yet, so it
starts with an interview of its product owner.

### A wizard you can talk to

The wizard answers only words FLOPOS doesn't know, one line each. He
can't yet be talked to, or remember what was said.

## Security, waiting for a decision

These are the weak spots found in FLOPOS's code and what fixing each
one would take. **Nothing here is started until Max picks it.** He sets the
order. The order below is only a suggestion: most protection for the least
work first.

These come from reading the code on 2026-11-10. None of them has been
tried as a real attack.

Sizes are rough. **Small** is a change in one place. **Medium** touches
several files or needs new tools. **Large** changes how the system works.

### 9. The time from the network is not signed (medium)

**Set aside (2026-11-12), not dropped.** The laptop's clock battery
works, so the clock is right without the network, and it already refuses
to go back past the last `bye`. Signed time would cost a few seconds at
every network start and a fixed outside time server. Take it up again if
the clock battery fails.

`rtc.c`, `ip.c`. The clock sets itself from a time server, and plain NTP
has no signature. The answer is checked to come from the server asked and
to carry back a random number from the question, so nobody off the path
can fake it. Someone on your own network still could.

**Fix:** NTS, Network Time Security (RFC 8915), which is NTP with a TLS
handshake first. **Stops:** a faked time making a recently expired
certificate look valid. The clock already refuses to go back past the last
`bye`, which limits how far back that can reach.

[^1]: [Security](../design/security.md)
