#!/usr/bin/env python3
"""Make data/roots.dat: the authorities FLOPOS trusts, small enough for a floppy.

    tools/mkroots.py [bundle.pem] [data/roots.dat]

The bundle is a PEM file of root certificates -- by default the one Debian
and Ubuntu install as /etc/ssl/certs/ca-certificates.crt, which is
Mozilla's list of roots trusted for websites.  Of each root only what
checking a chain needs is kept: its subject name (to match against the
issuer of the certificate below it) and its public key, both exactly as
the DER in the certificate has them.

    "FLOPROOT"  then a 16-bit count
    per root:   16-bit length, subject Name DER
                16-bit length, SubjectPublicKeyInfo DER

Roots whose keys FLOPOS cannot use (anything but RSA, P-256 and P-384) are
left out, and said so.  The file is committed, so building does not need
this tool; run it again to bring the list up to date.
"""
import base64
import sys

RSA = bytes.fromhex("2a864886f70d010101")
EC = bytes.fromhex("2a8648ce3d0201")
P256 = bytes.fromhex("2a8648ce3d030107")
P384 = bytes.fromhex("2b81040022")


def tlv(buf, at):
    """(tag, value start, value end, element end) of the element at `at`."""
    tag = buf[at]
    ln = buf[at + 1]
    p = at + 2
    if ln & 0x80:
        k = ln & 0x7F
        ln = int.from_bytes(buf[p:p + k], "big")
        p += k
    return tag, p, p + ln, p + ln


def children(buf, start, end):
    out = []
    while start < end:
        t, vs, ve, ee = tlv(buf, start)
        out.append((t, start, vs, ve, ee))
        start = ee
    return out


def oid_ok(spki):
    _, vs, ve, _ = tlv(spki, 0)
    alg = children(spki, vs, ve)[0]
    parts = children(spki, alg[2], alg[3])
    oid = spki[parts[0][2]:parts[0][3]]
    if oid == RSA:
        return "RSA"
    if oid == EC and len(parts) > 1:
        curve = spki[parts[1][2]:parts[1][3]]
        return {P256: "P-256", P384: "P-384"}.get(curve)
    return None


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else "/etc/ssl/certs/ca-certificates.crt"
    out = sys.argv[2] if len(sys.argv) > 2 else "data/roots.dat"
    text = open(src).read()
    ders = []
    for chunk in text.split("-----BEGIN CERTIFICATE-----")[1:]:
        b64 = chunk.split("-----END CERTIFICATE-----")[0]
        ders.append(base64.b64decode("".join(b64.split())))

    entries, kinds, skipped = [], {}, 0
    for der in ders:
        _, vs, ve, _ = tlv(der, 0)                    # Certificate
        tbs = children(der, vs, ve)[0]
        f = children(der, tbs[2], tbs[3])
        if f[0][0] == 0xA0:                            # explicit version
            f = f[1:]
        # serial, signature, issuer, validity, subject, spki
        subject = der[f[4][1]:f[4][4]]
        spki = der[f[5][1]:f[5][4]]
        kind = oid_ok(spki)
        if not kind:
            skipped += 1
            continue
        kinds[kind] = kinds.get(kind, 0) + 1
        entries.append((subject, spki))

    blob = bytearray(b"FLOPROOT") + len(entries).to_bytes(2, "big")
    for subject, spki in entries:
        blob += len(subject).to_bytes(2, "big") + subject
        blob += len(spki).to_bytes(2, "big") + spki
    open(out, "wb").write(blob)
    print("wrote %s: %d roots (%s), %d bytes; %d left out (keys FLOPOS cannot use)"
          % (out, len(entries), ", ".join("%d %s" % (v, k) for k, v in sorted(kinds.items())),
             len(blob), skipped))


if __name__ == "__main__":
    main()
