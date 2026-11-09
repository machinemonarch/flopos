# FLOPOS changelog

Every release gets an entry here, and `make release` will not build one
without it. Each version's notes are also written next to its image, as
`releases/flopos-X.Y.Z.txt`.

## Unreleased

## 1.11.0 — 2026-11-09

- **write: a Markdown view.** Ctrl-M shows the document as the Markdown it
  is saved as, and you can edit it there: the marks are dim, and the words
  between them already look bold, slanted, underlined or like headings.
  Ctrl-M again goes back to the page, with the cursor on the same word.
  Ctrl-B, I and U put the marks in for you, or take them out.
- **Documentation**, in `docs/`: the philosophy, every command, write, the
  network, the hardware, how it works inside, making apps, building and
  releasing, and the references consulted. The README is rewritten to match
  what FLOPOS is now.

## 1.10.0 — 2026-11-09

- **`write`, a small word processor**: bold, italic and underline (Ctrl-B, I,
  U), a title and four heading levels (Ctrl-P, or Ctrl-T and Ctrl-0 to 4),
  Shift-arrows to select. Eight documents live on the floppy (Ctrl-S, Ctrl-O),
  saved as plain text any other computer can read.
- **Apps load from the floppy.** `write` is the first program kept in its own
  file instead of inside the system, which leaves room to grow and costs nothing
  at boot. `apps` lists what's there.
- FLOPOS can now read any file on the floppy by name, including ones copied onto
  it from another computer.
- Text in windows can be bold, italic and underlined.
- Tools: writing a new version to the floppy now keeps your documents, journal and
  taught words (`--fresh` to start clean).

## 1.9.0 — 2026-11-09

- **Post-quantum encryption.** Secure connections offer X25519MLKEM768, so
  recorded traffic stays private even from a future quantum computer. Sites
  that don't support it fall back to the previous key exchange automatically.
- **A second cipher, AES-128-GCM**, for sites that don't speak ChaCha20 (the
  likely reason LinkedIn hung).
- **Esc cancels a secure connection that's stuck**, and a silent server is given
  up on after 15 seconds with a reason.
- **browzer shows images as boxes** sized like the original, saying
  "Image of …". Small icons stay inline; tracking pixels disappear.
- **Pages' no-JavaScript sections are now shown.**
- `crypto` also checks the new encryption code and times it.
- browzer's status messages go down the serial line.

## 1.8.0 — 2026-11-09

- **browzer**, a small web browser: address bar, links (Tab / Enter), back
  (Backspace), scrolling, a home page, and search through FrogFind when you type
  words instead of an address.
- A page that fails to load says why, on the page.
- Tab, PgUp, PgDn, Home and End work in menus, from the keyboard and from picocom.

## 1.7.0 — 2026-11-09

- **TCP, HTTP and TLS 1.3**: real web pages, plain and secure.
- **`fetch`** reads a web page out as text: headings, links, wrapped words,
  Swedish letters, redirects followed.
- **`network`** — a menu for the connection: automatic or by hand. Settings made
  by hand are saved to the floppy.
- **`crypto`** — checks the encryption code on the machine itself.
- Fix attempt for the black screen after a Shift boot then `screen big`: the
  switch always starts from a fresh screen mode, and the BIOS runs at its own
  timer speed while it works. `screen big` when already big redoes the switch.
- Switching screen sizes no longer leaks half a megabyte each time.
- Web pages are exempt from the line quirks, so they stay readable.

## 1.6.1 — 2026-11-09

- **Fixed the freezes** — `screen big`, the frozen boot screen, and `report`
  hanging. Every one was the same bug: video calls into the BIOS let a timer tick
  land where it stopped the clock for good. Fixed for every BIOS call, with a
  safety net that restarts a stopped clock.
- **Fixed the network card**: three separate faults kept the 3Com from ever
  switching on.
- **Automatic network setup (DHCP)** and **names (DNS)**: `ping example.com`
  just works on a LAN.
- 800×600 is the default again. Hold Shift while booting to stay at 320×200.
- Fixed `bye`, `reboot` and `whaaat` overwriting memory at 800×600.
- Commands typed quickly or over serial now show in the history.

## 1.6.0 — 2026-11-09

- Interim build of the 1.6.1 work, superseded the same day.

## 1.5.3 — 2026-11-09

- A crash now prints its details down the serial line before anything else.

## 1.5.2 — 2026-11-09

- Tools: writing the floppy takes about 16 seconds instead of 100. Only the part
  of the disk with something on it is written.

## 1.5.1 — 2026-11-09

- The test network card (NE2000) no longer mistakes an empty slot for a card.
- When no card comes up, every card's reason is reported.
- The big screen times itself and drops back to 320×200 if it's too slow.
- Tools: the floppy can be written without a password (one-time setup).

## 1.5.0 — 2026-11-09

- **`ping`**, and the machine answers pings itself.

## 1.4.2 — 2026-11-09

- 800×600: the panel stops stretching before the switch.
- A second network card driver (NE2000), for testing in an emulator.
- Tools: the floppy script says loudly when it has found a floppy.

## 1.4.1 — 2026-11-09

- 800×600 is no longer switched on at boot; 1.4.0 froze on the boot screen.

## 1.4.0 — 2026-11-09

- First driver for the 3Com network card (`net`).

## 1.3.3 — 2026-11-09

- **`report`** sends everything to the serial line in one go. The diagnostic
  commands are hidden from `help`.

## 1.3.2 — 2026-11-09

- `big` renamed to **`screen`** (`screen big`, `screen small`).

## 1.3.1 — 2026-11-09

- Boots straight into 800×600; `big` became the way back down.

## 1.3.0 — 2026-11-09

- **800×600** (`big`): the panel's own size, smoother double-size text.
- Fixed: any BIOS call made while the shell was running froze the keyboard and
  clock.
- `dump` shows any stretch of memory.

## 1.2.2 — 2026-11-09

- `vesa` lists what the video card can do.
- Fixed where the network card's configuration register is read from.

## 1.2.1 — 2026-11-09

- The banana tumbles on two axes.
- `card raw` shows the network card's own description, byte for byte.

## 1.2.0 — 2026-11-09

- Smaller text: an 80×33 face, and **`squint`** to switch faces.
- Frames and shading for text interfaces (`frames`).
- **`card`** finds the PC Card slots and asks the card what it is.

## 1.1.1 – 1.1.17 — 2026-11-06 to 2026-11-08

- The first bootable FLOPOS: the `here>` shell, the Swedish keyboard, the moving
  background, sound, taught words, the journal and saved state on the floppy.
- `whaaat`, line quirks, `bye` powers the laptop off, the jungle rhythm, and the
  banana in the corner.
