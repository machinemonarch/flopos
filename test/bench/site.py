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
  /hls/aes.m3u8      lo.m3u8 sealed with AES-128 (an IV given, then the
                     pieces' numbers as IVs); /hls/badkey.m3u8 the wrong key
  /hls/fmp4.m3u8     AAC in MP4 pieces (EXT-X-MAP), made by GStreamer;
  /hls/fmp4aes.m3u8  the same sealed, its header too
  /hls/break.m3u8    mp3 pieces, a break (EXT-X-DISCONTINUITY) to an advert in
                     AAC at 32 kHz, and a break back
                     (the sound is the first ~/.cache/flopos-fuzz/audio/seed*.mp3)
  /codecs/NAME       a file test/audiotest/codecs.py made (build/audiotest/codecs/),
                     whole; /codecs/live/NAME the same as a stream, with no length
"""
import glob, http.server, json, os, struct, subprocess, sys, tempfile, time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8150
SONGS = sys.argv[2] if len(sys.argv) > 2 else os.path.expanduser("~/.cache/flopos-songs")
HOST = "http://10.0.2.2:%d" % PORT

# A Worker that does what a speed test does (librespeed's worker, in short):
# a download read through onprogress and cut short, an upload of a Blob
# made from an ArrayBuffer, the results posted back.
WORKER = """var got = 0;
this.addEventListener('message', function (e) {       // as librespeed's worker listens
  if (e.data !== 'start') return;
  var x = new XMLHttpRequest(), seen = 0;
  x.onprogress = function (ev) { seen = ev.loaded; if (seen > 1000000) x.abort(); };
  x.onabort = function () {
    var buf = new ArrayBuffer(65536), f = new Float32Array(buf), parts = [], u = new XMLHttpRequest(), up = 0;
    for (var i = 0; i < f.length; i++) f[i] = Math.random();
    for (i = 0; i < 20; i++) parts.push(buf);
    u.upload.onprogress = function (ev) { up = ev.loaded; };
    u.onload = function () { postMessage({ down: seen > 1000000, up: up, said: u.responseText, typed: f instanceof Float32Array }); };
    u.open('POST', '/api/upload'); u.send(new Blob(parts));
  };
  x.responseType = 'arraybuffer';
  x.open('GET', '/api/stream?r=' + Math.random()); x.send();
});
"""

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
document.getElementById('f').insertAdjacentHTML('beforeend', '<p>form points elsewhere</p>');</script>""",
    # Vite's check for a modern browser, as its legacy plugin writes it into
    # every page it builds (radio.garden's among them); a browser that
    # fails it is given the old build, or told it is too old.
    "/js/vite": """<title>js vite</title><p id="o">the check did not run</p>
<script type="module">import.meta.url;import("_").catch(()=>1);(async function*(){})().next();if(location.protocol!="file:"){window.__vite_is_modern_browser=true}</script>
<script type="module">document.getElementById('o').textContent = window.__vite_is_modern_browser ? 'vite: modern' : 'vite: too old';</script>""",
    # test/bench/webprobe.py: looks for each web feature's interface and members
    "/js/probe": """<title>js probe</title><p id="o">probing</p><script>
var el = document.createElement('div'), sample = { Window: window, Document: document, Element: el, HTMLElement: el, Node: el,
  EventTarget: el, HTMLDivElement: el, HTMLInputElement: document.createElement('input'), HTMLFormElement: document.createElement('form'),
  HTMLAnchorElement: document.createElement('a'), HTMLImageElement: document.createElement('img'), Location: location, Navigator: navigator,
  Storage: window.localStorage, CSSStyleDeclaration: el.style, DOMTokenList: el.classList, History: window.history, Screen: window.screen };
var G = typeof globalThis !== 'undefined' ? globalThis : (function () { return this; })();    /* globalThis itself is probed below */
function has(k) {
  var p = k.split('.'), base, g;
  if (k === 'javascript.builtins.globalThis') return typeof globalThis !== 'undefined';
  try {
    if (p[0] === 'javascript') { g = G[p[2]]; if (p.length === 3) return g !== undefined;
      if (p[3] === p[2]) return typeof g === 'function';
      if (p[3] === '@@iterator') return g !== undefined && g.prototype !== undefined && (typeof Symbol !== 'undefined' && Symbol.iterator in g.prototype || '@@iterator' in g.prototype);
      return g !== undefined && (p[3] in Object(g) || (g.prototype !== undefined && p[3] in g.prototype)); }
    g = G[p[1]];
    if (p.length === 2) return g !== undefined || sample[p[1]] !== undefined;
    if (p[2] === p[1]) return typeof g === 'function';                       /* its constructor */
    base = sample[p[1]] || (g && g.prototype) || g;
    if (p[2] === '@@iterator') return base != null && (typeof Symbol !== 'undefined' && Symbol.iterator in Object(base) || '@@iterator' in Object(base));
    return base !== undefined && base !== null && (p[2] in Object(base) || (g !== undefined && p[2] in Object(g)));
  } catch (e) { return false; }
}
function part(n) {
  return fetch('/api/probe-list?part=' + n).then(function (r) { return r.json(); }).then(function (keys) {
    if (!keys.length) { document.getElementById('o').textContent = 'probe: done'; return; }
    var out = ''; for (var i = 0; i < keys.length; i++) out += has(keys[i]) ? '1' : '0';
    return fetch('/api/probe?part=' + n, { method: 'POST', body: out }).then(function () {
      setTimeout(function () { part(n + 1).catch(function (e) { document.getElementById('o').textContent = 'probe: failed ' + e; }); }, 10);
    });   /* each part its own turn: one script may only run so long */
  });
}
part(0).catch(function (e) { document.getElementById('o').textContent = 'probe: failed ' + e; });
</script>""",
    "/js/url": """<title>js url</title><p id="o">not yet</p><script>
var r = [], u = new URL('../b/./c?x=1#h', 'https://Ex.COM:443/a/d/e');
r.push(u.href);
u.searchParams.append('y', 'a b'); r.push(u.search);
u.pathname = '/z'; u.hash = ''; r.push(u.href);
r.push(URL.canParse('nope') + '/' + URL.canParse('mailto:a@b'));
try { new URL('http://'); r.push('no throw'); } catch (e) { r.push(e.name); }
var q = new URLSearchParams([['b', '2'], ['a', '1']]); q.sort(); r.push(q + ' ' + q.size + ' ' + Array.from(q.keys()).join(''));
var h = new Headers({ 'X-A': '1' }); h.append('x-a', '2'); r.push(h.get('X-A') + ' ' + [...h].length);
var x = new XMLHttpRequest(), ev = [];
x.addEventListener('load', function (e) { ev.push('load' + (e.loaded > 0 ? '+' : '')); });
x.addEventListener('loadend', function () { ev.push('end'); document.getElementById('o').textContent = 'url: ' + r.join(' | ') + ' | ' + ev.join(',') + ' ' + x.status + ' ' + (x.getResponseHeader('content-type') || ''); });
x.open('GET', '/api/cors-ok'); x.send();
</script>""",
    "/js/clickworker": """<title>js clickworker</title><p id="o">not yet</p><button id="b">go</button><script>
document.getElementById('b').addEventListener('click', function () {      // a Worker started by a click, as librespeed's
  var w = new Worker('/js/echo.js');
  w.onmessage = function (e) { document.getElementById('o').textContent = 'clickworker ' + e.data; };
  w.postMessage(21);
});
</script>""",
    "/js/speed": """<title>js speed</title><p id="o">not yet</p><script>
var w = new Worker('/js/worker.js'), o = document.getElementById('o');
w.onmessage = function (e) {
  var d = e.data;
  fetch('/api/stream').then(function (r) { return r.arrayBuffer(); }, function () { return null; }).then(function (b) {
    o.textContent = 'speed: down ' + d.down + ' up ' + d.up + ' said ' + d.said + ' typed ' + d.typed + ' fetched ' + (b ? b.byteLength : 'no');
  }, function (x) { o.textContent = 'speed: fetch said ' + x; });
};
w.onerror = function (e) { o.textContent = 'speed: worker error ' + e.message; };
w.postMessage('start');
</script>""",
    "/js/bigfetch": """<title>js bigfetch</title><p id="o">not yet</p><script>
fetch('/api/big').then(function (r) { return r.json().then(function (j) {
  document.getElementById('o').textContent = 'bigfetch ' + j.items.length + ' ' + j.items[2999].slice(0, 10) + ' ' + r.headers.get('x-test');
}); }, function (e) { document.getElementById('o').textContent = 'bigfetch failed ' + e; });
</script>""",
    "/https/form": """<title>https form</title><form action="http://10.0.2.2:8150/collect"><input name="q" value="hi"><input type="submit" value="Send"></form>
<form action="https://10.0.2.2.nip.io:8151/collect"><input name="r" value="ok"><input type="submit" value="Send safely"></form>""",
    "/collect": """<title>collected</title><p>COLLECTED</p>""",
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
    "/js/modules": """<title>js modules</title><p id="o">not yet</p><p id="d">not loaded</p>
<script type="importmap">{"imports": {"tools": "/js/mod/tools.js", "lib/": "/js/mod/"}}</script>
<script nomodule>document.getElementById('o').textContent = 'NOMODULE RAN';</script>
<p id="cs">no script</p><script>document.getElementById('cs').textContent = 'current ' + document.currentScript.tagName.toLowerCase();</script>
<script type="module" src="/js/mod/main.js"></script>
<script type="module">
import { twice } from 'tools';
import { name } from 'lib/name.js';
document.getElementById('d').textContent = 'inline ' + twice(name);
import('./mod/late.js').then(m => { document.getElementById('d').textContent += ', ' + m.default(); });
</script>""",
    "/js/mod/main.js": """import { twice, counter, bump } from './tools.js';
import * as N from './name.js';
bump();
document.getElementById('o').textContent = 'modules: ' + twice(N.name) + ' ' + counter + ' ' + typeof import.meta.url;""",
    "/js/mod/tools.js": """export let counter = 0;
export function bump() { counter++; }
export const twice = s => s + s;""",
    "/js/mod/name.js": """export const name = 'ab';""",
    "/js/mod/late.js": """export default function () { return 'late'; }""",
    "/js/big": """<title>js big</title><p id="b">not yet</p><p id="a">not after</p>
<script src="/js/big.js"></script><script src="/js/huge.js"></script>
<script>document.getElementById('a').textContent = 'after the huge one: ' + (typeof huge);</script>""",
    "/js/big.js": "var big = 0;\n" + "big += 1; // a line long enough to make the file big, in pieces\n" * 2600 + "document.getElementById('b').textContent = 'big ' + big;",
    "/js/huge.js": "var huge = 1; // HUGE RAN\n" * 12000,
    "/js/cors": """<title>js cors</title><p id="a">a: waiting</p><p id="b">b: waiting</p><p id="c">c: waiting</p>
<script>
var other = 'http://10.0.2.2.nip.io:8150';
fetch(other + '/api/cors-ok').then(r => r.json()).then(j => { document.getElementById('a').textContent = 'a: allowed ' + j.ok + ', cookies ' + j.cookie + ', from ' + j.origin; });
fetch(other + '/api/cors-no').then(r => r.text()).then(t => { document.getElementById('b').textContent = t; }, e => { document.getElementById('b').textContent = 'b: refused'; });
fetch(other + '/api/cors-put', {method: 'PUT', headers: {'Content-Type': 'application/json'}, body: 'hi'}).then(r => r.json()).then(j => { document.getElementById('c').textContent = 'c: put ' + j.put; }, e => { document.getElementById('c').textContent = 'c: put refused'; });
</script>""",
    "/js/cors-local": """<title>js cors local</title><script>fetch('http://10.0.2.2:8150/api/items').then(() => console.log('LOCAL REACHED'), () => console.log('local refused'));</script>""",
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

KEY, BADKEY = bytes(range(16)), bytes(range(1, 17))

def _seal(b, iv, key=KEY):
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
    n = 16 - len(b) % 16
    e = Cipher(algorithms.AES(key), modes.CBC(iv)).encryptor()
    return e.update(b + bytes([n]) * n) + e.finalize()

def _iv(n): return n.to_bytes(16, "big")

def _aac_mp4():
    """The sound as fragmented MP4 AAC, two seconds a piece, made once by
    GStreamer's encoder and muxer: (init, [pieces]), or None."""
    out = os.path.expanduser("~/.cache/flopos-fuzz/audio/hls-aac.mp4")
    if not os.path.exists(out) and FRAMES:
        with tempfile.TemporaryDirectory() as d:
            src = os.path.join(d, "a.mp3")
            open(src, "wb").write(b"".join(FRAMES))
            prog = ("import gi; gi.require_version('Gst','1.0'); from gi.repository import Gst; Gst.init(None)\n"
                    "p = Gst.parse_launch('filesrc location=%s ! mpegaudioparse ! decodebin ! audioconvert ! audioresample"
                    " ! audio/x-raw,rate=44100,channels=2 ! avenc_aac bitrate=64000 ! mp4mux fragment-duration=2000"
                    " streamable=true ! filesink location=%s')\n"
                    "p.set_state(Gst.State.PLAYING)\n"
                    "p.get_bus().timed_pop_filtered(120 * Gst.SECOND, Gst.MessageType.EOS | Gst.MessageType.ERROR)\n"
                    "p.set_state(Gst.State.NULL)\n") % (src, out + ".part")
            subprocess.run([sys.executable, "-c", prog], timeout=180)
            if os.path.exists(out + ".part") and os.path.getsize(out + ".part"): os.replace(out + ".part", out)
    if not os.path.exists(out): return None
    b, i, boxes = open(out, "rb").read(), 0, []
    while i + 8 <= len(b):
        n = struct.unpack(">I", b[i:i + 4])[0] or len(b) - i
        boxes.append((b[i + 4:i + 8], b[i:i + n])); i += n
    init = b"".join(x for t, x in boxes if t in (b"ftyp", b"moov"))
    pieces, cur = [], b""
    for t, x in boxes:
        if t == b"moof" and cur: pieces.append(cur); cur = b""
        if t in (b"moof", b"mdat"): cur += x
    if cur: pieces.append(cur)
    return init, pieces

MP4 = None

def _aac_ad():
    """Six seconds of the sound as AAC (ADTS) at 32 kHz, made once by
    GStreamer: the advert in /hls/break.m3u8."""
    out = os.path.expanduser("~/.cache/flopos-fuzz/audio/hls-ad.aac")
    if not os.path.exists(out) and FRAMES:
        with tempfile.TemporaryDirectory() as d:
            src = os.path.join(d, "a.mp3")
            open(src, "wb").write(b"".join(FRAMES[len(FRAMES) // 2:len(FRAMES) // 2 + 6 * RATE // 1152]))
            prog = ("import gi; gi.require_version('Gst','1.0'); from gi.repository import Gst; Gst.init(None)\n"
                    "p = Gst.parse_launch('filesrc location=%s ! mpegaudioparse ! decodebin ! audioconvert ! audioresample"
                    " ! audio/x-raw,rate=32000,channels=2 ! avenc_aac bitrate=48000 ! aacparse ! audio/mpeg,stream-format=adts"
                    " ! filesink location=%s')\n"
                    "p.set_state(Gst.State.PLAYING)\n"
                    "p.get_bus().timed_pop_filtered(60 * Gst.SECOND, Gst.MessageType.EOS | Gst.MessageType.ERROR)\n"
                    "p.set_state(Gst.State.NULL)\n") % (src, out + ".part")
            subprocess.run([sys.executable, "-c", prog], timeout=120)
            if os.path.exists(out + ".part") and os.path.getsize(out + ".part"): os.replace(out + ".part", out)
    return open(out, "rb").read() if os.path.exists(out) else None

def hls(p):
    """(content type, body) for an /hls/ path, or None."""
    global MP4
    pl = "application/vnd.apple.mpegurl"
    if p in ("/hls/aes.m3u8", "/hls/badkey.m3u8"):
        k = "key" if "aes" in p else "badkey"
        body = "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:2\n#EXT-X-MEDIA-SEQUENCE:0\n"
        body += '#EXT-X-KEY:METHOD=AES-128,URI="%s",IV=0x000102030405060708090A0B0C0D0E0F\n' % k
        for n in range(min(NSEG, 6)):
            if n == 2: body += '#EXT-X-KEY:METHOD=AES-128,URI="%s"\n' % k
            body += "#EXTINF:2.0,\naes/%d.ts\n" % n
        return pl, (body + "#EXT-X-ENDLIST\n").encode()
    if p == "/hls/break.m3u8":
        body = "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:6\n#EXT-X-MEDIA-SEQUENCE:0\n"
        body += "#EXTINF:2.0,\nts/0.ts\n#EXTINF:2.0,\nts/1.ts\n#EXTINF:2.0,\nts/2.ts\n"
        body += "#EXT-X-DISCONTINUITY\n#EXTINF:6.0,\nad.ts\n#EXT-X-DISCONTINUITY\n"
        body += "#EXTINF:2.0,\nts/3.ts\n#EXTINF:2.0,\nts/4.ts\n#EXTINF:2.0,\nts/5.ts\n#EXT-X-ENDLIST\n"
        return pl, body.encode()
    if p == "/hls/ad.ts":
        ad = _aac_ad()
        return ("video/mp2t", _ts(ad)) if ad else None
    if p == "/hls/key": return "application/octet-stream", KEY
    if p == "/hls/badkey": return "application/octet-stream", BADKEY
    if p.startswith("/hls/aes/") and p.endswith(".ts"):
        n = int(p[9:-3])
        return "video/mp2t", _seal(_ts(_seg(n)), bytes(range(16)) if n < 2 else _iv(n))
    if p.startswith("/hls/fmp4") or p.startswith("/hls/m4s/"):
        if MP4 is None: MP4 = _aac_mp4() or False
        if not MP4: return None
        init, pieces = MP4
        sealed = "aes" in p
        if p.endswith(".m3u8"):
            body = "#EXTM3U\n#EXT-X-VERSION:7\n#EXT-X-TARGETDURATION:3\n#EXT-X-MEDIA-SEQUENCE:0\n"
            if sealed: body += '#EXT-X-KEY:METHOD=AES-128,URI="key",IV=0x0000000000000000000000000000ABCD\n'
            body += '#EXT-X-MAP:URI="m4s/%s"\n' % ("init-aes.mp4" if sealed else "init.mp4")
            if sealed: body += '#EXT-X-KEY:METHOD=AES-128,URI="key"\n'
            for n in range(min(len(pieces), 12)):
                body += "#EXTINF:2.0,\nm4s/%s%d.m4s\n" % ("aes" if sealed else "", n)
            return pl, (body + "#EXT-X-ENDLIST\n").encode()
        if p == "/hls/m4s/init.mp4": return "video/mp4", init
        if p == "/hls/m4s/init-aes.mp4": return "video/mp4", _seal(init, _iv(0xABCD))
        n = p[9:-4]
        if n.startswith("aes"): return "video/iso.segment", _seal(pieces[int(n[3:])], _iv(int(n[3:])))
        return "video/iso.segment", pieces[int(n)]
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
    def send(self, code, ctype, body, extra=(), length=True):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        if length: self.send_header("Content-Length", str(len(body)))
        for k, v in extra: self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)
    # Another site's API, for CORS (reached as 10.0.2.2.nip.io: another
    # site by name, the same server): what it lets other pages read.
    def do_OPTIONS(self):
        if self.path == "/api/cors-put":
            return self.send(204, "text/plain", b"", [("Access-Control-Allow-Origin", "*"),
                                                      ("Access-Control-Allow-Methods", "GET, PUT")])
        return self.send(204, "text/plain", b"")
    # test/bench/webprobe.py: the list of what to look for, and what was found
    def do_POST(self):
        n = int(self.headers.get("Content-Length") or 0)
        got = self.rfile.read(n) if n else b""
        if self.path == "/api/upload":                     # a speed test's upload: counted, answered
            return self.send(200, "text/plain", str(len(got)).encode(), [("Access-Control-Allow-Origin", "*")])
        if self.path.startswith("/api/probe?part="):
            out = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), "build", "webprobe", "got.txt")
            open(out, "wb" if self.path.endswith("=0") else "ab").write(got)
            return self.send(200, "application/json", b'{"ok": true}')
        return self.send(404, "text/plain", b"not here")
    def do_PUT(self):
        n = int(self.headers.get("Content-Length") or 0)
        got = self.rfile.read(n) if n else b""
        return self.send(200, "application/json", b'{"put": "' + got.replace(b'"', b"") + b'"}', [("Access-Control-Allow-Origin", "*")])
    def do_GET(self):
        p = self.path
        if p == "/api/cors-ok":
            cookie = self.headers.get("Cookie") or ""
            return self.send(200, "application/json", json.dumps({"ok": "yes", "cookie": cookie or "none", "origin": self.headers.get("Origin") or "none"}).encode(),
                             [("Access-Control-Allow-Origin", "*"), ("Set-Cookie", "THIRDPARTY=1; path=/")])
        if p.startswith("/api/stream"):                   # a speed test's download: 3 MB, read as it comes
            return self.send(200, "application/octet-stream", bytes(range(256)) * 12288)
        if p == "/js/echo.js":
            return self.send(200, "application/javascript", b"onmessage = function (e) { postMessage(e.data * 2); };")
        if p == "/js/worker.js":
            return self.send(200, "application/javascript", WORKER.encode())
        if p == "/api/big":                                 # past what one door answer holds: comes in pieces
            body = json.dumps({"items": ["item %05d %s" % (i, "x" * 80) for i in range(3000)]}).encode()
            return self.send(200, "application/json", body, [("X-Test", "yes")])
        if p == "/api/cors-no":
            return self.send(200, "application/json", b'{"secret": "SECRET LEAKED"}')
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
        if p.startswith("/codecs/"):
            name = os.path.basename(p)
            f = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "build", "audiotest", "codecs", name)
            if os.path.isfile(f):
                ctype = {"mp2": "audio/mpeg", "mp3": "audio/mpeg", "m4a": "audio/mp4", "opus": "audio/ogg"}.get(name.rsplit(".", 1)[-1], "application/octet-stream")
                return self.send(200, ctype, open(f, "rb").read(), length="/live/" not in p)
        if p.startswith("/hls/"):
            h = hls(p)
            if h: return self.send(200, h[0], h[1])
        if p in JS:
            ctype = "application/javascript" if p.endswith(".js") else "application/json" if p.startswith("/api/") else "text/html; charset=UTF-8"
            return self.send(200, ctype, JS[p].encode())
        if p.startswith("/api/probe-list?part="):          # in parts: browzer's fetch() holds 64 KB
            keys = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), "build", "webprobe", "keys.json")
            allk, n = (json.load(open(keys)) if os.path.exists(keys) else []), int(p.split("=")[1])
            return self.send(200, "application/json", json.dumps(allk[n * 1000:(n + 1) * 1000]).encode())
        if p in PAGES: return self.send(200, "text/html", PAGES[p].encode())
        if p.startswith("/collect?"): return self.send(200, "text/html", JS["/collect"].encode())   # a form sent here
        self.send(404, "text/plain", b"not here")

if __name__ == "__main__":
    # https too, on the next port, under the test bench's own authority
    # (test/bench/testca.py), which only test images trust.
    try:
        import importlib.util, ssl, threading
        spec = importlib.util.spec_from_file_location("testca", os.path.join(os.path.dirname(os.path.abspath(__file__)), "testca.py"))
        testca = importlib.util.module_from_spec(spec); spec.loader.exec_module(testca)
        ca, crt, key = testca.files()
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); ctx.load_cert_chain(crt, key)
        tls = http.server.ThreadingHTTPServer(("127.0.0.1", PORT + 1), Handler)
        tls.socket = ctx.wrap_socket(tls.socket, server_side=True)
        threading.Thread(target=tls.serve_forever, daemon=True).start()
    except Exception as e:
        sys.stderr.write("no https on %d: %s\n" % (PORT + 1, e))
    http.server.ThreadingHTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
