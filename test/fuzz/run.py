#!/usr/bin/env python3
"""Feed FLOPOS's readers garbage on this PC (see fuzz.c).

    test/fuzz/run.py [TARGET ...] [--runs N] [--seed S]

Builds each target into build/fuzz with the compiler's memory checker,
makes or finds its inputs, and gives it N damaged ones (20000 unless
told).  Prints ok or FAILED for each; an input that broke something is
kept in build/fuzz/finds, and `build/fuzz/TARGET --one FILE MODE` (the mode
is in the file's name) shows it again.

Inputs: songs from ~/.cache/flopos-songs (test/audiotest/fetch.py),
recordings from ~/.cache/flopos-fuzz/audio (the first 96 KB of any mp3,
ogg or flac will do; they are never kept here).  The rest are made.
"""
import argparse, os, random, struct, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(ROOT, "build", "fuzz")
FINDS = os.path.join(OUT, "finds")
HOME = os.path.expanduser("~/.cache")
CC = ["gcc", "-O1", "-g", "-fno-omit-frame-pointer", "-fsanitize=address,undefined",
      # Wrapping arithmetic is everywhere in decoders and is not what this hunts.
      "-fno-sanitize=signed-integer-overflow,shift,float-cast-overflow",
      "-fno-sanitize-recover=undefined", "-w"]
LIB = ["src/apps/lib/audio", "src/apps/lib/dsp", "src/apps/lib/bits"]

def sh(*cmd, cwd=None):
    subprocess.run(cmd, check=True, cwd=cwd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

def files(*names): return [os.path.join(ROOT, n) for n in names]
def glob(d, ext=".c"): return sorted(os.path.join(ROOT, d, f) for f in os.listdir(os.path.join(ROOT, d)) if f.endswith(ext))

# ---- inputs that are made here ---------------------------------------------
def seeds_x509(d):
    """An authority, one below it, and sites' certificates of each kind,
    as chains; and the authority as the roots file."""
    if os.path.exists(os.path.join(d, "roots.dat")): return
    os.makedirs(d, exist_ok=True)
    ext = os.path.join(d, "ext.cnf")
    open(ext, "w").write(
        "[ca]\nbasicConstraints=critical,CA:TRUE\nkeyUsage=critical,keyCertSign,cRLSign\n"
        "[mid]\nbasicConstraints=critical,CA:TRUE,pathlen:0\nkeyUsage=critical,keyCertSign\n"
        "nameConstraints=permitted;DNS:.test\n"
        "[leaf]\nbasicConstraints=CA:FALSE\nsubjectAltName=DNS:fuzz.test,DNS:*.fuzz.test,IP:10.0.2.2\n"
        "extendedKeyUsage=serverAuth\nkeyUsage=critical,digitalSignature\n")
    o = lambda *a: sh("openssl", *a, cwd=d)
    o("req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", "ca.key", "-out", "ca.pem", "-days", "3650",
      "-subj", "/O=FLOPOS fuzz/CN=root", "-addext", "keyUsage=critical,keyCertSign")
    o("ecparam", "-name", "prime256v1", "-genkey", "-noout", "-out", "mid.key")
    o("req", "-new", "-key", "mid.key", "-out", "mid.csr", "-subj", "/O=FLOPOS fuzz/CN=mid")
    o("x509", "-req", "-in", "mid.csr", "-CA", "ca.pem", "-CAkey", "ca.key", "-CAcreateserial", "-out", "mid.pem",
      "-days", "3650", "-extfile", "ext.cnf", "-extensions", "mid", "-sha384")
    keys = {"rsa": ["genrsa", "-out", "rsa.key", "2048"],
            "p256": ["ecparam", "-name", "prime256v1", "-genkey", "-noout", "-out", "p256.key"],
            "p384": ["ecparam", "-name", "secp384r1", "-genkey", "-noout", "-out", "p384.key"]}
    der = lambda pem: subprocess.run(["openssl", "x509", "-in", pem, "-outform", "DER"], cwd=d, check=True,
                                     capture_output=True).stdout
    rnd = random.Random(1)
    n = 0
    for k, make in keys.items():
        o(*make)
        o("req", "-new", "-key", k + ".key", "-out", k + ".csr", "-subj", "/CN=fuzz.test")
        for signer, extra in (("mid", []), ("ca", []), ("ca", ["-sigopt", "rsa_padding_mode:pss", "-sigopt", "rsa_pss_saltlen:32", "-sha256"])):
            o("x509", "-req", "-in", k + ".csr", "-CA", signer + ".pem", "-CAkey", signer + ".key", "-CAcreateserial",
              "-out", "leaf.pem", "-days", "365", "-extfile", "ext.cnf", "-extensions", "leaf", *extra)
            chain = [der("leaf.pem")] + ([der("mid.pem")] if signer == "mid" else [])
            if n % 3 == 0: chain.append(der("ca.pem"))          # some sites send the root too
            chain.append(bytes(rnd.getrandbits(8) for _ in range((256, 72, 104)[n % 3])))   # a signature
            open(os.path.join(d, "chain%d.bin" % n), "wb").write(b"".join(struct.pack(">H", len(c)) + c for c in chain))
            n += 1
    sh(sys.executable, "-I", os.path.join(ROOT, "tools", "build", "mkroots.py"), "ca.pem", "roots.dat", cwd=d)
    for f in os.listdir(d):
        if not (f.startswith("chain") or f == "roots.dat"): os.remove(os.path.join(d, f))
    os.makedirs(os.path.join(d, "in"), exist_ok=True)
    for f in os.listdir(d):
        if f.startswith("chain"): os.rename(os.path.join(d, f), os.path.join(d, "in", f))

def seeds_browzer(d):
    """What web servers say: pages with everything browzer reads in them,
    redirects, cookies, a radio stream, a song, and JSON."""
    if os.path.isdir(d) and os.listdir(d): return
    os.makedirs(d, exist_ok=True)
    def resp(name, body, ctype="text/html; charset=utf-8", status="200 OK", extra=""):
        if isinstance(body, str): body = body.encode("utf-8")
        head = "HTTP/1.1 %s\r\nServer: fuzz\r\nContent-Type: %s\r\n%sContent-Length: %d\r\nConnection: close\r\n\r\n" % (
            status, ctype, extra, len(body))
        open(os.path.join(d, name), "wb").write(head.encode() + body)
    page = """<!DOCTYPE html><html lang="sv"><head><meta charset="utf-8"><title>A page &amp; its title</title>
<meta http-equiv="refresh" content="30; url=/again"><style>p { color: red }</style>
<link rel="preconnect" href="http://fuzz.test/radio-browser" /><script>var x = "<p>not a tag</p>";</script></head>
<body><h1>Heading one</h1><h2>Räksmörgås &aring;&auml;&ouml; &#229; &#xE5; &quot;quoted&quot; &lt;tag&gt; &nbsp; &copy; &unknown; &#99999999;</h2>
<p>A paragraph with a <a href="/next?a=1&amp;b=2">link</a>, <b>bold</b>, <i>italic</i>, <u>underlined</u>,
<kbd>Ctrl</kbd>+<kbd>S</kbd> and <code>http://fuzz.test/listen/radio.mp3</code> and a word that is far too long to fit:
aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.</p>
<ul><li>one<ul><li>nested <a href="http://other.test:8080/x/../y/./z?q#frag">far link</a></li></ul></li><li>two</li></ul>
<ol><li>first</li><li>second</li></ol><hr><pre>  kept   as
   written</pre><blockquote>quoted</blockquote>
<table><tr><th>a</th><th>b</th></tr><tr><td>1</td><td><a href="relative/page.html">2</a></td></tr></table>
<img src="/art.jpg" alt="a picture"><img src="x.png"><svg><path d="M0 0"/></svg><template><p>hidden</p></template>
<form action="/search" method="post"><input type="text" name="q" value="hello" size="20" maxlength="40">
<input type="password" name="p"><input type="hidden" name="h" value="1"><input type="checkbox" name="c" checked>
<input type="radio" name="r" value="a" checked><input type="radio" name="r" value="b">
<select name="s"><option value="1">One</option><option value="2" selected>Two</option><option>Three</option></select>
<textarea name="t" rows="3" cols="30">some
text</textarea><button type="submit" name="go" value="1">Go</button><input type="submit" value="Send"></form>
<form action="get.cgi"><input name="only"></form>
<iframe src="/frame.html"></iframe><frame src="/f2.html"><noscript><meta http-equiv="refresh" content="0; url=/plain"></noscript>
<div data-stations="[{&quot;id&quot;:1,&quot;name&quot;:&quot;Live&quot;,&quot;stream&quot;:&quot;/sc/stream/1/&quot;}]"></div>
<div data-show="{&quot;url&quot;:&quot;http:\\/\\/fuzz.test\\/Flashback\\/170.mp3&quot;}"></div>
<a href="/m/215671.xm">a song</a> <a href="stream.pls">a list</a> <a href="mailto:x@y.z">mail</a> <a>no address</a>
<p>שלום 世界 😀 end</p></body></html>"""
    resp("page.bin", page)
    resp("small.bin", "<title>t</title><p>Just <a href='/a'>one</a> link.</p>")
    # HLS: every request answered alike, so a playlist's segments are the
    # playlist again, or the transport packets below
    resp("hls-master.bin", "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=64000\nlo.m3u8\n#EXT-X-STREAM-INF:BANDWIDTH=999999999999\nhi.m3u8\n",
         ctype="application/vnd.apple.mpegurl")
    resp("hls-live.bin", "#EXTM3U\n#EXT-X-TARGETDURATION:2\n#EXT-X-MEDIA-SEQUENCE:4294967290\n" +
         "".join("#EXTINF:2.0,\nseg%d.ts\n" % i for i in range(8)), ctype="application/vnd.apple.mpegurl")
    ts = b"".join(bytes([0x47, 0x41 if i == 0 else 0x01, 0x01, 0x10 | (i & 15)]) +
                  ((b"\x00\x00\x01\xc0\x00\x00\x80\x80\x05\x21\x00\x01\x00\x01" if i == 0 else b"") +
                   b"\xff\xfb\x90\x44" + bytes(range(256)) * 2)[:184] for i in range(12))
    resp("hls-ts.bin", ts, ctype="video/mp2t")
    # Several answers in one (browzer.c's NEXT): a playlist, then what it names.
    def answers(name, *parts):
        out = []
        for body, ctype in parts:
            if isinstance(body, str): body = body.encode()
            out.append(("HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %d\r\n\r\n" % (ctype, len(body))).encode() + body)
        open(os.path.join(d, name), "wb").write(b"\0\0NEXT\0\0".join(out))
    def xor_seal(b, key, iv):               # what browzer.c's pretend crypt_cbc undoes
        b = b + bytes([16 - len(b) % 16]) * (16 - len(b) % 16)
        return bytes(x ^ key[i & 15] ^ iv[i & 15] for i, x in enumerate(b))
    pl, key = "application/vnd.apple.mpegurl", bytes(range(16))
    answers("hls-aes.bin", ("#EXTM3U\n#EXT-X-TARGETDURATION:2\n#EXT-X-MEDIA-SEQUENCE:7\n"
                            "#EXT-X-KEY:METHOD=AES-128,URI=\"k\"\n#EXTINF:2,\na.ts\n#EXT-X-ENDLIST\n", pl),
            (key, "application/octet-stream"), (xor_seal(ts, key, (7).to_bytes(16, "big")), "video/mp2t"))
    mp4 = os.path.expanduser("~/.cache/flopos-fuzz/audio/hls-aac.mp4")   # made by test/bench/site.py
    if os.path.exists(mp4):
        b, i, init, piece = open(mp4, "rb").read(), 0, b"", b""
        while i + 8 <= len(b):
            n = struct.unpack(">I", b[i:i + 4])[0] or len(b) - i
            if b[i + 4:i + 8] in (b"ftyp", b"moov"): init += b[i:i + n]
            elif b[i + 4:i + 8] in (b"moof", b"mdat") and len(piece) < 20000: piece += b[i:i + n]
            i += n
        answers("hls-mp4.bin", ("#EXTM3U\n#EXT-X-TARGETDURATION:2\n#EXT-X-MAP:URI=\"i.mp4\"\n#EXTINF:2,\na.m4s\n"
                                "#EXTINF:2,\nb.m4s\n#EXT-X-ENDLIST\n", pl), (init, "video/mp4"), (piece, "video/iso.segment"))
        iv = bytes(range(16, 32))
        answers("hls-mp4aes.bin", ("#EXTM3U\n#EXT-X-TARGETDURATION:2\n#EXT-X-KEY:METHOD=AES-128,URI=\"k\",IV=0x101112131415161718191A1B1C1D1E1F\n"
                                   "#EXT-X-MAP:URI=\"i.mp4\",BYTERANGE=\"%d@0\"\n#EXTINF:2,\na.m4s\n#EXT-X-ENDLIST\n" % len(init), pl),
                (key, "application/octet-stream"), (xor_seal(init, key, iv), "video/mp4"), (xor_seal(piece, key, iv), "video/iso.segment"))
    resp("text.bin", "plain text\nwith lines\n\tand tabs\n" * 20, ctype="text/plain")
    resp("moved.bin", "<a href='/new'>moved</a>", status="302 Found",
         extra="Location: /new/place?x=1\r\nSet-Cookie: sid=abc123; Path=/; HttpOnly\r\nSet-Cookie: big=%s; Max-Age=60\r\n" % ("v" * 300))
    resp("cookie.bin", page[:900], extra="Set-Cookie: a=1; Domain=fuzz.test; Path=/; Expires=Wed, 01 Jan 2031 00:00:00 GMT\r\n"
         "Set-Cookie: b=2\r\nSet-Cookie: gone=; Max-Age=0\r\n")
    resp("json.bin", '{"station":{"name":"Chiptune","listen_url":"http:\\/\\/fuzz.test\\/listen\\/radio.mp3","mounts":'
         '[{"url":"http://fuzz.test/a.mp3","bitrate":128},{"url":"http://fuzz.test/b.aac"}]},"now_playing":{"song":{"title":"x"}}}',
         ctype="application/json")
    resp("rb.bin", '[{"name":"Radio 103FM","url_resolved":"http://fuzz.test/103.mp3","codec":"MP3","bitrate":128,"votes":5},'
         '{"name":"\u05d2\u05dc\u05d2\u05dc\u05e6 Glglz","url":"http://fuzz.test/g.aac","codec":"AAC+","bitrate":64}]', ctype="application/json")
    resp("pls.bin", "[playlist]\nNumberOfEntries=2\nFile1=http://fuzz.test/one.mp3\nTitle1=One\nFile2=http://fuzz.test/two.ogg\n", ctype="audio/x-scpls")
    resp("m3u.bin", "#EXTM3U\n#EXTINF:-1,Station\nhttp://fuzz.test/stream\n", ctype="audio/x-mpegurl")
    # A radio stream and downloads, from whatever recordings and songs there are.
    audio = os.path.join(HOME, "flopos-fuzz", "audio")
    n = 0
    for f in sorted(os.listdir(audio)) if os.path.isdir(audio) else []:
        b = open(os.path.join(audio, f), "rb").read()[:40000]
        ct = {"mp3": "audio/mpeg", "ogg": "application/ogg", "flac": "audio/flac", "aac": "audio/aacp", "wav": "audio/wav"}.get(f.rsplit(".", 1)[-1])
        if not ct or n >= 20: continue
        if f.endswith("mp3") and n % 2 == 0:         # as a station sends it, with titles let in
            out = bytearray()
            for at in range(0, len(b), 8192):
                t = b"StreamTitle='Artist - Song %d';" % at
                t += b"\0" * (-len(t) % 16)
                out += b[at:at + 8192] + bytes([len(t) // 16]) + t
            head = "ICY 200 OK\r\nicy-name: Fuzz FM\r\nicy-metaint: 8192\r\nicy-br: 128\r\nContent-Type: audio/mpeg\r\n\r\n"
            open(os.path.join(d, "icy%d.bin" % n), "wb").write(head.encode() + bytes(out))
        else:
            resp("rec%d.bin" % n, b, ctype=ct)
        n += 1
    songs = os.path.join(HOME, "flopos-songs")
    for f in sorted(os.listdir(songs))[:16] if os.path.isdir(songs) else []:
        b = open(os.path.join(songs, f), "rb").read()
        if len(b) < 60000:
            resp("song-" + f + ".bin", b, ctype="application/octet-stream",
                 extra='Content-Disposition: attachment; filename="%s"\r\nSet-Cookie: dl=1\r\n' % f)

    # Styles and scripts: the tree, the style sheets, and (when the fuzzer
    # allows the site) the door, all get pieces of this.
    css = """<html><head><title>styled</title><style>
@media (max-width: 600px) { .a { display: none } } @media screen and (min-width: 700px), print { .b { color: #f00 } }
@supports (display: grid) { .g { display: grid } } @font-face { font-family: x } @keyframes k { from { opacity: 0 } }
div > p + p ~ span.x.y#z[data-k="v"]:not(.q):nth-child(2n+1):first-child { font-weight: bold !important; margin: 0 0 0 40px }
ul li:hover a, .menu:focus-within, a::before, :is(.p, .q) > b { display: block; content: "x" }
.pop { position: fixed } .sr { position: absolute; width: 1px; clip: rect(0 0 0 0) } [hidden] { display: none }
h1 { text-align: center; text-transform: uppercase; color: rgb(10, 200, 30) } nav ul { list-style: none; padding-left: 2em }
/* a comment */ .x { color: hsl(120, 50%, 50%); color: tomato } .y { }} .broken { color: </style></head>
<body><h1 class="a b">Title</h1><div class="pop">Pop</div><nav><ul><li><a href="/">Home</a></li></ul></nav>
<p class="sr">sr</p><span class="x y" id="z" data-k="v" style="color: red; font-style: italic">styled</span>
<p hidden>gone</p><div onclick="alert(1)">click</div><button type="button" id="b">B</button>
<script>document.getElementById('b').addEventListener('click', function () { this.textContent = 'pressed'; });
var l = document.querySelectorAll('div > p, .x'); for (var i = 0; i < l.length; i++) l[i].classList.toggle('on');
fetch('/api').then(function (r) { return r.json(); }).then(function (j) { document.body.insertAdjacentHTML('beforeend', '<p>' + j.a + '</p>'); });
setTimeout(function () { document.title = 'later'; }, 10); document.write('<p>written</p>');</script>
<script src="/app.js"></script><script src="http://elsewhere.test/x.js"></script></body></html>"""
    resp("styled.http", css)

    # Tables: laid out in columns (dom.c, web.c), with everything awkward.
    rows = "".join("<tr><td>%d</td><td><a href='/r%d'>row %d</a></td><td>%s</td></tr>" % (i, i, i, "word " * (i * 7 % 40)) for i in range(60))
    resp("tables.http", """<html><head><title>tables</title><style>.h{display:none}</style></head><body>
<table><caption>c</caption><thead><tr><th>a<th>b<th colspan=999>c</tr></thead><tbody>%s</tbody></table>
<table><tr><td><table><tr><td>in<td>side</table><td>outer</table>
<table><tr><td colspan=0>x<td colspan=-3>y<td colspan=12>z<td>w</table>
<table><tr>%s</tr></table>
<table><tr><td><form><input name=a size=80><select name=s><option>1<option>2</select><textarea name=t>x</textarea><input type=checkbox name=c></form><td><h1>head</h1><p>para<ul><li>a<li>b</ul><hr><pre>pre
text</pre></table>
<table><tr><td></td><td></td></tr><tr></tr><tr><td class=h>hidden<td align=right>right<td align=center>mid</table>
<table><tr><td>%s<td>long</table>
<table><tbody><tr><td>1<td>2</tbody><tfoot><tr><td>f<td>g</tfoot></table>
</body></html>""" % (rows, "<td>c</td>" * 40, "x" * 2900))

    # A radio page: a player in an attribute, <audio>, and streams named in
    # JSON, with escapes in the names.
    resp("player.http", """<html><head><title>radio</title></head><body>
<div class="play" url="/m/1" data-stream="https://s.test/a.mp3"><div>Listen Live</div></div>
<a href="/x"><span data-audio="/in/a/link.mp3">inside a link</span></a>
<audio controls src="/a.ogg"></audio><video><source src="/v.mp4"><source src="/b.aac"></video>
<span data-src="/c.m3u8?x=1#y">c</span><span data-src="/not-sound.png">d</span>
<script type="application/json">{"ch":[{"name":"Main \\u00e5 \\"q\\" \\u","stream_url":"https:\\/\\/a.test\\/l\\/main\\/radio.mp3"},
{"title":"\\u0420\\u0430\\u0434\\u0438\\u043e","listen_url":"http://b.test:8000/stream"},{"name":"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx","mp3":"/m.mp3"},
{"name":"left open</script><p>&larr; back &rarr; &laquo;x&raquo; &times;</p></body></html>""")

def seeds_inflate(d):
    """The floppy's programs as the build packs them, in pieces of up to 48 KB."""
    import zlib
    os.makedirs(d, exist_ok=True)
    for p in glob("build/apps", ".app") + [os.path.join(ROOT, "build", "room.bin"), os.path.join(ROOT, "build", "kernel.bin")]:
        if not os.path.isfile(p): continue
        data = open(p, "rb").read()
        for i, at in enumerate(range(0, min(len(data), 4 << 16), 48 << 10)):
            open(os.path.join(d, "%s-%d" % (os.path.basename(p), i)), "wb").write(zlib.compress(data[at:at + (48 << 10)], 9))

def seeds_nts(d):
    """A key exchange's answer (the records a real server sends: protocol,
    cipher, eight cookies, a server and a port, the end) and a time
    server's signed answer (an identifier, then the tag and the new cookies
    it seals), whole and in variations."""
    if os.path.isdir(d) and os.listdir(d): return
    os.makedirs(d, exist_ok=True)
    rec = lambda t, body: struct.pack(">HH", t, len(body)) + body
    rnd = random.Random(8915)
    for k in range(6):
        cookies = [bytes(rnd.getrandbits(8) for _ in range((100, 104, 132, 64, 256, 300)[k])) for _ in range(8)]
        ke = rec(0x8001, b"\x00\x00") + rec(0x8004, b"\x00\x0f") + b"".join(rec(5, c) for c in cookies)
        if k & 1: ke += rec(6, b"ntp.fuzz.test") + rec(7, b"\x00\x7b")
        if k == 4: ke += rec(0x0003, b"\x00\x01")        # a warning, not critical
        ke += rec(0x8000, b"")
        hdr = bytes([0x24, 1, 0, 0xE9]) + bytes(20) + bytes(8) + bytes(8) + struct.pack(">II", 3900000000, 0)
        uid = struct.pack(">HH", 0x0104, 36) + bytes(32)
        inner = b"".join(struct.pack(">HH", 0x0204, 4 + len(c)) + c for c in cookies[:(k % 3) + 1])
        nonce = bytes(range(16))
        auth_body = struct.pack(">HH", 16, 16 + len(inner)) + nonce + bytes(16) + inner
        auth = struct.pack(">HH", 0x0404, 4 + len(auth_body)) + auth_body
        tail = struct.pack(">HH", 0x0204, 8) + b"late" if k == 5 else b""     # after the tag: not believed
        open(os.path.join(d, "nts%d.bin" % k), "wb").write(struct.pack(">H", len(ke)) + ke + hdr + uid + auth + tail)

def seeds_tls(d):
    """Real conversations: an authority and two sites' certificates are
    made, openssl serves them on this PC in each form FLOPOS takes, and
    the target itself talks to each and keeps what the server said."""
    ind = os.path.join(d, "in")
    if os.path.isdir(ind) and os.listdir(ind): return
    os.makedirs(ind, exist_ok=True)
    open(os.path.join(d, "ext.cnf"), "w").write(
        "[leaf]\nbasicConstraints=CA:FALSE\nsubjectAltName=DNS:fuzz.test\nextendedKeyUsage=serverAuth\n")
    o = lambda *a: sh("openssl", *a, cwd=d)
    o("req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", "ca.key", "-out", "ca.pem", "-days", "3650",
      "-subj", "/O=FLOPOS fuzz/CN=tls root")
    o("genrsa", "-out", "rsa.key", "2048")
    o("ecparam", "-name", "prime256v1", "-genkey", "-noout", "-out", "ec.key")
    for k in ("rsa", "ec"):
        o("req", "-new", "-key", k + ".key", "-out", k + ".csr", "-subj", "/CN=fuzz.test")
        o("x509", "-req", "-in", k + ".csr", "-CA", "ca.pem", "-CAkey", "ca.key", "-CAcreateserial", "-out", k + ".pem",
          "-days", "365", "-extfile", "ext.cnf", "-extensions", "leaf")
    sh(sys.executable, "-I", os.path.join(ROOT, "tools", "build", "mkroots.py"), "ca.pem", "roots.dat", cwd=d)
    forms = [("-tls1_2", "rsa", "-cipher", "ECDHE-RSA-CHACHA20-POLY1305", "-groups", "X25519", "-sigalgs", "RSA+SHA256"),
             ("-tls1_2", "ec", "-cipher", "ECDHE-ECDSA-AES128-GCM-SHA256", "-groups", "P-256"),
             ("-tls1_2", "rsa", "-cipher", "ECDHE-RSA-AES128-GCM-SHA256", "-groups", "P-256", "-sigalgs", "RSA-PSS+SHA256"),
             ("-tls1_3", "rsa"), ("-tls1_3", "ec"),
             ("-tls1_3", "rsa", "-ciphersuites", "TLS_CHACHA20_POLY1305_SHA256"),
             ("-tls1_3", "ec", "-groups", "P-256:X25519")]            # asks FLOPOS to try again with another group
    import time
    env = dict(os.environ, FUZZ_ROOTS=os.path.join(d, "roots.dat"), ASAN_OPTIONS="detect_leaks=0")
    for n, (ver, k, *more) in enumerate(forms):
        port = 8460 + n
        srv = subprocess.Popen(["openssl", "s_server", "-accept", str(port), "-www", ver, "-cert", k + ".pem", "-key", k + ".key"]
                               + more, cwd=d, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(0.5)
        try:
            for pq in ((), ("pq",)):
                out = os.path.join(ind, "talk%d%s.bin" % (n, "pq" if pq else ""))
                r = subprocess.run([os.path.join(OUT, "tls"), "--record", str(port), out, *pq], env=env,
                                   capture_output=True, text=True)
                if r.returncode: print("   (tls: form %d %s not recorded: %s)" % (n, " ".join(pq), r.stdout.strip()[:80]))
                if r.returncode or os.path.getsize(out) < 200: os.remove(out)
        finally:
            srv.terminate(); srv.wait()

TARGETS = {
    # name: (sources, include dirs, where the inputs are, a function that makes them, environment)
    "tracker": (files("test/fuzz/tracker.c", "src/apps/lib/audio/tracker.c"), ["src/apps/sdk"] + LIB,
                os.path.join(HOME, "flopos-songs"), None, {}),
    "audio":   (files("test/fuzz/audio.c") + [f for f in glob("src/apps/lib/audio") if not f.endswith("tracker.c")]
                + glob("src/apps/lib/dsp") + glob("src/apps/lib/bits"), ["src/apps/sdk"] + LIB,
                os.path.join(HOME, "flopos-fuzz", "audio"), None, {}),
    "x509":    (files("test/fuzz/x509.c", "test/fuzz/kshim.c", "src/kernel/x509.c", "src/kernel/rsa.c", "src/kernel/ecdsa.c", "src/kernel/bignum.c",
                      "src/kernel/sha256.c", "src/kernel/sha512.c", "src/kernel/sha1.c", "src/kernel/fmt.c"), ["src/kernel"],
                os.path.join(OUT, "made", "x509", "in"), lambda: seeds_x509(os.path.join(OUT, "made", "x509")),
                {"FUZZ_ROOTS": os.path.join(OUT, "made", "x509", "roots.dat")}),
    "tls":     (files("test/fuzz/tls.c", "test/fuzz/kshim.c", "src/kernel/tls.c", "src/kernel/x509.c", "src/kernel/rsa.c", "src/kernel/ecdsa.c",
                      "src/kernel/bignum.c", "src/kernel/sha256.c", "src/kernel/sha512.c", "src/kernel/sha3.c", "src/kernel/x25519.c",
                      "src/kernel/mlkem.c", "src/kernel/fmt.c"), ["src/kernel"],
                os.path.join(OUT, "made", "tls", "in"), lambda: seeds_tls(os.path.join(OUT, "made", "tls")),
                {"FUZZ_ROOTS": os.path.join(OUT, "made", "tls", "roots.dat")},
                # The seals' checks are renamed, and tls.c (the target) puts its own in front of them.
                {"src/kernel/chacha.c": "-Daead_open=real_aead_open", "src/kernel/aes.c": "-Daesgcm_open=real_aesgcm_open"}),
    "inflate": (files("test/fuzz/inflate.c", "src/kernel/inflate.c"), ["src/kernel"],
                os.path.join(OUT, "made", "inflate"), lambda: seeds_inflate(os.path.join(OUT, "made", "inflate")), {}),
    "nts":     (files("test/fuzz/nts.c", "test/fuzz/kshim.c", "src/kernel/nts.c", "src/kernel/fmt.c"), ["src/kernel"],
                os.path.join(OUT, "made", "nts"), lambda: seeds_nts(os.path.join(OUT, "made", "nts")), {},
                # the signature's check is renamed, and nts.c (the target) puts its own in front of it
                {"src/kernel/aes.c": "-Daessiv_open=real_aessiv_open"}),
    "browzer": (files("test/fuzz/browzer.c", "src/kernel/fmt.c") + glob("src/apps/browzer") + glob("src/apps/lib/audio")
                + glob("src/apps/lib/dsp") + glob("src/apps/lib/bits"), ["src/apps/sdk", "src/apps/browzer", "src/kernel"] + LIB,
                os.path.join(OUT, "made", "browzer"), lambda: seeds_browzer(os.path.join(OUT, "made", "browzer")), {}),
}

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("targets", nargs="*")
    ap.add_argument("--runs", type=int, default=20000)
    ap.add_argument("--seed", type=int, default=1)
    a = ap.parse_args()
    os.makedirs(FINDS, exist_ok=True)
    bad = 0
    for name in a.targets or TARGETS:
        if name not in TARGETS: print("FAILED  %s: no such target" % name); bad = 1; continue
        src, inc, seeds, make, env, *renamed = TARGETS[name]
        exe = os.path.join(OUT, name if a.seed == 1 else "%s-s%d" % (name, a.seed))   # each seed its own: they run side by side
        src = [s for s in src if os.path.exists(s)]
        incs = sum((["-I", os.path.join(ROOT, i)] for i in inc), [])
        for f, define in (renamed[0] if renamed else {}).items():      # built apart, with a name changed
            obj = os.path.join(OUT, "%s-s%d-%s.o" % (name, a.seed, os.path.basename(f)))
            subprocess.run(CC + incs + [define, "-c", os.path.join(ROOT, f), "-o", obj], check=True)
            src.append(obj)
        r = subprocess.run(CC + incs + ["-o", exe,
                           os.path.join(ROOT, "test/fuzz/fuzz.c")] + src + ["-lm"], capture_output=True, text=True)
        if r.returncode: print("FAILED  %s: does not build\n%s" % (name, r.stderr[-600:])); bad = 1; continue
        if make: make()
        if not os.path.isdir(seeds) or not os.listdir(seeds):
            print("FAILED  %s: no inputs in %s" % (name, seeds)); bad = 1; continue
        r = subprocess.run([exe, seeds, str(a.runs), str(a.seed), FINDS], capture_output=True, text=True,
                           env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0", **env))
        if r.returncode == 0 and r.stdout.startswith("ok"):
            print(r.stdout.strip())
        else:
            lines = [l for l in r.stderr.splitlines() if "ERROR" in l or "runtime error" in l or l.startswith("kept")
                     or l.lstrip().startswith(("#0", "#1", "#2", "#3"))]
            print("FAILED  %s\n   %s" % (name, "\n   ".join(l.strip()[:160] for l in lines[:8])))
            bad = 1
    sys.exit(bad)

if __name__ == "__main__":
    main()
