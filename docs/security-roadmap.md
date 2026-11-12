# Security roadmap

This page lists the weak spots found in FLOPOS's code and what fixing each
one would take. **Nothing here is started until Max picks it.** He sets the
order. The order below is only a suggestion: most protection for the least
work first.

These come from reading the code on 2026-11-10. None of them has been
tried as a real attack.

Sizes are rough. **Small** is a change in one place. **Medium** touches
several files or needs new tools. **Large** changes how the system works.

## Waiting for a decision

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

## Done

- **Walls between apps and the kernel (1.33.0).** Apps run in ring 3
  behind paging. An app can touch its own 8 MB only and asks the kernel
  for everything else; every pointer it hands over is checked. An app
  that breaks the rules is closed and FLOPOS carries on. Tested in the
  emulator with an app that breaks each rule on purpose
  (`tools/walltest`), and with every real app. Cost measured there: the
  browser playing a song uses 7.1% of the machine for sound instead of
  6.8%, and the screen stays at 30 frames a second. **Not yet run on the
  laptop.** What it does not do: the kernel itself is still one piece,
  so a fault in the kernel's own readers (network, files) is not
  contained; an app may still ask for anything the `Api` offers, such as
  the network or writing the three files it is allowed to write; and on
  a 486 there is no wall.

- **TCP's "hang up" (1.31.4).** A reset is believed only at the exact
  number expected. One that is merely close gets an answer saying where we
  are, and the real other end then hangs up properly (RFC 5961).
- **The stack and the forgotten router (1.31.4).** A send set off by
  arriving data never waits for the network; it is dropped and TCP sends
  it again. The router's place in the address table cannot be pushed out.
- **Claims to be the router (1.31.4).** An address is learned only as the
  answer to a question FLOPOS just asked, and what is known is never
  replaced. What remains, as on every network: someone on the same wire
  who answers the first question faster than the router is believed.
  HTTPS still catches the harm from that.
- **TCP's starting numbers (1.31.4).** The starting number and the port
  come from the random pool.
- **Name lookups (1.32.0).** The question's number and the port it is
  asked from are two separate random numbers, and an answer is taken only
  from the name server that was asked, while a question is open.
- **The tripwire (1.32.0).** A known number sits 108 KB down the stack
  (`kernel/time.c`) and is checked 250 times a second. If it changes,
  FLOPOS stops on the crash screen and says "The stack ran over its
  tripwire." Tried in a test build (`tripwire`).
- **The readers fed garbage (1.32.1).** `tools/fuzz` (see
  [Building](building.md)) gives damaged songs, recordings, certificate
  chains, secure conversations and web answers to FLOPOS's own code,
  built for the PC with a memory checker. Several hundred thousand
  inputs so far. It found three faults, all fixed:
  an XM song whose sizes wrapped round could make the tracker write far
  outside the song's memory; an IT song with more than 128 samples and
  no instruments wrote past the instrument table; and a page with an
  attribute name of twelve letters or more wrote one zero byte past a
  buffer in browzer. Not covered: the network's own packets (ARP, IP,
  TCP, DNS, DHCP), which were read through by hand in the review.
- **Four ways a crafted Ogg file could write past its buffers**
  (`apps/lib/audio/vorbis.c`), fixed before 1.19.0 shipped.

These were checked by reading and by the ordinary network tests. None was
tried as a real attack.

## Checked and fine

These were looked at in the same review and need no work. They don't need
looking at again unless their code changes.

- **Running out of memory:** an attack has nothing to use up. Every buffer
  is a fixed size, set up once, and never freed.
- **Network parsing:** every layer checks its lengths before reading.
  That covers the network card, ARP, DHCP, DNS, ping, TCP, TLS records,
  certificates, HTTP headers (capped at 2 KB) and redirects (at most 5).
- **HTTPS:** TLS 1.3, with post-quantum key exchange and full certificate
  checks; and since 1.25, TLS 1.2 in its AEAD-only form. What 1.2 adds
  was weighed before it was written:
  - **New code that reads what a server sends:** its ServerKeyExchange
    (curve, point, signature), checked against the message's length at
    every step. A P-256 point is checked to be on the curve, and an X25519
    result of all zeros is refused.
  - **Downgrades:** a 1.3 server's "DOWNGRD" mark is refused, so 1.2 is
    used only with servers that really have nothing newer.
  - **Left out on purpose:** RSA key exchange (no forward secrecy), CBC
    (padding oracles), SHA-1, renegotiation, compression and session
    resumption, the parts of 1.2 the known attacks went through.
  - **Weaker than 1.3 in one way:** without the extended master secret
    (an old server), the keys aren't bound to the whole handshake. That is
    what the triple-handshake attack needs, together with resumption or
    renegotiation, and FLOPOS does neither.
  - Tested against OpenSSL with RSA PKCS#1, RSA-PSS and ECDSA signatures,
    both ciphers, both curves, and without the extended master secret.
- **Nothing listens.** No outside machine can start a conversation; FLOPOS
  only answers ping.
- **The mp3 decoder and the MOD/XM player:** sizes read from the file are
  clamped.
- **TCP's out-of-order pieces (1.22):** a piece is kept only inside the
  space the window offered, and at most eight are tracked, so nothing a
  server sends can write past the receive buffer.
- **The table decoders (1.22):** a bit reader never moves past the end of
  its padded buffer, and a codebook that is not a prefix code, or has
  codes too long for two tables, is refused when it is built.
