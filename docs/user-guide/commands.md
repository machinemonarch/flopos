# Commands

Type a word at `here>` and press Enter; capitals don't matter. `help`
lists the everyday words. A word FLOPOS doesn't know goes into the
journal, and the wizard answers it with a line from his tiny language
model: he knows no facts, only sentences to put your word into.

FLOPOS also answers ordinary questions and pleasantries (*who are you*,
*how are you*, *hello* / *hej*, *thanks* / *tack*, *what time is it* and
more).

## Being here

- **`whoami`, `look`, `here`:** Who and where you are.
- **`time`:** Time and date, uptime, the clock battery, and how far off the network found the clock.
- **`sys`:** Processor, speed, memory, screen, panel.
- **`version`:** Which FLOPOS this is.

## Light, sound and screen

- **`mood`:** The next colours for the background, or name one: STILL, WARM, DEEP, DUSK, VOID.
- **`listen`:** The next rhythm: JUNGLE, BREAKS, DRIFT, ACID, TEMPLE, SPIRAL.
- **`sound` / `quiet`:** Music on or off.
- **`whaaat`:** Try it. Again to come back.
- **`quirks`:** Stops text misbehaving, or lets it again.
- **`squint`:** Text size: easy (the default), large, close or fine.
- **`keyboard`:** The layout: `keyboard de`. Alone, lists them.
- **`screen big` / `screen small`:** 800×600 or 320×200.
- **`wide`:** At 320×200 only: fill the panel.
- **`goaway`:** Clear the screen.

## Remembering

- **`journal`:** What FLOPOS wrote down, including every word it didn't know.
- **`remember`:** Write everything to the floppy now.

`JOURNAL.TXT` is plain text any computer can read.

## Programs

- **`inventory`:** Your files and folders. See The inventory[^1].
- **`scribe`:** The word processor; `scribe letter` opens `LETTER.MD`. See scribe[^2].
- **`maelcum`:** Backup to cassette. See maelcum[^3].
- **`browzer`:** The web browser. See browzer[^4].
- **`apps`:** The programs on the floppy. A program's name runs it.

## The network

- **`network`:** The connection menu.
- **`ping place`:** `ping example.com`.
- **`browzer fetch address`:** A page as text in the terminal.

More in The network[^5].

## Stopping

- **`wizard`:** Locks the screen until his riddle is answered.
- **`bye`:** Writes everything down and switches off.
- **`reboot`:** Starts again.
- **Ctrl-Alt-Delete:** Restarts from anywhere, the crash screen included.
- **Ctrl-Shift-F5:** Switches the screen again, from anywhere. For a black screen at the start.

## The HUD

**Ctrl-Shift-F10** opens a small window in the top right, over anything:
sound bars, the battery, **'Thunking** (how busy the processor is),
**'Membering** (memory claimed), a brain coloured blue to red by the two,
and a command line that takes the same words as `here>`. Esc hands the
keys back and leaves it showing; Ctrl-Shift-F10 again closes it. Long
answers make it grow; PgUp, PgDn and the arrows scroll.

## Keys at the prompt

- **Left / Right:** Move along the line.
- **Home / End, Ctrl-A / Ctrl-E:** Start or end.
- **Backspace / Delete:** Remove a character.
- **Ctrl-U / Ctrl-K:** Remove before / after the cursor.
- **Up / Down:** Earlier lines.
- **PgUp / PgDn:** Scroll back.
- **Esc:** Drop the line.
- **Ctrl-C:** Drop the line, leaving it on screen marked `^C`.

## Keyboards

FLOPOS starts on **Swedish**. `keyboard` and a code changes it, and the
choice is kept on the floppy (a new release starts on Swedish again):

`se` Swedish, `fi` Finnish, `no` Norwegian, `dk` Danish, `us` American,
`uk` British (or `gb`), `de` German, `fr` French, `be` Belgian, `es`
Spanish, `it` Italian, `nl` Dutch (the American keys), `ro` Romanian,
`bg` Bulgarian (BDS), `ua` Ukrainian, `il` Hebrew (or `he`), `yi`
Yiddish.

- **Lost?** Ctrl-Shift-F1 opens a list of every keyboard, from anywhere. Arrows pick, Enter chooses, Esc leaves.
- **Two alphabets:** On Bulgarian, Ukrainian, Hebrew and Yiddish, Alt+Shift switches to the American keys and back. Ctrl shortcuts go by the Latin letter on the key.
- **Right to left:** Hebrew and Yiddish are drawn right to left, numbers and brackets the right way; text is saved in typed order, as ordinary Unicode.
- **Accent keys** wait for the next letter: ´ then e is é. Accent then space types the accent alone.
- **AltGr** is the right Alt: on Swedish, `@` is AltGr-2, `\` AltGr-plus, `€` AltGr-E.

**Yiddish** is the Hebrew keyboard with its own letters on AltGr (keys
named by their Latin letter):

- **AltGr:** T אַ, Y װ, G ױ, H ײ, J יִ, U וּ, P פּ, F כּ, A שׂ, C בֿ, comma תּ.
- **AltGr-Shift:** T אָ, H ײַ, P פֿ.

Signs the font lacks (² º ª ¬ ½ ¤ № ₪) type nothing. Inventory names stay
plain Latin letters and digits, as FAT12 expects.

## Behind the scenes

Left out of `help`; for finding faults. `report` runs them all and sends
the results down the serial line.

- **`report`:** Everything below, down the serial line.
- **`vesa`:** The video card's memory and modes.
- **`card`:** The PC Card slots; `card raw` shows the bytes.
- **`net`:** Switches the network card on and says what it found.
- **`dsp`:** Plays a chime through the Sound Blaster and measures its rate.
- **`ip`:** Address, mask, gateway, name server.
- **`crypto`:** Checks the encryption against known answers and times it.
- **`wire`:** The serial line's settings and traffic.
- **`speed`:** Where each frame's time goes.
- **`disk`:** Reads the first sector back.
- **`frames`:** Draws the frame pieces.
- **`dump address`:** Bytes at a place in memory.

Test builds (`make EXTRA_CFLAGS=-DFLOPOS_TEST`) add `stall` and a pretend
tape.

[^1]: [The inventory](inventory.md)
[^2]: [scribe](scribe.md)
[^3]: [maelcum](maelcum.md)
[^4]: [browzer](browzer.md)
[^5]: [The network](network.md)
