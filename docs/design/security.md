# Security

How FLOPOS protects what it does, and the record of the security review.
Open items are in the roadmap[^1]. To report a problem, see
SECURITY.md[^2].

## Encryption

Written from the published standards and checked against their test
answers (the RFCs' and FIPS documents' examples, NIST's ML-KEM vectors).
`crypto` runs the checks again on the machine.

- **`sha256.c`:** SHA-256, HMAC, HKDF.
- **`nts.c`:** Signed network time (NTS).
- **`sha512.c`, `sha3.c`, `sha1.c`:** SHA-512 and SHA-384; SHA-3 and SHAKE; SHA-1 (HMAC on TLS 1.2 CBC records only).
- **`chacha.c`, `aes.c`:** ChaCha20-Poly1305; AES-GCM, AES-CBC and AES-SIV-CMAC.
- **`x25519.c`, `mlkem.c`:** X25519 and ML-KEM-768.
- **`bignum.c`, `rsa.c`, `ecdsa.c`:** RSA (PKCS#1 v1.5 and PSS), ECDSA and ECDH on P-256 and P-384.
- **`x509.c`:** Certificates and chains, up to `ROOTS.DAT`.
- **`random.c`:** Timing jitter between the timer and the cycle counter, stirred into a SHA-256 pool.

TLS offers hybrid X25519MLKEM768 and plain X25519, and uses what the
server picks. The server must then prove who it is:

1. Its certificate must name the host (`*.` is exactly one label), be in date and be for websites, with no critical extension FLOPOS doesn't understand.
2. Each authority up the chain must be a CA allowed to sign, in date, within its path length and name constraints, with a signature that checks, ending at a root in `ROOTS.DAT`.
3. CertificateVerify must be signed by the site's own key.
4. Finished is refused unless all of that happened.

Tested against 518 OpenSSL signatures, the real chains of 14 sites, and
chains built to break one rule each.

## TLS 1.2

Weighed before it was written, and kept to what Chrome offers:

- **New code reading the server:** ServerKeyExchange is length-checked at every step; a P-256 point must be on the curve; an all-zero X25519 result is refused.
- **HelloRetryRequest:** suite, group and cookie (at most 256 bytes) checked, once only, and the answer must use the asked-for curve.
- **Downgrades:** a TLS 1.3 server's DOWNGRD mark is refused.
- **CBC (Lucky 13):** bad padding and a bad MAC get the same answer (closing), and the work done is the same whatever the padding.
- **Left out:** RSA key exchange, CBC with ECDSA, 3DES, RC4, renegotiation, compression and resumption.
- **Without the extended master secret,** keys aren't bound to the whole handshake; the triple-handshake attack also needs resumption or renegotiation, which FLOPOS lacks.

## Patched

- **Signed network time (2026-11-18):** the clock is set only from NTS (RFC 8915, `nts.c`): a TLS 1.3 key exchange with the certificate checked, then NTP answers signed with AES-SIV-CMAC (`aes.c`, checked against RFC 5297's examples), each with a fresh identifier and a one-time cookie. Plain NTP is only a fallback, by default only from the time server the local network names, and never earlier than the last `bye` (`clock` sets it). Tried against Netnod's and PTB's public servers (Cloudflare's and time.nl's hang up before a 120 MHz machine has checked their certificates); `test/fuzz` has an `nts` target.
- **App requests `clock` and `glyph`:** read only, writing a fixed few bytes into a checked buffer.
- **Sealing for apps (1.37.0):** PBKDF2 keys and ChaCha20/AES-GCM sealing, buffers checked, rounds capped. 10,000 rounds is light, so passphrases must be long.
- **Walls between apps and the kernel (1.33.0):** apps run in ring 3 behind paging, every pointer checked, a rule-breaker closed (`test/walltest`). Not covered: faults in the kernel's own readers, and a 486 (no wall).
- **TCP resets (1.31.4):** believed only at the exact sequence number (RFC 5961).
- **ARP (1.31.4):** an address is learned only as an answer to a question just asked, and the router's entry can't be pushed out.
- **TCP starting numbers and ports (1.31.4):** from the random pool.
- **DNS (1.32.0):** random question number and port, answers only from the server asked.
- **Stack tripwire (1.32.0):** a known value 108 KB down the stack, checked 250 times a second.
- **Fuzzing (1.32.1):** `test/fuzz` found three memory faults (two tracker songs, one long attribute name in browzer), all fixed. Network packets below TLS were reviewed by hand instead.
- **Ogg (before 1.19.0):** four ways a crafted file could write past its buffers.

None of these was tried as a real attack.

## Checked and fine

Rechecked 2026-11-14; still true unless their code changes.

- **Scripts:** run only on allowed sites, in a 4 MB room with no network, files or cookies. Every request goes through one door browzer checks (`src/apps/browzer/script.c`); the kernel stops a room that faults or overruns. Other sites' scripts are never fetched, except libraries from four public hosts over https. A script's fetch() reaches another site only as that site allows (CORS: Origin sent, no cookies sent or kept, a preflight when not a plain request), and never a local-network address from a public page. The fuzzer plays a hostile room.
- **Memory exhaustion:** every buffer is fixed in size, allocated once, never freed.
- **Network parsing:** every layer checks lengths before reading; HTTP headers are capped at 2 KB and redirects at 5.
- **Decoders:** sizes from files are clamped; bit readers stop at the padded end; a codebook that is not a prefix code is refused.
- **TCP's out-of-order pieces:** kept only inside the offered window, at most eight.

[^1]: [roadmap](../project/roadmap.md#security-waiting-for-a-decision)
[^2]: [SECURITY.md](../../SECURITY.md)
