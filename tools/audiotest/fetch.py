#!/usr/bin/env python3
"""Fetch the test songs in songs.txt from The Mod Archive into a folder.

    tools/audiotest/fetch.py DIR        (DIR outside the repository)

Each is saved as DIR/<id>.<ext>.  Files already there are not fetched again.
"""
import os, sys, urllib.request

def main():
    if len(sys.argv) != 2: sys.exit(__doc__)
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    for line in open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "songs.txt")):
        if not line.strip() or line.startswith("#"): continue
        mid, name = line.split()[:2]
        path = os.path.join(out, mid + os.path.splitext(name)[1])
        if os.path.exists(path): continue
        req = urllib.request.Request("https://api.modarchive.org/downloads.php?moduleid=" + mid,
                                     headers={"User-Agent": "FLOPOS test tools"})
        with urllib.request.urlopen(req, timeout=60) as r: data = r.read()
        open(path, "wb").write(data)
        print("%s  %d bytes" % (path, len(data)))

if __name__ == "__main__":
    main()
