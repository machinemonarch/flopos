# Commands

This is cyberdelia, the shell, where you land once the wizard at the gate
has had his riddle answered (see [Getting started](getting-started.md#booting)).

Type a word at `here>` and press Enter. Capital letters don't matter. A word
FLOPOS doesn't know gets "I don't know that word." and goes into the journal,
so it can be considered for next time.

`help` lists the everyday words. The behind-the-scenes ones at the end of
this page are left out of `help` on purpose.

## Being here

| Word | What it does |
|---|---|
| `whoami` | Who you are. ("You're alright.") |
| `look` | What is around. |
| `here` | Where you are. |
| `echo words` | Says the words back. |
| `time` | The time and date from the laptop's clock, and how long FLOPOS has been awake. |
| `sys` | What this machine actually is: processor, measured speed, memory, screen mode, panel. |
| `version` | Which FLOPOS this is. |
| `help` | The list. |

FLOPOS also understands a handful of ordinary questions and pleasantries,
however they are typed: *who are you*, *what's your name*, *how are you*,
*are you alive*, *are you real*, *are you there*, *hello* / *hej*,
*thanks* / *tack*, *sorry*, *please*, *yes* / *ja*, *no* / *nej*,
*why*, *what now*, *good night*, and a few more. "What time is it" shows the
clock.

## Light, sound and screen

| Word | What it does |
|---|---|
| `mood` | Changes the light: the colours of the moving background. Moves to the next one, or name it: `mood dusk`. The five are STILL, WARM, DEEP, DUSK and VOID. |
| `listen` | A different rhythm: JUNGLE, BREAKS, DRIFT, ACID, RAGA, TEMPLE, SPIRAL. |
| `sound` / `quiet` | Music on or off. |
| `whaaat` | Pulls the screen through a hole in itself, turns it inside out, and drops you back somewhere colder. Say it again to come back up. |
| `quirks` | Lines of text sometimes misbehave on purpose. This holds them still, or lets them go again. |
| `squint` | Switches between the larger and the smaller text, for more room. |
| `screen big` / `screen small` | 800×600 with smooth text (the default), or the chunky 320×200 picture. `screen` alone says which is on. |
| `wide` | Only at 320×200: stretches the picture to fill the whole panel, or puts it back in its box. |
| `goaway` | Wipes the screen. |

## Remembering

FLOPOS keeps two files on the floppy, `JOURNAL.TXT` and `STATE.BIN`, and
writes to them only when something asks it to.

| Word | What it does |
|---|---|
| `teach word "answer"` | Gives FLOPOS a new word. From then on, typing the word gets the answer. Up to 48 words, kept on the disk. Example: `teach frobnicate "Done. Probably."` |
| `forget word` | Takes a taught word away. |
| `learned` | Lists what you have taught it. |
| `journal` | Shows what FLOPOS has written down: words taught and forgotten, and every word it didn't recognise. |
| `remember` | Writes everything to the disk now. |

The journal is plain text, so you can read it on any computer: open
`JOURNAL.TXT` on the floppy.

## Programs

| Word | What it does |
|---|---|
| `write` | The word processor. `write 3` opens document 3. See [write](write.md). |
| `apps` | Lists the programs on the floppy. Typing a program's name runs it. |
| `browzer` | A small web browser, with forms. An app on the floppy. See [The network](network.md). |

## The network

| Word | What it does |
|---|---|
| `network` | A menu showing the network card, the connection and the address, where you can choose automatic setup or set it by hand. |
| `ping place` | Asks a machine on the network whether it is there and times the answer: `ping example.com`, `ping 1.1.1.1`. |
| `fetch address` | Reads a web page out as text in the terminal: `fetch example.com`, `fetch https://...`. (This is browzer, reading instead of showing.) |

More in [The network](network.md).

## Stopping

| Word | What it does |
|---|---|
| `bye` | Writes everything down and switches the laptop off. |
| `reboot` | Starts the machine again. |

**Ctrl-Alt-Delete** restarts from anywhere, including the crash screen.

## Keys at the prompt

| Key | What it does |
|---|---|
| Left / Right | Move along the line |
| Home / End, Ctrl-A / Ctrl-E | Jump to the start or end |
| Backspace / Delete | Remove a character either way |
| Ctrl-U / Ctrl-K | Remove everything before / after the cursor |
| Up / Down | Walk back through what you have typed |
| Page Up / Page Down | Look back through what has scrolled away |
| Esc | Abandon the line |

The keyboard layout is **Swedish**. Letters are where they are on any
QWERTY keyboard; symbols are where a Swedish keyboard prints them: `"` is
Shift-2, `/` is Shift-7, `@` is AltGr-2, `\` is AltGr and the plus key.
Å, Ä and Ö have their own letters in the font.

## Behind the scenes

These are for finding out what is wrong. They are left out of `help`, and
`report` runs all of them at once and sends the results down the serial
cable, so normally you only ever need `report`.

| Word | What it does |
|---|---|
| `report` | Everything below, sent down the serial cable in one go. |
| `vesa` | What the video card can do: its memory and screen modes. |
| `card` | What is in the two PC Card slots, as the cards describe themselves. `card raw` shows their raw bytes. |
| `net` | Switches the network card on and reports what it found. |
| `ip` | The address, mask, way out (gateway) and name server. `ip 192.168.1.50 255.255.255.0 192.168.1.1` sets them by hand. |
| `crypto` | Checks every piece of the encryption code against known answers, and times the slow parts. |
| `wire` | The serial line: its settings and how much has gone each way. |
| `speed` | Where each frame's time goes. |
| `disk` | Reads the first sector of the floppy back, to prove the disk still answers. |
| `frames` | Draws the frame and shading pieces used by menus. |
| `dump address` | Shows the bytes at a place in memory. |

Test builds (made with `EXTRA_CFLAGS=-DFLOPOS_TEST`) also have `stall`,
which reproduces the old freeze on purpose to prove the fix holds.
