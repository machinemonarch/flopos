# Making apps

Programs don't have to be built into the system. An **app** is a file on
the floppy, `NAME.APP`, loaded into memory and run when you type its name.
`write` is the first.

Why apps live outside the system:

- The system stays small and boots just as fast however many programs
  there are.
- An app can be added, replaced or removed without touching the system.
- Apps get their own 4MB of memory instead of squeezing into the 640KB
  the system lives in.

## Running one

Type its name. When FLOPOS doesn't know a word as a command, it looks on
the floppy for a file of that name ending in `.APP`. `apps` lists them.
Anything after the name is passed to the app: `write 3`.

Apps need at least 14MB of memory, which the 430CDT has.

## How an app is built

Each folder under `apps/` is one app. `make` finds it, compiles every `.c`
file in it, and puts the result on the floppy:

    apps/
      app.h          what an app can ask of FLOPOS (the only header it shares)
      app.ld         where an app sits in memory
      crt.c          the app's header, and memset / memcpy / memmove
      write/
        write.c
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

Every app is linked to run at address 0x00A00000 (10MB) and may use up to
4MB from there. The file starts with a header, which FLOPOS checks before
running anything:

| Field | Meaning |
|---|---|
| `magic` | `FLOPAPP`, so a stray file is never run |
| `api_version` | Which version of the `Api` table it was built for |
| `load_addr` | Must be 0x00A00000 |
| `image_end`, `bss_end` | How much of the file is code and data, and how much zeroed memory follows |
| `entry` | Where `app_main` is |
| `name` | The app's name |

FLOPOS reads the file into place, zeroes the rest of the app's memory,
checks the header, and calls `app_main`. When it returns, you are back at
`here>`.

## What an app can ask for

`app_main` receives a table of functions. Its first field is the version,
currently 1. New functions are only ever added at the end, so an app built
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

Colours (`A_TEXT`, `A_DIM`, `A_BRIGHT`, `A_ACCENT`, `A_GREY`, `A_PAPER`,
`A_HILITE`, `A_SELECT`), frame pieces (`A_FR_*`) and the Swedish letters
(`A_CH_ARING` and so on) are named in `app.h`.

## Files an app wants to keep

Because apps can only write into files that already exist, an app's files
are made when the floppy is built, at full size. `tools/mkimage.py` does
this for `write`'s eight documents. A new app that keeps data adds its own
files there the same way, and adds them to the list `writefloppy.sh`
carries across upgrades.
