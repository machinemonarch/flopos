#!/usr/bin/env python3
"""A local test site for browzer, imitating the sites it has had trouble with.

    tools/bench/site.py [PORT] [SONGS]     (default 8150; SONGS: fetch.py's folder)

QEMU's guest reaches it at http://10.0.2.2:PORT/.  Pages:
  /m/ID              a song, sent as The Mod Archive sends it (octet-stream,
                     Content-Disposition, a cookie) from SONGS/ID.*
  /refresh           DuckDuckGo's front page: a <noscript> refresh to the
                     plain page, its address in &quot;, then 200 KB of script
  /plain             where that leads
  /code              72fm.com: stream addresses as text in <code>, one long
  /data              ERICADE Radio: streams in data attributes as JSON,
                     entity-encoded, one relative, one with \\/ escapes
  /public/chiptune   an AzuraCast station page: no stream, only its art
  /api/nowplaying/chiptune   the station's JSON, naming its streams
  /radio/il          72fm.com's country page: names only, and the Radio
                     Browser directory named in a <link rel="preconnect">
  /radio-browser/json/stations/bycountrycodeexact/IL   that directory's answer
"""
import glob, http.server, json, os, sys

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8150
SONGS = sys.argv[2] if len(sys.argv) > 2 else os.path.expanduser("~/.cache/flopos-songs")
HOST = "http://10.0.2.2:%d" % PORT

PAGES = {
    "/plain": "<title>plain</title><h1>The plain page</h1><p>Arrived by the refresh.</p>",
    "/code": ("<title>72fm-like</title><h1>A station</h1><p>Its stream address:</p>"
              "<p><code class=\"break-all\">%s/listen/radio.mp3</code></p>"
              "<p>and a long one, longer than a word can be: %s/a/very/long/path/that/goes/on/and/on/"
              "for/more/than/eighty/characters/stream.mp3.</p>" % (HOST, HOST)),
    "/data": ("<title>ERICADE-like</title><h1>Radio</h1><div data-stations=\"[{&quot;id&quot;:1,&quot;name&quot;:"
              "&quot;Live&quot;,&quot;stream&quot;:&quot;/sc/stream/1/&quot;}]\"></div><div data-show=\"{&quot;title"
              "&quot;:&quot;Show 170&quot;,&quot;image&quot;:&quot;http:\\/\\/10.0.2.2:%d\\/art.webp&quot;,"
              "&quot;url&quot;:&quot;http:\\/\\/10.0.2.2:%d\\/Flashback\\/170.mp3&quot;}\"></div>"
              "<script>var api='%s/api/radio/v2/listening';</script>" % (PORT, PORT, HOST)),
    "/radio/il": ("<html><head><link rel=\"preconnect\" href=\"%s/radio-browser\" /><title>72fm-like</title></head>"
                  "<body><h1>Radio stations from Israel</h1><ul><li>Radio 103FM</li><li>Glglz</li></ul></body></html>" % HOST),
    "/public/chiptune": ("<title>24/7 Chiptune Radio</title><div id=\"app\"></div>"
                         "<img src=\"/api/nowplaying/chiptune/art/1.jpg\"><script>/* a player */</script>"),
}

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
        if p in PAGES: return self.send(200, "text/html", PAGES[p].encode())
        self.send(404, "text/plain", b"not here")

if __name__ == "__main__":
    http.server.ThreadingHTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
