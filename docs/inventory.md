# The inventory: your files

Everything you make in FLOPOS lives in one place: the inventory. On the
floppy it is a folder called `INVENTORY`, holding ordinary files and
folders. The system's own files sit beside that folder and are never
moved, renamed or mixed with yours.

    here> inventory

## On another computer

Put the floppy in any computer and it shows:

    INVENTORY\        <- everything of yours, as plain files and folders
    README.TXT        <- what this floppy is
    JOURNAL.TXT       <- FLOPOS's own diary, plain text
    KERNEL.BIN, *.APP, WIZARD.WIZ ...   <- FLOPOS itself; leave these

Documents are plain text (`.MD`) that any editor opens. You can also copy
files into `INVENTORY` from another computer and FLOPOS will list them.
Keep names short when you do: FLOPOS shows a long name by its short DOS
form (`MYLONG~1.MD`).

## In the inventory

The list shows each thing's name, kind, size and the day it last changed,
folders first. The bottom edge shows how much room is left on the floppy.

| Key | What it does |
|---|---|
| Up, Down, PgUp, PgDn | Move along the list |
| <kbd>Enter</kbd> | Step into a folder, or open a document in `write` (leaving `write` comes back here) |
| <kbd>Backspace</kbd> | Up a folder |
| <kbd>W</kbd> | Write a new document, to be kept in this folder |
| <kbd>N</kbd> | A new folder here |
| <kbd>R</kbd> | Rename |
| <kbd>M</kbd> | Move: pick the thing up. Walk to the folder it should be in and press <kbd>P</kbd> to put it down. <kbd>Esc</kbd> puts it back. |
| <kbd>D</kbd> | Drop it for good. It asks first. A folder must be empty. |
| <kbd>Esc</kbd> | Leave |

`inventory NOTES` starts in that folder.

## Names

A name is up to eight letters or digits, then a dot and up to three more:
`LETTER.MD`, `SONG.MOD`. That is the floppy's own format (FAT12, the same
one every DOS and Windows floppy uses), which is what lets any computer
read it. Folders have no dot. Folders can sit inside folders, seven deep.

## How safe it is

FLOPOS writes in an order that keeps the floppy whole if the power goes
in the middle of a save: the new contents first, in space nothing uses;
then the floppy's table of what is where, its spare copy before the main
one; then the file's entry in its folder; and only then is the old
version let go. After a cut, a file is either the old version or the new
one, complete.

Three things to know:

- A cut can leave a little space marked as used that nothing owns. A
  disk checker on any computer gives it back.
- If the floppy is so full that the new version cannot sit beside the old
  one, the old one has to go first. Only then can a cut during the save
  cost you the file.
- A move between folders, cut halfway, leaves the thing listed in both.
  Nothing is lost; drop one of them.

This is tested on the PC (`tools/fstest/run.py`): thousands of random
changes checked by the PC's own disk tools, and hundreds of simulated
power cuts. It has not yet run on the laptop's real floppy drive.

## New releases

Writing a new release to the floppy (`tools/writefloppy.sh`) carries the
whole inventory across, with the journal, the taught words and the
bookmarks. Documents from releases before 1.31.0 (`WRITE1.MD` to
`WRITE8.MD`) are moved into the inventory under the same names. If your
files would not fit beside the new release, nothing is written.
