#!/usr/bin/env python3
"""The scripts of the scoreboard's sites, fetched on this PC, for counting
what real pages use (test/bench/webprobe.py ranks the missing by it).

    test/sites/corpus.py

For each site in sites.txt: its page, the scripts written in it, and the
scripts it names (any site, https; 40 a site at most), into
~/.cache/flopos-corpus/<site>/.  Many at once.  A site that refuses this
PC is left out and said so.
"""
import os, re, sys, urllib.parse, urllib.request
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.expanduser("~/.cache/flopos-corpus")


def get(url):
    req = urllib.request.Request(url, headers={"Accept": "*/*"})
    with urllib.request.urlopen(req, timeout=20) as r:
        return r.read(4 << 20).decode("utf-8", "replace")


def one(url):
    host = urllib.parse.urlparse(url).hostname
    d = os.path.join(OUT, host)
    os.makedirs(d, exist_ok=True)
    try: page = get(url)
    except Exception as e: return "%-32s refused (%s)" % (host, str(e)[:40])
    n = 0
    for i, body in enumerate(re.findall(r"<script\b[^>]*>(.*?)</script>", page, re.S | re.I)):
        if body.strip(): open(os.path.join(d, "inline-%d.js" % i), "w").write(body); n += 1
    srcs = [urllib.parse.urljoin(url, s) for s in re.findall(r"<script\b[^>]*\bsrc=[\"']([^\"']+)", page, re.I)]
    for i, s in enumerate([s for s in srcs if s.startswith("https://")][:40]):
        try: open(os.path.join(d, "src-%d.js" % i), "w").write(get(s)); n += 1
        except Exception: pass
    return "%-32s %d scripts" % (host, n)


def main():
    urls = []
    for line in open(os.path.join(HERE, "sites.txt")):
        line = line.strip()
        if line and not line.startswith("#"):
            urls.append(line[4:].split()[0] if line.startswith("out ") else line.split()[0])
    with ThreadPoolExecutor(32) as ex:
        for r in ex.map(one, urls): print(r)


if __name__ == "__main__":
    main()
