# browzer

FLOPOS's web browser, `BROWZER.APP` on the floppy.

    here> browzer
    here> browzer example.com
    here> browzer fetch example.com      the page as text in the terminal

![browzer searching DuckDuckGo Lite, in the emulator](../images/browzer-search.png)

## Keys

- **Up / Down:** Scroll a line.
- **PgUp / PgDn / Space:** Scroll a screen.
- **Tab / Shift-Tab:** Next / previous link.
- **Enter:** Follow the link.
- **Backspace or Left:** Back.
- **g:** Go somewhere. The address edits as in scribe: Shift with the arrows, Home or End picks; Ctrl-A picks it all; Ctrl with the arrows goes a word at a time; Ctrl-C, Ctrl-X and Ctrl-V copy, cut and paste.
- **/:** Find on the page, as in `less`; **n** next, **N** previous, Esc clears.
- **r:** Reload.
- **h:** Home: your bookmarks.
- **b:** Bookmark this page.
- **m:** Change bookmarks: **p** puts one first, **d** twice deletes.
- **j:** Allow this site's scripts, or stop them.
- **s:** Stop the music.
- **Esc:** Stop loading; on its own, asks to leave (**y**).

Words instead of an address search DuckDuckGo Lite. `squint` (outside
browzer) gives smaller text.

## Music and radio

Links to sound play through the Sound Blaster: mp3 and mp2, AAC and AAC+
(also in .m4a files), Ogg Vorbis, Opus, FLAC, wav, tracker songs (mod, xm, s3m, it), Shoutcast or
Icecast radio with their playlists, and stations that stream in short
pieces (HLS, `.m3u8`), live or not, sealed with AES-128 or in MP4
pieces, adverts spliced in included. A file under 2 MB plays while you
browse; radio plays on a player screen with the station and song, and Esc
stops it. An HLS station offering several versions starts on the
leanest; + steps to a better one and - back, which costs the laptop more
work and more network.

A page's streams are found even without scripts: "Listen live" buttons
play, and streams named only in the page's data are listed at the top.

Not yet: Opus's speech modes (a station of nothing but speech-coded Opus
is refused; music in Opus plays), and HLS sealed with SAMPLE-AES or in
parts of one long file (byte ranges). An .m4a file with its index at the end plays only if
it is under 2 MB.

![Радио Music Bg playing, its name and the song in Cyrillic](../images/browzer-radio-bulgaria.png)

## Forms

Boxes, tick boxes, choices, lists and buttons work. Enter on a box starts
typing and Enter again sends; Enter on a tick box, choice or list changes
it; Esc stops typing. Not yet: file uploads.

## Pages

- **Styles:** hidden stays hidden; bold, italic, underline, colour, centring and indents show. Anything pinned over the page (pop-ups, cookie walls) is never drawn.
- **Tables** are laid out in columns; one too wide is drawn cell by cell.
- **Images** become boxes with their description; frames become links.
- **Cookies** live in memory only, per site, until browzer closes.
- **Bookmarks** are in `BOOKMARK.TXT`, readable anywhere.

## Scripts

Off until you press **j** on a site. That is once per site: it is kept in
`SITES.TXT` and covers the site's pages, reloads and anywhere its scripts
send you on it; **j** again turns them off. Scripts can change the page,
react to clicks and forms, use timers, `async` and `await`, generators
(async ones too) and modules, and fetch from their own site, or from
another that allows it (answers up to 512 KB). jQuery works. A script can
be 256 KB, a page's scripts 640 KB; the biggest modern sites won't fit.

How they are kept safe:

- **The gate:** on an allowed site, scripts from other sites are fetched too (many sites keep theirs under a second name), but only over https and without your cookies there, so a tracker among them cannot follow you from site to site.
- **The room:** scripts run walled off in 4 MB of their own, with no network, files or cookies.
- **The door:** every request goes through browzer, which checks it, and a script that asks for something odd is stopped.
- **Forms:** a form goes wherever it says, as in other browsers; the key bar shows where before you send it. Only a form from an https page to a plain http address asks first (y sends it), since anyone on the way could read it.
- **Other sites:** a script may fetch from another site only as that site allows (CORS), without your cookies there, and never from a public page into your own network.
- **The clock:** two seconds a script while loading (up to fifteen for a big one), half a second for a click.
- **No `eval`.**

Secure connections: The network[^1].

[^1]: [The network: how private it is](network.md#how-private-it-is)
