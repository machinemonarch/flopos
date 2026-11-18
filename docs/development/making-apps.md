# Making apps

An **app** is a file on the floppy, `NAME.APP`, loaded and run when you
type its name. Apps keep the system small, can be added or replaced
without touching it, and get 8 MB of memory of their own. Anything after
the name is passed on: `scribe letter`. `apps` lists them.

## A new app

Each folder under `src/apps/` (but `sdk/` and `lib/`) is one app. Make
`src/apps/hello/hello.c` and run `make`; it becomes `HELLO.APP`. Give it
an entry in `data/help.txt` for `hello help`:

    #include "app.h"

    int app_main(const Api *api, int argc, char **argv)
    {
        api->print("Hello from the floppy.");
        api->newline();
        return 0;
    }

Apps are built like the system: 32-bit, 486 or better, no C library.
`src/apps/sdk/crt.c` gives `memset`, `memcpy` and `memmove`; the `Api`
table gives everything else. When `app_main` returns, you are back at
`here>`.

## What an app can ask for

Everything is in the `Api` table in `src/apps/sdk/app.h`, which documents
each call. New calls are only added at the end, and `APP_API_VERSION`
goes up, so older apps keep working. In short:

- **Windows:** `win_open`, `win_text`, `win_put`, `win_frame` and the rest, in character cells, with colours and bold, italic, underline. A program that draws its own title puts `os_version()` after its name.
- **The terminal:** `print`, `newline`, `putc`, `colour`.
- **Keys and time:** `key`, `pump` (keeps music and screen going while waiting), `ms`.
- **Files:** `inv_*` for the user's inventory (make, write, list, move, remove; `inv_room` and `inv_size` for how full the floppy is); `file_*` for the system's own files, written in place only.
- **The network:** `net_online`, then one connection at a time with `conn_open` (TLS and the certificate check included), `conn_read`, `conn_write`, `conn_close`, `conn_why`.
- **Sound:** `audio_open`, `audio_write`, `audio_feeder` (a refill routine the timer calls), `audio_listen` and `audio_heard`.
- **Sealing:** `crypt_key`, `crypt_seal`, `crypt_open`, `random`; `crypt_cbc` undoes AES-128-CBC (sealed radio).
- **Other:** `clock`, `glyph`, `wire` (to the serial line), `then` (run another app on leaving), `win_ember` (maelcum's colours), and the script room for browzer.

## The wall

An app runs in ring 3 behind paging. It can touch its own 8 MB at 12 MB
(`APP_BASE`) and nothing else, and asks for everything through the `Api`
table; every pointer it hands over is checked. An app that reaches
outside, uses a privileged instruction or divides by zero is closed with
one line saying why, also written in the journal. On a 486 there is no
wall. The file's header (`AppHeader` in `app.h`) is checked before
anything runs.

## Libraries

Shared code lives in `src/apps/lib/<name>/`, built into each app that
lists it in the Makefile, with what is generated in its `gen/`:

    APPLIBS_browzer := audio bits dsp

- **`audio`:** Tracker songs (mod, xm, s3m, it) and recordings (mp3, AAC, Ogg Vorbis, FLAC, wav) into 8-bit samples. Needs `bits` and `dsp`.
- **`bits`:** Bit reading and table-driven prefix codes.
- **`dsp`:** Fast rounding, the DCT and the inverse MDCT.
- **`tape`:** Files as cassette sound and back.

An app using floating point also needs `APPFLAGS_<name> := -m80387
-mhard-float` in the Makefile and an `fninit` at its start.

## Characters

Characters are ints, not bytes: FLOPOS's own codes below 0x100, Unicode
above. Keep them in `u16` or wider. In a string, a wide one is four bytes
starting with `A_CH_WIDE_ESC`; read with `a_ch_next()`, write with
`a_ch_put()`, convert with `a_unicode()` and `a_from_unicode()`. Put
Hebrew into a window in reading order; the system turns it.

## Keeping files

What the user makes goes in the inventory (`inv_*`) and is carried across
upgrades by itself. An app's own record (like browzer's bookmarks) can be
a file made at full size by `tools/build/mkimage.py`, written in place
with `file_write`, and added to `tools/floppy/keepfiles.sh`.
