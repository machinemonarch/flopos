# Releasing

## Version numbers

`MAJOR.MINOR.PATCH`, kept in `VERSION`: **minor** for new things,
**patch** for fixes. `version` says which is running.

## The changelog

`CHANGELOG.md`, newest first. Its top heading is `## Unreleased`: whenever
something changes that a user would notice, add a line there, in plain
words as the user sees it ("Hebrew is drawn right to left"), under
`### FEATURES`, `### BUG FIXES` or `### SECURITY FIXES`.

## Making a release

Check the Unreleased list says everything, then:

    make release PART=minor        (or PART=patch)

It refuses if Unreleased is empty. Otherwise it bumps the version
(`src/kernel/gen/version.h`), builds from clean, copies the image to
`releases/flopos-X.Y.Z.img`, and moves the Unreleased notes under the new
version with today's date, also writing them to
`releases/flopos-X.Y.Z.txt`.
