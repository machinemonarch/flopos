# Releasing

## Version numbers

`MAJOR.MINOR.PATCH`, kept in `VERSION`:

- **minor** (1.10.0 → 1.11.0) for new things
- **patch** (1.10.0 → 1.10.1) for fixes

`version` says which one is running.

## Making a release

1. Describe the changes under `## Unreleased` at the top of
   `CHANGELOG.md`. Write for the person using FLOPOS, not for a
   programmer.
2. Run `make release PART=minor` (or `PART=patch`).

The release refuses to start if `## Unreleased` is empty. Otherwise it:

1. bumps the version and regenerates `kernel/version.h`
2. builds everything from clean
3. copies the image to `releases/flopos-X.Y.Z.img`, with its `.used` file
4. moves the Unreleased notes under the new version number with today's
   date, and writes them beside the image as `releases/flopos-X.Y.Z.txt`

Any old version can be put back on a floppy from `releases/`.
