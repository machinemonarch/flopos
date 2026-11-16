# Roadmap

Ideas waiting their turn. None is started until Max picks it, and each
gets a short plan with a rough cost first. What is done is in the
CHANGELOG; what was fixed in the security review is in Security[^1].

## Waiting to be picked

- **A chat app:** Like Discord, IRC, WhatsApp or Signal; which is open. IRC is easiest for this machine, given a channel worth being in.
- **A mystical protocol for sharing data:** A client on the Toshiba and a server elsewhere, doing what SFTP, NFS or SMB do without reinventing netcat. It should feel obscure and enticing, like a forgotten metro tunnel, all the way down to how it checks data and starts a session. What it is for is undecided, so it starts with an interview of its product owner.
- **A wizard you can talk to:** One who remembers what was said.

## Security, waiting for a decision

Weak spots found by reading the code on 2026-11-10, none tried as a real
attack. Max sets the order.

### 9. The time from the network is not signed (medium)

**Set aside on 2026-11-12, not dropped.** The clock battery works and the
clock already refuses to go back past the last `bye`; take this up again
if the battery fails.

Plain NTP (`rtc.c`, `ip.c`) has no signature. Answers must come from the
server asked and carry back a random number, so nobody off the path can
fake one, but someone on your own network could, to make an expired
certificate look valid. **Fix:** NTS (RFC 8915), NTP with a TLS handshake
first, at a few seconds per network start.

[^1]: [Security](../design/security.md)
