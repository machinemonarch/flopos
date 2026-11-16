#!/usr/bin/env python3
"""doccheck.py -- every link in the project's Markdown leads somewhere.

    test/doccheck.py

Checks each relative link in the tracked .md files (not releases/,
reference/ or logs/): the file is there, and a #section names a heading
in it (the way GitHub turns a heading into an anchor).  Prints ok, or
FAILED and each broken link.
"""
import os, re, subprocess, sys

root = subprocess.check_output(["git", "rev-parse", "--show-toplevel"]).decode().strip()
os.chdir(root)
files = [l for l in subprocess.check_output(["git", "ls-files", "--cached", "--others", "--exclude-standard", "*.md"]).decode().split("\n")
         if l and not l.startswith(("releases/", "reference/", "logs/")) and os.path.exists(l)]

def anchors(path):
    out, seen = set(), {}
    fence = False
    for line in open(path, encoding="utf-8"):
        if line.startswith("```"): fence = not fence
        if fence: continue
        m = re.match(r"#{1,6}\s+(.*?)\s*#*\s*$", line)
        if not m: continue
        a = m.group(1).strip().lower()
        a = re.sub(r"[`*_]", "", a)
        a = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", a)
        a = re.sub(r"[^\w\- ]", "", a).replace(" ", "-")
        n = seen.get(a, 0); seen[a] = n + 1
        out.add(a if n == 0 else f"{a}-{n}")
    return out

bad = []
link = re.compile(r"\]\(([^)\s]*)\)")
for f in files:
    text = open(f, encoding="utf-8").read()
    text = re.sub(r"```.*?```", "", text, flags=re.S)
    for m in link.finditer(text):
        target = m.group(1)
        if re.match(r"[a-z]+:", target) or not target: continue
        # The repository's own pages on GitHub (its releases, its issues),
        # which exist there and not in a checkout.
        if f == "README.md" and re.match(r"\.\./\.\./(releases|issues)\b", target): continue
        path, _, anchor = target.partition("#")
        dest = os.path.normpath(os.path.join(os.path.dirname(f), path)) if path else f
        if not os.path.exists(dest): bad.append(f"{f}: {target} (no such file)"); continue
        if anchor and dest.endswith(".md") and anchor not in anchors(dest):
            bad.append(f"{f}: {target} (no such section)")
if bad:
    print("FAILED"); print("\n".join(bad)); sys.exit(1)
print("ok")
