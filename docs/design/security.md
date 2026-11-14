# Security

How FLOPOS protects what it does, and the record of the security review:
what was fixed, and what was looked at and found fine. Open items are in
the roadmap[^1]. To
report a problem, see SECURITY.md[^2].

## Encryption

All written from the published standards, and checked before it was
trusted against the test answers published with them: the worked
examples in the RFCs and FIPS documents themselves, and NIST's test
vectors for ML-KEM. `crypto` runs the checks again on the machine itself.

- **`sha256.c`**: SHA-256, HMAC, HKDF: the hashing every TLS 1.3 key is derived with
- **`chacha.c`**: ChaCha20-Poly1305 encryption
- **`aes.c`**: AES-128-GCM encryption
- **`x25519.c`**: X25519 key agreement
- **`sha3.c`**: SHA-3 and SHAKE, which ML-KEM is built on
- **`mlkem.c`**: ML-KEM-768, the post-quantum key agreement
- **`random.c`**: Random numbers: timing jitter between the timer and the processor's cycle counter, stirred into a SHA-256 pool
- **`sha512.c`**: SHA-512 and SHA-384, which many certificates are signed with
- **`bignum.c`**: Arithmetic modulo a big number (up to 4096 bits), Montgomery's way, for the signature checks
- **`rsa.c`**: RSA signature checks: PKCS#1 v1.5 (certificates) and PSS (the TLS 1.3 handshake)
- **`ecdsa.c`**: ECDSA signature checks on P-256 and P-384, in Jacobian coordinates
- **`x509.c`**: Reading certificates (DER) and checking a site's chain: names, dates, signatures, which authorities may vouch, up to `ROOTS.DAT`
- **`selftest.c`**: `crypto`: every one of the above checked on the machine itself

A TLS connection offers both the hybrid post-quantum key (X25519MLKEM768:
the ML-KEM secret and the X25519 secret, joined) and plain X25519, and
uses whichever the server picks.

The server must then prove who it is (`tls.c`, `x509.c`):

1. Its Certificate message is copied out and split into the chain.
2. `x509_check` reads each certificate and checks the site's own: its name
   against the host asked for (with `*.` standing for exactly one label),
   its dates against the clock (which sets itself from a time server when
   the network comes up, or from the time saved at the last `bye`), that it is for websites, and that it
   makes no critical demand this code doesn't understand.
3. It walks up the chain. At each step it tries the roots in `ROOTS.DAT`
   whose name matches the issuer; failing that, the certificates sent with
   it. Each authority must be a CA, allowed to sign certificates, in date,
   within its path-length limit and its name constraints, and its signature
   must check.
4. The CertificateVerify message must be the site's own key's signature
   over the handshake so far: ECDSA on P-256 or P-384, or RSA-PSS.
5. Finished is refused unless steps 2 to 4 happened.

The signature algorithms offered in the ClientHello are exactly the ones
this code can check. All of it was tested against 518 signatures made by
OpenSSL, the real chains of 14 well-known sites, and chains built to break
one rule each.

## Patched

- **Two more requests for apps (after 1.39.0).** `clock` (the date and
  time) and `glyph` (a letter's shape in the system's face), added for
  maelcum's tape sleeve. Both only read; each writes a fixed few bytes
  (six, five) into a buffer checked against the app's own memory.
  Neither gives out anything secret.

- **Sealing for apps (1.37.0).** Apps can ask the kernel to make a key
  from a passphrase (PBKDF2-HMAC-SHA256) and to seal or open data
  (ChaCha20-Poly1305, AES-256-GCM, AES-128-GCM); every buffer in such a
  request is checked against the app's own memory, and the rounds are
  capped. `maelcum` uses it to encrypt backups. Known limit, stated in
  its page: 10,000 rounds is light, so the passphrase must be long. A
  memory-hard step (scrypt, Argon2) would be stronger and is not built.

- **Walls between apps and the kernel (1.33.0).** Apps run in ring 3
  behind paging. An app can touch its own 8 MB only and asks the kernel
  for everything else; every pointer it hands over is checked. An app
  that breaks the rules is closed and FLOPOS carries on. Tested in the
  emulator with an app that breaks each rule on purpose
  (`tools/walltest`), and with every real app. Cost measured there: the
  browser playing a song uses 7.1% of the machine for sound instead of
  6.8%, and the screen stays at 30 frames a second. On the laptop the
  apps have run behind it since 1.33.0; the cost has not been measured
  there. What it does not do: the kernel itself is still one piece,
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
  Building[^3]) gives damaged songs, recordings, certificate
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

Looked at again on 2026-11-14: since the review, this code has changed
only in the fixes listed under Patched, in browzer's reader for the Radio
Browser directory (1.26, whose every string is held to the size of its
buffer), and in the wider characters of 1.42. All of the below still
holds.

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

[^1]: [roadmap](../project/roadmap.md#security-waiting-for-a-decision)
[^2]: [SECURITY.md](../../SECURITY.md)
[^3]: [Building](../development/building.md)
