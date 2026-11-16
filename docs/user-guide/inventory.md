# The inventory: your files

Everything you make lives in the inventory: on the floppy, a folder
called `INVENTORY` of ordinary files and folders. The system's own files
sit beside it and are never mixed with yours.

    here> inventory
    here> inventory NOTES        start in a folder

## Keys

- **Up, Down, PgUp, PgDn:** Move.
- **Enter:** Into a folder, or open a document in `scribe` (leaving `scribe` comes back here).
- **Backspace:** Up a folder.
- **W:** Write a new document in this folder.
- **N:** A new folder.
- **R:** Rename.
- **M:** Pick it up; walk to the folder it belongs in and press **P** to put it down. Esc puts it back.
- **D:** Drop it for good, after asking. A folder must be empty.
- **Esc:** Leave.

## Names

Up to eight letters or digits, a dot, and up to three more: `LETTER.MD`.
Folders have no dot and nest seven deep. That is FAT12's own format, so
any computer reads the floppy. A long name copied in from elsewhere shows
by its short form (`MYLONG~1.MD`).

## On another computer

    INVENTORY\        everything of yours, as plain files
    README.TXT        what this floppy is
    JOURNAL.TXT       FLOPOS's diary, plain text
    KERNEL.BIN, *.APP, WIZARD.WIZ ...   FLOPOS itself; leave these

Documents are plain text (`.MD`). Files copied into `INVENTORY` from
another computer show up in FLOPOS.

## How safe it is

Saves are ordered so a power cut leaves each file whole, old or new
(see Techniques[^1]). Three things to know:

- A cut can leave a little space marked used; any disk checker frees it.
- On a floppy too full for the new version beside the old, the old goes first, and a cut then can cost the file.
- A move cut halfway lists the thing in both folders. Drop one.

## New releases

`tools/floppy/writefloppy.sh` carries the whole inventory, the journal,
the settings and the bookmarks to a new release. If they wouldn't fit,
nothing is written.

[^1]: [Techniques](../design/techniques.md#files-and-paper)
