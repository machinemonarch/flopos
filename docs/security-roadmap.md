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

### 1. TCP accepts a "hang up" too easily (small)

`kernel/tcp.c`, `tcp_input`. A reset is believed if its sequence number is
anywhere ahead of the one expected. So someone who guesses the connection's
port (a 1 in 16,384 chance) can cut a download or a radio stream.

**Fix:** believe a reset only at the exact expected number. For other
numbers in range, reply with a check instead, as RFC 5961 describes.
**Stops:** cut connections.

### 2. The stack can run out when the router is forgotten (small)

`kernel/ip.c`. The address table has 8 slots and learns from every ARP
message on the wire, so a busy network can push the router's entry out.
Sending then waits for the network while still inside the send. While it
waits, incoming data can trigger another send, which waits again, nesting
deeper until the stack runs out.

**Fix:** a send caused by incoming data never waits. It drops the packet
instead, and TCP sends it again later. Also keep the router's slot from
being pushed out. **Stops:** a crash that a hostile local network could
probably force.

### 3. Anyone on the network can claim to be the router (small)

`kernel/ip.c`, `arp_learn`. Every ARP message overwrites what is known.

**Fix:** learn only from answers to questions we asked, and never let a
known router entry be replaced. **Stops:** some local-network spoofing.
HTTPS already catches the harm from it; plain HTTP does not.

### 4. Name lookups are easy to fake (small)

`kernel/ip.c`, `ip_lookup` and `take_dns`. The query's ID number and port
both come from a single 16-bit value taken from the clock. Nobody checks
who the answer came from.

**Fix:** take both from the random pool (`rand_bytes`), and accept answers
only from the DNS server. **Stops:** fake addresses for plain-HTTP sites.

### 5. TCP's starting numbers are guessable (small)

`kernel/tcp.c`, `tcp_connect`. The starting sequence number and the port
come from the cycle counter.

**Fix:** take them from `rand_bytes`. **Stops:** blind injection into
plain-HTTP connections. It also makes number 1 harder to pull off.

### 6. A tripwire at the bottom of the stack (small)

**Fix:** write a known value at the lowest point of the stack and check it
every frame. If it has changed, stop with a clear message. This prevents
nothing, but it turns silent memory damage into an honest crash.

### 7. Feed the parsers garbage on the host (medium)

**Fix:** build the code that reads outside data on the Linux machine and
feed it millions of random and mangled inputs. This is called fuzzing. It
covers certificates, TLS, HTTP, HTML, mp3, MOD/XM, Ogg and FLAC. It finds
overflows like the four in the Ogg code before anyone else does. **Cost:**
setting it up, plus fixing whatever it finds.

### 8. Walls between apps and the kernel (large)

Today everything shares all of memory, with full control of the machine.
One overflow anywhere owns the whole machine and could even rewrite the
floppy. Until this is fixed, the floppy's write-protect tab is the only
guard.

**Fix:** use paging and run apps with lower privilege (ring 3). An app
could then touch only its own 8 MB, and would have to ask the kernel for
everything else. **Stops:** a single bug taking over everything. This is
the biggest gain and also the most work, and every call an app makes would
cost a little speed.

## In progress

- **Four ways a crafted Ogg file can write past its buffers**, in
  `apps/lib/audio/vorbis.c`. These are bugs in unreleased code, being fixed
  as part of 1.19.0.

## Done

Nothing yet.

## Checked and fine

These were looked at in the same review and need no work. They don't need
looking at again unless their code changes.

- **Running out of memory:** an attack has nothing to use up. Every buffer
  is a fixed size, set up once, and never freed.
- **Network parsing:** every layer checks its lengths before reading.
  That covers the network card, ARP, DHCP, DNS, ping, TCP, TLS records,
  certificates, HTTP headers (capped at 2 KB) and redirects (at most 5).
- **HTTPS:** TLS 1.3 only, with post-quantum key exchange and full
  certificate checks.
- **Nothing listens.** No outside machine can start a conversation; FLOPOS
  only answers ping.
- **The mp3 decoder and the MOD/XM player:** sizes read from the file are
  clamped.
