#!/usr/bin/env python3
"""The changelog's part in a release.

    changelog.py check            fail unless "## Unreleased" has something in it
    changelog.py stamp VERSION OUT
                                  turn "## Unreleased" into "## VERSION -- date",
                                  open a fresh empty "## Unreleased" above it,
                                  and write that version's notes to OUT

Called by `make release`: check before anything is bumped or built, so a
release with nothing written about it never starts; stamp after the image
exists, so a failed build leaves the notes where they were.
"""
import datetime
import sys

PATH = "CHANGELOG.md"
HEAD = "## Unreleased"


def unreleased(lines):
    start = next(i for i, l in enumerate(lines) if l.strip() == HEAD)
    end = next((i for i in range(start + 1, len(lines)) if lines[i].startswith("## ")),
               len(lines))
    return start, end


def main():
    path = sys.argv[4] if len(sys.argv) > 4 else PATH
    lines = open(path, encoding="utf-8").read().split("\n")
    start, end = unreleased(lines)
    notes = lines[start + 1:end]
    while notes and not notes[-1].strip():
        notes.pop()
    while notes and not notes[0].strip():
        notes.pop(0)

    if sys.argv[1] == "check":
        if not notes:
            sys.exit("\n  No release without a changelog entry: write what changed under\n"
                     "  \"## Unreleased\" in CHANGELOG.md first.\n")
        return

    version, out = sys.argv[2], sys.argv[3]
    today = datetime.date.today().isoformat()
    stamped = (lines[:start] + [HEAD, "", "## %s — %s" % (version, today), ""] +
               notes + [""] + lines[end:])
    open(path, "w", encoding="utf-8").write("\n".join(stamped))
    with open(out, "w", encoding="utf-8") as f:
        f.write("FLOPOS %s, %s\n\n" % (version, today))
        f.write("\n".join(notes) + "\n")


main()
