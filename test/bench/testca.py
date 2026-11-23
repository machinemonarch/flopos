"""A certificate authority for the test bench only, so the test site can
speak https as a real one does.

    files()        build/bench-tls/: ca.pem, srv.pem, srv.key (made once)
    image(img, out)   a copy of a floppy image whose ROOTS.DAT trusts it too
                      (tlstest.py's own authority too, by image(img, out, ca))

The server's certificate is for 10.0.2.2.nip.io, the PC as the emulator
sees it, by a name.  The authority goes only on copies of an image made for
a test, never on the image itself: a release never trusts it.
"""
import os, subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DIR = os.path.join(ROOT, "build", "bench-tls")
NAME = "10.0.2.2.nip.io"


def files():
    ca, srv, key = (os.path.join(DIR, n) for n in ("ca.pem", "srv.pem", "srv.key"))
    if all(os.path.exists(p) for p in (ca, srv, key)):
        return ca, srv, key
    os.makedirs(DIR, exist_ok=True)
    cakey, csr, ext = (os.path.join(DIR, n) for n in ("ca.key", "srv.csr", "srv.ext"))
    q = dict(check=True, capture_output=True)
    subprocess.run(["openssl", "ecparam", "-name", "prime256v1", "-genkey", "-noout", "-out", cakey], **q)
    subprocess.run(["openssl", "req", "-x509", "-new", "-key", cakey, "-sha256", "-days", "3650",
                    "-subj", "/CN=FLOPOS test bench authority", "-out", ca], **q)
    subprocess.run(["openssl", "ecparam", "-name", "prime256v1", "-genkey", "-noout", "-out", key], **q)
    subprocess.run(["openssl", "req", "-new", "-key", key, "-subj", "/CN=" + NAME, "-out", csr], **q)
    open(ext, "w").write("subjectAltName=DNS:%s\nbasicConstraints=CA:FALSE\nkeyUsage=digitalSignature\n"
                         "extendedKeyUsage=serverAuth\n" % NAME)
    subprocess.run(["openssl", "x509", "-req", "-in", csr, "-CA", ca, "-CAkey", cakey, "-CAcreateserial",
                    "-days", "3650", "-sha256", "-extfile", ext, "-out", srv], **q)
    return ca, srv, key


def image(img, out, ca=None):
    """img copied to out, with a test authority (this one, unless another's
    PEM is named) added to its ROOTS.DAT."""
    import shutil, tempfile
    ca = os.path.abspath(ca) if ca else files()[0]
    tmp = tempfile.mkdtemp(prefix="flopos-testca.")
    try:
        one = os.path.join(tmp, "one.dat")
        subprocess.run(["python3", os.path.join(ROOT, "tools", "build", "mkroots.py"), ca, one], check=True, capture_output=True)
        have = open(os.path.join(ROOT, "data", "roots.dat"), "rb").read()
        add = open(one, "rb").read()
        n = int.from_bytes(have[8:10], "big") + int.from_bytes(add[8:10], "big")
        both = os.path.join(tmp, "ROOTS.DAT")
        open(both, "wb").write(have[:8] + n.to_bytes(2, "big") + have[10:] + add[10:])
        shutil.copy(img, out)
        subprocess.run(["mcopy", "-o", "-i", out, both, "::ROOTS.DAT"], check=True, capture_output=True)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return out
