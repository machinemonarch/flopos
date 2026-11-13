# Making apps

Programs don't have to be built into the system. An **app** is a file on
the floppy, `NAME.APP`, loaded into memory and run when you type its name.
There are two so far: `scribe`, the word processor, and `browzer`, the web
browser.

Why apps live outside the system:

- The system stays small and boots just as fast however many programs
  there are.
- An app can be added, replaced or removed without touching the system.
- Apps get their own 8MB of memory instead of squeezing into the 640KB
  the system lives in.

## Running one

Type its name. When FLOPOS doesn't know a word as a command, it looks on
the floppy for a file of that name ending in `.APP`. `apps` lists them.
Anything after the name is passed to the app: `scribe letter`.

Apps need at least 20MB of memory, which the 430CDT (32MB) has.

## How an app is built

Each folder under `apps/` is one app. `make` finds it, compiles every `.c`
file in it, and puts the result on the floppy:

    apps/
      app.h          what an app can ask of FLOPOS (the only header it shares)
      app.ld         where an app sits in memory
      crt.c          the app's header, and memset / memcpy / memmove
      scribe/
        scribe.c
        doc.c
        doc.h

To make a new app called `hello`, create `apps/hello/hello.c` and run
`make`. It becomes `HELLO.APP` on the floppy.

    #include "app.h"

    int app_main(const Api *api, int argc, char **argv)
    {
        api->print("Hello from the floppy.");
        api->newline();
        return 0;
    }

Apps are compiled with the same settings as the system: 32-bit, for a 486
or better, no floating point, and no C library. `crt.c` provides `memset`,
`memcpy` and `memmove`, and the `Api` table provides everything else.

## Where an app sits

Every app is linked to run at address 0x00C00000 (12MB) and may use up to
8MB from there. The file starts with a header, which FLOPOS checks before
running anything:

| Field | Meaning |
|---|---|
| `magic` | `FLOPAPP`, so a stray file is never run |
| `api_version` | Which version of the `Api` table it was built for |
| `load_addr` | Must be 0x00C00000 |
| `image_end`, `bss_end` | How much of the file is code and data, and how much zeroed memory follows |
| `entry` | Where `app_main` is |
| `name` | The app's name |

FLOPOS reads the file into place, zeroes the rest of the app's memory,
checks the header, and calls `app_main`. When it returns, you are back at
`here>`.

## The wall

An app runs with fewer rights than the system (ring 3, with paging). It
can read and write its own 8MB and nothing else: not the system's memory,
not the screen's, not the hardware's ports. Everything it wants done it
asks for through the `Api` table, exactly as before; each entry is now a
small stub that hands the request over the wall (`INT 0x80`), and FLOPOS
checks every pointer in it lies inside the app's own memory before using
it. Functions an app hands to FLOPOS (the sound feeder, the callbacks for
listings) are called back on the app's side of the wall.

An app that reaches outside its memory, uses an instruction only the
system may use, or divides by zero is closed with one line saying which,
and you are back at `here>`. The line is also written in the journal.

The top 4KB of the app's area belongs to the wall (the stubs and a few
strings), and the stack sits under it. On a CPU without 4MB pages (a 486)
there is no wall and apps are called directly, as before; `sys` says
which.

## What an app can ask for

`app_main` receives a table of functions. Its first field is the version,
currently 2. New functions are only ever added at the end, so an app built
for an older version keeps working.

**A window**, in character cells, drawn over the terminal:

| Function | What it does |
|---|---|
| `win_open(w, h)` | Open a window of that size (or the largest that fits) |
| `win_close()` | Close it |
| `win_width()`, `win_height()` | Its size |
| `win_put(x, y, ch, fg, bg, attr)` | One character, with colours and `AT_BOLD`, `AT_ITALIC`, `AT_UNDERLINE` |
| `win_text(x, y, text, fg, bg)` | A string; returns where it ended |
| `win_band(y, x0, x1, bg)` | Colour part of a row |
| `win_frame(title, fg)` | A frame around the window, with a title |
| `screen_cols()`, `screen_rows()` | How big the whole screen is in characters |

**The terminal**, for apps that only print: `print(text)`, `newline()`,
`colour(c)`.

**Keys and time**:

| Function | What it does |
|---|---|
| `key(&mods)` | The next key, or -1 if none is waiting. Letters come as themselves; special keys as `K_UP`, `K_ENTER`, `K_ESC` and so on; `mods` holds `M_SHIFT`, `M_CTRL`, `M_ALT`. |
| `pump(draw_now)` | Keeps the music, background and screen going. Call it while waiting for a key. |
| `ms()` | Milliseconds since boot. |

**Files on the floppy**:

| Function | What it does |
|---|---|
| `file_find(name, &f)` | Find a file by its DOS name (`WRITE1.MD`) |
| `file_read(&f, buf, max)` | Read it |
| `file_write(&f, buf)` | Write it **in place**: exactly as long as the file already is. Files can't grow, shrink, be made or be deleted. |
| `file_each(fn, ctx)` | Call `fn` for every file on the floppy |

**Text**: `format(out, cap, fmt, ...)`, a small `printf` into a buffer
supporting `%s`, `%u`, `%d`, `%x` and `%c`, with widths and zero padding like `%02u`.

### Added in version 2

| Function | What it does |
|---|---|
| `os_version()` | Which FLOPOS this is, as text |
| `putc(c)` | One character to the terminal |
| `plain(on)` | Stop the terminal's line quirks while an app prints something that has to stay readable |
| `wire(text)` | Send text down the serial cable, if there is one |

**The network.** The system does everything below the conversation itself,
so an app only reads and writes:

| Function | What it does |
|---|---|
| `net_online()` | Brings the network card up and finds an address if it has to. 0 if there is no network. |
| `conn_open(host, port, secure, note)` | Looks the host up, connects, and if `secure`, agrees on keys with TLS 1.3 (or 1.2, for a server with nothing newer) and makes the server prove who it is. `note` is told how it is going ("agreeing on a key..."). 0 on failure. |
| `conn_write(data, n)` | Send |
| `conn_read(buf, n, wait_ms)` | Receive: the number of bytes, 0 when the other end has finished, -1 when it went quiet for `wait_ms`, -2 when it broke |
| `conn_close()` | Hang up |
| `conn_why()` | After a failure, a whole sentence saying why ("It could not prove it is example.com: its certificate ran out on 2026-01-27.") |
| `conn_info(&ci)` | Whether it was secure, post-quantum, checked, who vouched for it, and the cipher |
| `conn_check(on)` | `conn_check(0)` lets the next secure connection through without the proof, for "go there anyway" |

One connection at a time. Anything above it is the app's own: browzer
speaks HTTP and reads HTML itself, in `apps/browzer/`.

### Added in version 3: sound

| Function | What it does |
|---|---|
| `audio_open(rate)` | Start the Sound Blaster playing 8-bit unsigned mono samples. Returns the rate it got (22050 asked for gives 22222), or 0 if there is no card that plays samples. The FM music steps aside. |
| `audio_write(p, n)` | Queue samples. Takes what fits and returns how many that was. The queue holds about six seconds. |
| `audio_space()` | How many samples would fit now |
| `audio_queued()` | How many are still waiting to be played |
| `audio_close()` | Stop, and let the FM music back |

### Added in version 4: refilling at interrupt time

| Function | What it does |
|---|---|
| `audio_feeder(fn)` | Hand the system a refill routine. The timer calls `fn` whenever the queue has room, a thousand times a second, whatever the app is doing meanwhile. 0 stops it, and so does `audio_close`. |

`fn` runs at interrupt time, with interrupts on and the FPU's state saved
around it. It may call `audio_space`, `audio_queued`, `audio_write` and `ms`,
and use its own memory, nothing else. It should come back within about
4 ms. Anything the app's own code shares with it must be changed under a
flag the routine checks: browzer's `s_lock`. See
[techniques.md](techniques.md).

### Added in version 5: listening

| Function | What it does |
|---|---|
| `audio_listen(rate, source)` | The card records instead of playing (never both). `source` is 1 for the line-in socket, 0 for the microphone. Returns the rate it got, or 0. |
| `audio_heard(buf, max)` | Up to `max` samples of what has come in, oldest first. About six seconds are kept. `audio_close` ends the listening. |

Reading or writing `JOURNAL.TXT` and `STATE.BIN` from an app is safe: the
system writes its own copy down before an app reads them, and reads them
back in after an app writes them.

### Added in version 6: the inventory, and handing over

A path is names with `/` between them, down from the inventory: `""` is
the inventory itself, `NOTES/A.MD` a file in a folder. Names are DOS
names (eight, a dot, three); no path can leave the inventory. A call that
fails returns 0 (or -1 where it returns a count) and `inv_why()` is a
sentence to show.

| Function | What it does |
|---|---|
| `inv_list(dir, fn, ctx)` | Calls `fn(item, ctx)` for each thing in a folder; returns how many. `fn` must not call the inventory. |
| `inv_stat(path, &item)` | Is it there, and what is it: name, folder or not, size, date |
| `inv_read(path, buf, max)` | A file's bytes; how many, or -1 |
| `inv_write(path, buf, n)` | Make the file, or replace it, at exactly `n` bytes |
| `inv_mkdir(path)` | A new folder |
| `inv_remove(path)` | A file, or an empty folder |
| `inv_move(from, to)` | Rename, move to another folder, or both |
| `inv_room()` | Bytes free on the floppy |
| `win_ember(on)` | Frames in red, gold and green instead of neon, and keys in yellow instead of pink, until the app leaves. Fixed colours to go with it: `A_CREAM`, `A_OLIVE`, `A_GOLD`, `A_EMBER`, `A_GREEN_D`, and `A_ON` (green), `A_RED`. |
| `then(command)` | Run another app when this one leaves, as if typed: `then("scribe NOTES/A.MD")`. How the inventory opens a document and `scribe` returns to it. |

## Libraries apps share

Code more than one app could use lives in `apps/lib/<name>/` and is built
into each app that asks for it in the Makefile:

    APPLIBS_browzer := audio bits dsp

| Library | What it does |
|---|---|
| `audio` | `tracker.c`: .mod, .xm, .s3m and .it songs into samples (`tracker_load`, which changes the file where it lies, so a file is loaded once, then `tracker_rate` and `tracker_render`). `decode.c`, `mp3.c`, `aac.c`, `vorbis.c`, `flac.c`: recordings (mp3, AAC, Ogg Vorbis, FLAC, wav) through one interface (`audio_detect`, then the decoder's `decode`), from a whole file or a stream as it arrives, and a noise-shaped resampler to the card's 8 bits. Needs `bits` and `dsp`. |
| `bits` | `bits.h`: reading 1 to 25 bits at once, high-first or low-first. `prefix.h`: Huffman and other prefix codes decoded by table (`prefix_build`, `prefix_get`, `prefix_lget`). For anything compressed: audio now, and JPEG, gzip and PNG would use the same. |
| `dsp` | `dsp.h`: `dsp_round` (float to int without the x87's slow cast), `dsp_dct2` (the cosine transform of 2 to 64 numbers, fast). `mdct.h`: the inverse MDCT by FFT, its tables kept per size. |
| `tape` | `tape.h`: files as cassette sound and back (`tape_tx_*`, `tape_rx_*`), whole numbers only. Used by `tape`. |

An app that uses floating point (the audio library does) also needs the
Pentium's FPU switched on for it in the Makefile (`APPFLAGS_browzer :=
-m80387 -mhard-float`) and an `fninit` when it starts. Nothing in the system
uses the FPU, so an app has it to itself.

Colours (`A_TEXT`, `A_DIM`, `A_BRIGHT`, `A_ACCENT`, `A_GREY`, `A_PAPER`,
`A_HILITE`, `A_SELECT`), frame pieces (`A_FR_*`) and the Swedish letters
(`A_CH_ARING` and so on) are named in `app.h`.

## Files an app wants to keep

What the user makes goes in the inventory, through the `inv_` calls below:
real files of any size, under names the user chooses, carried across
upgrades by `writefloppy.sh` without anything more to do. That is where
`scribe` keeps its documents.

An app's own records (settings, a list like browzer's bookmarks) can
instead be a file made at full size when the floppy is built
(`tools/mkimage.py`), written in place with `file_write`, and added to the
list in `tools/keepfiles.sh`.
