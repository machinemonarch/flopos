#!/usr/bin/env python3
"""Check FLOPOS's TLS against local OpenSSL servers, end to end.

    test/bench/tlstest.py IMAGE WORKDIR [--slow]

Makes a test authority and certificates for 10.0.2.2 in WORKDIR, a copy
of IMAGE that trusts that authority, and starts these servers:
  18443  TLS 1.2, ChaCha20, X25519, RSA signed with PKCS#1
  18444  TLS 1.2, AES-128-GCM, P-256 only, ECDSA
  18445  TLS 1.2, AES-128-GCM, P-256, RSA-PSS
  18446  TLS 1.3
  18447  TLS 1.2 without the extended master secret
  18448  TLS 1.1 only: must be refused
  18449  TLS 1.3, P-384 only: it must ask us to say hello again
  18450  TLS 1.2, P-384 only, ECDSA on P-384
  18451  TLS 1.2, ECDHE-RSA with AES-128-CBC and SHA-1 only
  18452  TLS 1.2, ECDHE-RSA with AES-256-CBC and SHA-1 only
  18453  TLS 1.3 that moves its key on mid-answer, and asks us to as well
(Ports high enough that nothing else on the PC uses them: 8443 is often a
tunnel's.)  Then run.py fetches each and prints ok or FAILED, one line each.
Needs openssl and mtools.
"""
import os, socket, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
NOEMS = r'''
import socket, ssl, sys
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.minimum_version = ctx.maximum_version = ssl.TLSVersion.TLSv1_2
ctx.load_cert_chain(sys.argv[1], sys.argv[2]); ctx.set_ciphers("ECDHE-RSA-CHACHA20-POLY1305")
ctx.options |= 0x1                      # SSL_OP_NO_EXTENDED_MASTER_SECRET
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); s.bind(("127.0.0.1", 18447)); s.listen(4)
while True:
    c, _ = s.accept()
    try:
        t = ctx.wrap_socket(c, server_side=True); t.recv(4096)
        t.sendall(b"HTTP/1.0 200 OK\r\nContent-Type: text/html\r\n\r\n<title>no ems</title><p>ok</p>"); t.close()
    except Exception: pass
'''

# openssl s_server takes a line "K" on its input as: send a KeyUpdate,
# asking for one back; "q" closes.  Each piece is fed after a pause, once
# the handshake is done, so each is read alone.
KEYUPD = r'''
import subprocess, sys, time
while True:
    p = subprocess.Popen(["openssl", "s_server", "-accept", "18453", "-tls1_3", "-naccept", "1",
                          "-cert", sys.argv[1], "-key", sys.argv[2]],
                         stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    for line in p.stdout:
        if b"CIPHER is" in line: break
    for part in (b"HTTP/1.0 200 OK\r\nContent-Type: text/html\r\n\r\n", b"K\n",
                 b"<title>key update</title><p>ok</p>\n", b"q\n"):
        time.sleep(0.5); p.stdin.write(part); p.stdin.flush()
    time.sleep(0.5); p.kill(); p.wait()
'''

def sh(*cmd): subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

def main():
    if len(sys.argv) < 3: sys.exit(__doc__)
    img, wd = os.path.abspath(sys.argv[1]), sys.argv[2]
    os.makedirs(wd, exist_ok=True); os.chdir(wd)
    for port in range(18443, 18454):                 # another program on one would answer in its place
        with socket.socket() as c:
            if not c.connect_ex(("127.0.0.1", port)): sys.exit("FAILED  port %d is in use by another program" % port)
    sh("openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", "ca.key", "-out", "ca.pem", "-days", "30",
       "-subj", "/CN=FLOPOS test authority", "-addext", "basicConstraints=critical,CA:TRUE", "-addext", "keyUsage=critical,keyCertSign")
    open("ext.cnf", "w").write("subjectAltName=DNS:10.0.2.2\nbasicConstraints=CA:FALSE\nkeyUsage=digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\n")
    sh("openssl", "genrsa", "-out", "rsa.key", "2048")
    sh("openssl", "ecparam", "-name", "prime256v1", "-genkey", "-noout", "-out", "ec.key")
    sh("openssl", "ecparam", "-name", "secp384r1", "-genkey", "-noout", "-out", "ec384.key")
    for k in ("rsa", "ec", "ec384"):
        sh("openssl", "req", "-new", "-key", k + ".key", "-out", k + ".csr", "-subj", "/CN=10.0.2.2")
        sh("openssl", "x509", "-req", "-in", k + ".csr", "-CA", "ca.pem", "-CAkey", "ca.key", "-CAcreateserial",
           "-out", k + ".pem", "-days", "30", "-extfile", "ext.cnf")
    sys.path.insert(0, HERE); import testca
    testca.image(img, "test.img", ca="ca.pem")     # the floppy's own roots, and this authority
    open("noems.py", "w").write(NOEMS)
    open("keyupd.py", "w").write(KEYUPD)
    srv = lambda port, *a: subprocess.Popen(["openssl", "s_server", "-accept", str(port), "-www"] + list(a),
                                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    procs = [srv(18443, "-tls1_2", "-cert", "rsa.pem", "-key", "rsa.key", "-cipher", "ECDHE-RSA-CHACHA20-POLY1305", "-groups", "X25519", "-sigalgs", "RSA+SHA256"),
             srv(18444, "-tls1_2", "-cert", "ec.pem", "-key", "ec.key", "-cipher", "ECDHE-ECDSA-AES128-GCM-SHA256", "-groups", "P-256"),
             srv(18445, "-tls1_2", "-cert", "rsa.pem", "-key", "rsa.key", "-cipher", "ECDHE-RSA-AES128-GCM-SHA256", "-groups", "P-256", "-sigalgs", "RSA-PSS+SHA256"),
             srv(18446, "-tls1_3", "-cert", "rsa.pem", "-key", "rsa.key"),
             srv(18448, "-tls1_1", "-cert", "rsa.pem", "-key", "rsa.key", "-cipher", "ALL:@SECLEVEL=0"),
             srv(18449, "-tls1_3", "-cert", "ec384.pem", "-key", "ec384.key", "-groups", "P-384"),
             srv(18450, "-tls1_2", "-cert", "ec384.pem", "-key", "ec384.key", "-cipher", "ECDHE-ECDSA-AES128-GCM-SHA256", "-groups", "P-384"),
             srv(18451, "-tls1_2", "-cert", "rsa.pem", "-key", "rsa.key", "-cipher", "ECDHE-RSA-AES128-SHA"),
             srv(18452, "-tls1_2", "-cert", "rsa.pem", "-key", "rsa.key", "-cipher", "ECDHE-RSA-AES256-SHA"),
             subprocess.Popen([sys.executable, "-I", "noems.py", "rsa.pem", "rsa.key"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL),
             subprocess.Popen([sys.executable, "-I", "keyupd.py", "rsa.pem", "rsa.key"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)]
    time.sleep(1)
    try:
        steps = ["https://10.0.2.2:%d/=>%s" % (p, w) for p, w in
                 ((18443, r"TLS 1.2, X25519"), (18444, r"TLS 1.2, P-256"), (18445, r"TLS 1.2, P-256"),
                  (18446, r"ChaCha20-Poly1305, X25519|AES-128-GCM, X25519"), (18447, r"no extended master secret"),
                  (18448, r"TLS 1.1 or older"), (18449, r"it asked for P-384(.|\n)*, P-384, "),
                  (18450, r"TLS 1.2, P-384"), (18451, r"AES-128-CBC with SHA-1(.|\n)*\[web\] 200"),
                  (18452, r"AES-256-CBC with SHA-1(.|\n)*\[web\] 200"),
                  (18453, r"moved its key on, and ours(.|\n)*: key update"))]
        subprocess.run([sys.executable, os.path.join(HERE, "run.py"), "test.img", "out"] + sys.argv[3:] + steps)
    finally:
        for p in procs: p.terminate()

if __name__ == "__main__":
    main()
