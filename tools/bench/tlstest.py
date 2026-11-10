#!/usr/bin/env python3
"""Check FLOPOS's TLS against local OpenSSL servers, end to end.

    tools/bench/tlstest.py IMAGE WORKDIR [--slow]

Makes a test authority and certificates for 10.0.2.2 in WORKDIR, a copy
of IMAGE that trusts that authority, and starts these servers:
  8443  TLS 1.2, ChaCha20, X25519, RSA signed with PKCS#1
  8444  TLS 1.2, AES-128-GCM, P-256 only, ECDSA
  8445  TLS 1.2, AES-128-GCM, P-256, RSA-PSS
  8446  TLS 1.3
  8447  TLS 1.2 without the extended master secret
  8448  TLS 1.1 only: must be refused
Then run.py fetches each and prints ok or FAILED, one line each.
Needs openssl and mtools.
"""
import os, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
NOEMS = r'''
import socket, ssl, sys
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.minimum_version = ctx.maximum_version = ssl.TLSVersion.TLSv1_2
ctx.load_cert_chain(sys.argv[1], sys.argv[2]); ctx.set_ciphers("ECDHE-RSA-CHACHA20-POLY1305")
ctx.options |= 0x1                      # SSL_OP_NO_EXTENDED_MASTER_SECRET
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); s.bind(("127.0.0.1", 8447)); s.listen(4)
while True:
    c, _ = s.accept()
    try:
        t = ctx.wrap_socket(c, server_side=True); t.recv(4096)
        t.sendall(b"HTTP/1.0 200 OK\r\nContent-Type: text/html\r\n\r\n<title>no ems</title><p>ok</p>"); t.close()
    except Exception: pass
'''

def sh(*cmd): subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

def main():
    if len(sys.argv) < 3: sys.exit(__doc__)
    img, wd = os.path.abspath(sys.argv[1]), sys.argv[2]
    os.makedirs(wd, exist_ok=True); os.chdir(wd)
    sh("openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", "ca.key", "-out", "ca.pem", "-days", "30",
       "-subj", "/CN=FLOPOS test authority", "-addext", "basicConstraints=critical,CA:TRUE", "-addext", "keyUsage=critical,keyCertSign")
    open("ext.cnf", "w").write("subjectAltName=DNS:10.0.2.2\nbasicConstraints=CA:FALSE\nkeyUsage=digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\n")
    sh("openssl", "genrsa", "-out", "rsa.key", "2048")
    sh("openssl", "ecparam", "-name", "prime256v1", "-genkey", "-noout", "-out", "ec.key")
    for k in ("rsa", "ec"):
        sh("openssl", "req", "-new", "-key", k + ".key", "-out", k + ".csr", "-subj", "/CN=10.0.2.2")
        sh("openssl", "x509", "-req", "-in", k + ".csr", "-CA", "ca.pem", "-CAkey", "ca.key", "-CAcreateserial",
           "-out", k + ".pem", "-days", "30", "-extfile", "ext.cnf")
    open("bundle.pem", "w").write(open("/etc/ssl/certs/ca-certificates.crt").read() + open("ca.pem").read())
    sh(sys.executable, "-I", os.path.join(HERE, "..", "mkroots.py"), "bundle.pem", "roots.dat")
    open("test.img", "wb").write(open(img, "rb").read())
    sh("mcopy", "-o", "-i", "test.img", "roots.dat", "::ROOTS.DAT")
    open("noems.py", "w").write(NOEMS)
    srv = lambda port, *a: subprocess.Popen(["openssl", "s_server", "-accept", str(port), "-www"] + list(a),
                                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    procs = [srv(8443, "-tls1_2", "-cert", "rsa.pem", "-key", "rsa.key", "-cipher", "ECDHE-RSA-CHACHA20-POLY1305", "-groups", "X25519", "-sigalgs", "RSA+SHA256"),
             srv(8444, "-tls1_2", "-cert", "ec.pem", "-key", "ec.key", "-cipher", "ECDHE-ECDSA-AES128-GCM-SHA256", "-groups", "P-256"),
             srv(8445, "-tls1_2", "-cert", "rsa.pem", "-key", "rsa.key", "-cipher", "ECDHE-RSA-AES128-GCM-SHA256", "-groups", "P-256", "-sigalgs", "RSA-PSS+SHA256"),
             srv(8446, "-tls1_3", "-cert", "rsa.pem", "-key", "rsa.key"),
             srv(8448, "-tls1_1", "-cert", "rsa.pem", "-key", "rsa.key", "-cipher", "ALL:@SECLEVEL=0"),
             subprocess.Popen([sys.executable, "-I", "noems.py", "rsa.pem", "rsa.key"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)]
    time.sleep(1)
    try:
        steps = ["https://10.0.2.2:%d/=>%s" % (p, w) for p, w in
                 ((8443, r"TLS 1.2, X25519"), (8444, r"TLS 1.2, P-256"), (8445, r"TLS 1.2, P-256"),
                  (8446, r"ChaCha20-Poly1305, X25519|AES-128-GCM, X25519"), (8447, r"no extended master secret"),
                  (8448, r"TLS 1.1 or older"))]
        subprocess.run([sys.executable, os.path.join(HERE, "run.py"), "test.img", "out"] + sys.argv[3:] + steps)
    finally:
        for p in procs: p.terminate()

if __name__ == "__main__":
    main()
