# Releasing

## Version numbers

`MAJOR.MINOR.PATCH`, kept in `VERSION`:

- **minor** (1.10.0 → 1.11.0) for new things
- **patch** (1.10.0 → 1.10.1) for fixes

`version` says which one is running.

## The changelog

`CHANGELOG.md` is the story of every release, newest first. At its very
top is a heading, `## Unreleased`, with a list of what has changed since
the last release. Whenever something changes that someone using FLOPOS
would notice, a line is added to that list, in plain words, as the user
sees it ("Hebrew is drawn right to left"), not as the code does it.

When a release is made, that list becomes the new version's notes: it is
moved under the version number and copied beside the image. That is why
a release refuses to start when the list is empty: there would be nothing
to say about it. Keeping the list in the changelog itself, rather than in
a separate file, means anyone reading the repository can see what the
next release will bring.

## Making a release

1. Check the list under `## Unreleased` in `CHANGELOG.md` says everything
   that changed.
2. Run `make release PART=minor` (or `PART=patch`).

The release refuses to start if `## Unreleased` is empty. Otherwise it:

1. bumps the version and regenerates `kernel/version.h`
2. builds everything from clean
3. copies the image to `releases/flopos-X.Y.Z.img`, with its `.used` file
4. moves the Unreleased notes under the new version number with today's
   date, and writes them beside the image as `releases/flopos-X.Y.Z.txt`

Any old version can be put back on a floppy from `releases/`.
