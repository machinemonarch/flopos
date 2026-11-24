# FLOPOS changelog

Every release gets an entry here, and `make release` will not build one
without it. Each version's notes are also written next to its image, as
`releases/flopos-X.Y.Z.txt`.

## Unreleased

### FEATURES

None in this release.

### BUG FIXES

#### Secure connections

- **A secure site that changes its key mid-download stays connected.**
  TLS 1.3 lets a server move to a fresh key at any time, and ask the
  client to do the same. FLOPOS didn't follow, so the next piece failed
  its seal and the page or stream stopped. It now follows, both ways.
- **A site that asks for your own certificate says so plainly.** Some
  sites want the visitor to prove who they are with a certificate.
  FLOPOS has none, so it now says that, and gets in where it's optional.
  Where it isn't, the page gives the error code and what it means in
  plain words, instead of "a handshake message out of turn".

#### browzer

- **A redirect to a relative address goes to the right place.** A server
  may answer "moved to next/page", without the site's name. browzer took
  that for a site called "next", and the page never came.
- **Addresses with spaces or letters like å in them work.** browzer sent
  them as written, which servers refuse; they now go as %20 and %C3%A5,
  as other browsers send them.
- **A bookmark that didn't fit on the floppy is no longer called added.**
  When BOOKMARK.TXT was full, the new one was kept only until browzer
  closed, while the message said it was saved. It now says the file is
  full.
- **Esc stops browzer asking a radio directory for its stations.** A slow
  directory could keep browzer waiting with no way out.

### SECURITY FIXES

#### Secure connections

- **Certificate dates are checked in UTC.** The laptop's clock keeps
  Swedish time and certificates are dated in UTC, so a certificate that
  had run out up to two hours ago was still believed.
- **A certificate's key can no longer stall the machine.** An RSA key
  may carry any exponent, and one in a certificate not yet checked was
  used as given; a huge one made a single check take minutes. Exponents
  over 32 bits, even or below 3 are refused, as browsers do.
- **A TLS 1.3 server is held to the rules of its hello.** It must hand
  back our session number, ask for no compression, keep the cipher it
  named when it asked us to say hello again, answer only with a key
  share we offered, and send its messages in the order the standard
  gives. A shared secret of all zeros is refused, and so is a
  ChangeCipherSpec after the handshake.
- **A certificate must be allowed to do what it is used for.** A site's
  certificate that may not sign can't prove who the site is, and an
  authority meant for something other than websites can't vouch for one.
  Both are refused now.
- **The older key exchange no longer gives away its key through timing.**
  With servers that lack X25519, FLOPOS's secret for the P-256 or P-384
  exchange shaped how long the sums took. They now take the same time
  whatever the secret is.

#### browzer

- **A drop-down list in a narrow table no longer writes past the page.**
  A page with lists in table cells narrower than six letters made
  browzer fill hundreds of KB of its memory beyond the page, which could
  crash it or worse.
- **A page that ends inside a table no longer breaks the next one.**
  The next page was drawn into a table cell's small scratch space as if
  it were the whole page, and ran past its end.
- **A big form can no longer write past the memory it is sent from.**
  A form whose values grow when sent (each line end is six characters
  on the way) could overflow its buffer by a few bytes per field.
- **A link can no longer add lines to browzer's request.** A line end in
  an address went into the request as it was, so a page could make
  browzer send another site headers of its choosing. Line ends are now
  dropped and other odd characters encoded, and a site name that is too
  long or holds them is refused instead of cut short.
- **A style sheet can no longer freeze browzer.** A selector such as
  "x ~ a ~ a ~ a ~ a" over a long list made browzer try every way of
  matching it, which takes forever. Matching now gives up after a fixed
  number of tries per element and per page.
- **What a station or file says is shown as words, never as part of the
  page.** A station's name, its song titles, a file's type or a
  certificate's complaint were put into browzer's own pages as written,
  so they could add links, forms or a redirect there.
- **A plain http page can no longer set or replace a Secure cookie.**
  Secure cookies are for https sites only, as the cookie standard says.
- **Smaller fixes.** A style sheet with a comment left open read one byte
  past its end; a header line shorter than nine letters was read a few
  bytes past its end; a letter outside the first 65536 in an attribute
  (&#x1F600;) came out as three wrong bytes.

## 1.48.0 — 2026-11-24

### FEATURES

#### Network

- **A busy line is shared fairly.** Sending starts with a few pieces and
  grows while nothing is lost, backs off when something is, resends a
  single lost piece at once, and the resend timer is measured from real
  answers instead of guessed.

#### Writing a floppy

- **`--no-verify` skips the read-back,** about 50 seconds saved, when
  checking the disk doesn't matter.

### SECURITY FIXES

#### Network

- **A web server could make FLOPOS read memory it had no business
  reading.** One piece of a download, given a carefully wrong number,
  slipped past the checks, and FLOPOS copied up to 64 KB from far
  outside the piece into the page. On the laptop nothing lives there, so
  it was junk rather than secrets, but it is now refused.
- **A name's answer must repeat the question.** Before, any answer with
  the right number was taken, whatever name it was about. Now the name
  has to match too, as the rules for name servers ask, so a faked
  answer is harder to slip in.
- **The time server's answer must come from its own port,** not just its
  address.
- **Pings sent to everybody on the network are no longer answered,** so
  FLOPOS can't be used to multiply someone else's traffic.
- **Pieces of a split-up packet are dropped** instead of being read as
  if whole.
- **No falling back from post-quantum.** When a site chose the
  post-quantum handshake and it broke halfway, FLOPOS used to try again
  without it. Someone in between could break it on purpose to get the
  older kind. Now it stops and says so.
- **Neighbours on the network are learned only from answers to our own
  questions,** not from anything they send, and asked again every five
  minutes. Before, the first claim was kept for good. A stranger
  answering for the router is only noted: the router is replaced only if
  it has stopped answering.
- **Damaged packets are thrown away:** the checksum on every IP header
  and every ping is now checked. And the address handed out by DHCP is
  taken only from the server whose offer was accepted.
- **No stray byte goes out with a frame.** Both card drivers read one
  byte past the end of a frame of odd length when handing it to the
  card. Now the last byte is sent on its own.

#### Kernel

- **A stack overrun stops the program.** The kernel, the apps and the
  script room keep a secret value beside each function's return address;
  if it has changed on the way out, the program is closed instead of
  jumping wherever the overrun points.
- **Only memory that is really there can be reached** (and the screen's).
  A stray pointer now stops at once instead of reading junk.
- **An app's own code is read-only while it runs,** so a bug cannot be
  turned into new instructions.

#### browzer

- **Requests from the script room must have exactly the expected shape,**
  or they are refused.

### BUG FIXES

#### Network

- **A name too long to ask for** is refused, instead of FLOPOS quietly
  looking up a shortened name.
- **With no way out set,** packets for other networks are no longer sent
  to a made-up card address.
- **Uploads no longer stall** when a server says it has no room for
  more: FLOPOS asks now and then whether it has room again, instead of
  waiting for a message that might have been lost.
- **The network no longer stops after 49 days** switched on: a wait that
  spanned the clock's wrap-around lasted another 49 days.
- **In QEMU, the network no longer goes deaf for good** after a big
  download is cut short. The emulated card writes over a frame when its
  memory fills exactly; FLOPOS now sees the damage and starts afresh. (The
  laptop's own card was never affected.)
- **A resent piece no longer fools the resend timer.** After a resend,
  the time to the answer was measured from the resend, so the timer
  could shrink to almost nothing and FLOPOS resent pieces that were
  only slow.
- **An old acknowledgement no longer changes how much the server will
  take.** One arriving late, out of turn, could shut the window until
  the next ask.
- **Waits for the network card are timed by the clock,** not counted in
  turns of a loop, so they last as long on any machine.

#### Kernel

- **The serial line's key wait no longer hangs after 49 days** switched
  on, the same clock wrap as the network's.

#### Writing a floppy

- **A drive plugged in empty no longer asks for a password,** once the
  rule from the guide is in: it now knows the drive itself, not the disk.
- **A drive it could not open is no longer called 0 bytes.** The size is
  read without opening it, and the write asks for a password only if it
  must.

## 1.47.0 — 2026-11-23

### FEATURES

#### browzer

- **Editing the address (g)** works as in scribe: Shift with the arrows
  selects, Home and End jump, Ctrl with the arrows moves a word at a
  time, and Ctrl-C, Ctrl-X and Ctrl-V copy, cut and paste.
- **Radio in several qualities (HLS)** still starts on the leanest. On
  the player screen, + and - now step up or down.
- **Scripts from a site's other domains** load on allowed sites. Many
  sites keep their scripts under a second name (bandcamp.com's are on
  bcbits.com). Only over https, and without cookies, so a tracker among
  them can't follow you from site to site. Before, only four library
  hosts were let through.
- **Forms** go wherever they say, as in other browsers, also when a
  script points one at another site. One case asks first (y sends it):
  an https page sending a form unencrypted, to a plain http address.
- **Bigger scripts:** up to 256 KB each (was 100) and 640 KB a page (was
  250). They go to the script room in pieces as they arrive, so browzer
  never holds one whole.
- **fetch() to other sites** works when that site allows it (CORS), as in
  other browsers. Before, the script was stopped.
- **browzer names itself** to every server, with a link, as crawlers do:
  "browzer/1.46.0 (FLOPOS; Running on a 1996 Toshiba Laptop;
  +https://github.com/machinemonarch/flopos)".

- **Big pages run their scripts:** a page may have 32,768 parts (was
  6,144), so news sites such as BBC News no longer skip them.

#### Script room (JavaScript)

- **librespeed.org's speed test runs**, tried on the laptop.
- **Newer JavaScript runs**, where before the whole script was refused:
  - class fields (private ones too) and static blocks
  - modules: import and export, import maps, import() and import.meta;
    scripts marked for old browsers are then skipped, as elsewhere
  - modules from data: addresses, and import.meta.resolve (the check
    radio.garden makes)
  - generators, async generators and for await
  - taking lists and objects apart: ...rest, defaults, into properties
  - tagged templates, new X(...list), await at a module's top level
  - document.currentScript, which packed scripts use to find their
    other files
- **Web Workers:** a page's background script runs beside it, with
  postMessage both ways, as speed tests and many apps use. A Worker's
  work may take a minute; other timers four seconds.
- **Typed arrays:** ArrayBuffer, Uint8Array to Float64Array, DataView,
  TextEncoder and TextDecoder, built into the engine.
- **Blob and File**, and fetch() and XMLHttpRequest send and receive
  bytes: arrayBuffer(), blob(), and responseType "arraybuffer" and "blob".
- **fetch() answers up to 512 KB** come whole, passed on in pieces as
  they arrive, with their headers. Before, they were cut at 64 KB.
- **XMLHttpRequest streams:** a download is read as it arrives, with
  progress, and can be stopped part way; an upload of any size goes out
  in pieces, with upload progress. Every event, many listeners each.
- **URL** follows the standard: ../ and ./ are worked out, an address
  that is not one throws, every part can be read and set and the address
  follows, and searchParams stays linked. URL.canParse and URL.parse.
- **Headers, Request and Response** are complete: Request can be made
  and handed to fetch(), Response.json() and Response.error() exist.
- **Proxy and Reflect**, which frameworks such as Vue need.
- **Observers:** MutationObserver reports changes; Resize- and
  IntersectionObserver give their first measure.
- **Intl:** numbers, currencies, dates, plurals, "3 days ago" and lists
  (English and common European ways of writing them).
- **Map and Set are built in**, as fast as other browsers' and as the
  standard says: WeakMap, WeakSet and groupBy too.
- **Smaller pieces:** Array.from takes anything list-like; arrays and
  strings carry their iterator; functions have their own name and
  length; new Error(message, {cause}), URIError and AggregateError.
- **Fetches are rate-limited, not capped:** 60 at once, then six a
  second, so a long-running page carries on and a runaway one is held.
- **Problems say so:** an error no script caught in a timer shows on the
  serial wire, and a timer stopped for time or memory names its function.
- **Reading a big script** takes a quarter of the memory it did.
- **The result:** of 568 scripts from 38 well-known sites, 34 are still
  refused, down from 176. Most of those left are too big.

#### Kernel

- **Apps get 12 MB** (was 8), and **the script room 7 MB** (was 4), at
  24-31 MB, in memory nothing used: a page's scripts get about 5 MB.
- **Programs are packed on the floppy** and unpacked as they load: the
  script room and every app take about half the space (300 KB freed) and
  less of the floppy's slow reading. Your own files stay plain.
- **The kernel, Ozzy's picture and the list of trusted authorities are
  packed too,** 167 KB more free on the floppy (it was 82% full, now 71%),
  and 131 KB less to read off the drive at boot. Saffran stays as he is:
  the boot sector shows him before anything can unpack.

### BUG FIXES

#### browzer

- **Wikipedia articles hung** after connecting. Their headers are 9 KB
  and browzer kept 8 KB, so it never saw where they end. A header line
  too long to keep is now left out, and the end is still found. Scripts
  and fetch() had the same bug at 2 KB.
- **Long stream addresses** play: signed ones (RTL's) run past a thousand
  characters, and browzer cut them at 200.
- **A missing station playlist** shows the station's own error page, not
  "Not text, so not shown".
- **Radio in pieces (HLS):** the status line stays put, instead of saying
  "agreeing on a key" for every piece.
- **j on a page without scripts** says so, instead of loading the page
  again.

#### Script room (JavaScript)

- **A Worker or import() started from a click or a timer** was never
  loaded; now it is.
- **Pages with many downloads at once ran the scripts out of memory:**
  all XMLHttpRequests together now keep at most 768 KB of answers, and a
  finished Worker is let go.
- **An error inside a generator** stopped the whole script room.
- **A plain for-of over promises** went round for ever; it now stops with
  an error.

#### maelcum

- **A damaged spot on an encrypted tape** no longer makes maelcum say
  your passphrase is wrong. Every piece of the backup is labelled twice
  on the tape; when one label is damaged, maelcum now reads the other
  and carries on.

#### Clock

- **The clock menu** wraps its longer lines instead of cutting them off.

#### Sound

- **The serial wire** no longer says "decoding took 0.0%" every ten
  seconds: twice a minute at most, and only when there is something to say.

### SECURITY FIXES

#### browzer

- **A hostile script could make browzer read past a buffer:** a request
  from the script room with a false, huge length slipped past the check
  (the sum wrapped round). The check now cannot wrap. Found by fuzzing.
- **A long list of cookies overflowed a buffer by one byte** when sent.
  A cookie now goes in whole or not at all. Found by fuzzing.
- **fetch() to another site** sends no cookies there, keeps none it is
  given, says where it comes from, and asks that site first when the
  request is not a plain one.
- **No public page's script can reach your own network** (a router's
  pages, say).

#### Script room (JavaScript)

- **A page's script could feed the script engine bad data** that it took
  on trust (through Proxy, a newer JavaScript feature). It now checks.

## 1.46.0 — 2026-11-18

### FEATURES

- `clock` opens a menu: the time, where it came from, the signed time
  servers, and how far to trust the network beyond them.
- browzer plays stations that stream in short pieces (HLS, `.m3u8`),
  live or not, AAC or mp3, sealed with AES-128 or not, in transport
  packets or MP4 pieces, as many big broadcasters do.
- The inventory shows how full the floppy is: a bar, the share used, and
  how many KB of how many.
- browzer plays mp2 (MPEG audio layer II), which some broadcasters still
  send, AAC in .m4a files, as podcasts come, and Opus, as more and more
  stations send. Opus's speech modes are not played yet.
- HLS stations that splice in adverts of another kind or rate play on
  through them.
- browzer's keys at the bottom show j when a page has scripts that have
  not run, and say what it does.
- The system now runs above the first megabyte: it had six kilobytes
  left to grow in, and now has hundreds.

### BUG FIXES

- None yet.

### SECURITY FIXES

- The clock takes its time from the network signed (NTS) first, so
  nobody on the network can wind it back to make an expired certificate
  look good. By default the only unsigned time it will take instead is
  from the time server your own network names.

## 1.45.0 — 2026-11-18

### FEATURES

- The HUD rolls down from the top of the screen, the whole width of it:
  the command line on the left, with room for long lines, and the
  meters on the right. With `gibson on` it chimes as it opens and closes.
- The HUD's sound bars span 32 Hz to 10.5 kHz in ten bands, thirty times
  a second, and the lowest ones now tell their notes apart.
- Ctrl-C stops the wizard in the middle of his line.
- The wizard's riddle takes long answers, carried onto the rows below.
- browzer's frames turn through cyan, teal and blue, with a glint of
  silver.
- `gibson`'s key sound is new: a fast phone-tone warble ending in a low
  thump, as loud as the HUD's chimes.
- While maelcum is open the background music is silent, and while it
  writes or reads a tape the key sounds are too. Apps can ask for that
  quiet (`hush` in the app interface).

### BUG FIXES

- `gibson`'s sound comes the moment a key is pressed, not most of a
  second later, and each key cuts the last one's sound off instead of
  piling on top of it.
- `gibson off` (or `on`) while browzer plays music no longer stops the
  music for good.
- `gibson on` leaves the background music playing: the key sounds and
  the music play together.

### SECURITY FIXES

- None in this release.

## 1.44.0 — 2026-11-16

### FEATURES

- browzer reads pages' styles: what a site hides stays hidden, with bold,
  italic, underline, colour, centring and indenting. Anything a page pins
  over its content (pop-ups, cookie walls, newsletter boxes) is never drawn.
- browzer runs JavaScript on sites you allow with j, in a walled room of its
  own. Scripts are off everywhere else: "3 scripts, none of them run."
- Scripts can use `async` and `await`, and the browser pieces pages
  often stop on (AbortController, FormData, TextEncoder and more).
- On an allowed site, libraries from jsDelivr, cdnjs, Google's and unpkg
  are fetched too, so pages built on jQuery work. Reading jQuery takes
  about three seconds on the laptop.
- browzer lays out tables as tables, in columns.
- browzer finds on the page as `less` does: `/`, the words, Enter; `n`
  and `N` for the next and the one before.
- A radio page's "Listen live" button plays, with or without scripts. Streams
  a page names only in its data are listed at the top, by station name.
- A "Listen live" or "Play" with no link, on a page that names a stream,
  plays it.
- Sound files are asked for from their first byte, as browsers' players
  ask, so servers that send recordings only that way play them.
- browzer's keys are yellow. Each bookmark is one link: its name, with
  its address under it.
- Secure sites that use only the P-384 curve work, as they do in Chrome and
  Firefox: TLS 1.3 servers that ask for a second hello, and TLS 1.2 ones.
- TLS 1.2 also speaks the two older ciphers Chrome still offers (AES in
  CBC mode with SHA-1), last, for servers with nothing newer.
- Smooth, shaded text on the big screen: 100 columns by 50 rows, drawn at
  the screen's own pixels. `squint` also gives the large letters, the close
  ones, and a fine size of 133 columns.
- The moving background is drawn at 800x600 itself and dithered: no
  blocks, no bands, and it drifts a pixel at a time.
- The big screen's letters are a little sharper: each is fitted to the
  pixel grid, with fewer grey half-tones around its strokes.
- `help` after any word or program says a little about it: `ping help`,
  `browzer help`. The text is `HELP.TXT` on the floppy, readable on any
  computer.
- `gibson on` makes every key sound a thumpy pair of phone tones, like a
  film's mainframe (a recorded hit, mixed into whatever plays); `gibson
  off` quiets them.
- In `network`, Tab goes round every setting and button.
- Ctrl-C at the prompt drops what you were typing.
- Ctrl-Shift-F5 switches the screen again, from anywhere.
- `tools/floppy/writefloppy.sh --dev` writes your own build instead of
  the newest release.
- `make wire` talks to the laptop over the serial line and adds every
  session to one log, `logs/serial.log`.
- The project's files are arranged by kind: the code in `src/`, what
  builds and writes it in `tools/`, the tests in `test/`, and what a tool
  made in a `gen/` folder beside the code it belongs to. What is built
  goes in `build/`; `dist/` holds only the finished floppy image.
- The documentation is less than half as long: how-tos kept, history
  and explanations the code already gives taken out.

### BUG FIXES

- The black screen at the start: after the switch to 800x600 the panel's
  own power and output settings are put back as they were while it was
  lit, and looked at again a moment later.
- browzer shows a radio station's name and the song it is playing in
  Cyrillic, Hebrew or accented letters, instead of garbled signs.
- On the big screen D no longer looks like O: letters with a straight left
  side keep their square corners.
- browzer keeps the space a page puts around a word with a margin
  ("Hacker News new", not "Hacker Newsnew"), and a link no longer splits
  a word at its edges.
- Links in headings look like links.
- In the emulator, the network card recovers when more arrives than it
  can hold, instead of going quiet.

### SECURITY FIXES

- None in this release.

## 1.43.2 — 2026-11-14

- The AAC decoder's tables come from the VisualOn AAC encoder, under the
  Apache License 2.0: its licence text and notice are now in `licenses/`,
  with credits for the MP3 tables and Mozilla's list of trusted
  authorities.

- `tools/floppy/writefloppy.sh` says before writing that your files on the floppy
  are carried over, and that `--fresh` gives a clean disk. Before, it said
  everything on the disk would be gone, and then kept your files.

- The README on the floppy is the published one: the opening in Max's
  words, screenshots, and how to put a release on a floppy.

## 1.43.1 — 2026-11-14

- `version` says "Cyberdelic Neurofunkadelic Dreamscape", "Directed by
  Max and Synthesized with Claude.", and the day it was built.

## 1.43.0 — 2026-11-14

- Ozzy, the other cat, sees the machine off: `bye` and `reboot` show his
  picture while the steps of shutting down are typed over him, the way
  Saffran's boot lines are.

## 1.42.1 — 2026-11-14

- browzer searches through DuckDuckGo Lite when words are typed in the
  address line: plain pages of results, over an encrypted connection.
  FrogFind, which it used before, no longer answers.

- browzer could close with "it reached outside its own memory" on a page
  with a very long word (1.42.0 only). Fixed.

- The documentation was gone through from top to bottom: the README now
  says what FLOPOS is, who makes it and everything it can do, and takes in
  the old philosophy page and the index; long tables became lists; links
  in the middle of a sentence became numbered notes at the bottom of each
  page; the user guide has screenshots of scribe, browzer and maelcum;
  maelcum's page carries a warning that cassettes are not for important
  data.

- Saffran's boot picture is now under the MIT License, like everything
  else.

## 1.42.0 — 2026-11-14

- `keyboard`: FLOPOS now knows seventeen keyboards: Swedish (still the
  default), Finnish, Norwegian, Danish, American, British, German,
  French, Belgian, Spanish, Italian, Dutch, Romanian, Bulgarian,
  Ukrainian, Hebrew and Yiddish. `keyboard de` changes it; `keyboard`
  alone says which is in use. The choice is kept after a restart; a new
  release on the floppy starts on Swedish again.

- Ctrl+Shift+F1, on any keyboard and in any app, opens a window listing
  every keyboard: the arrows pick, Enter chooses, Esc leaves it be. For
  when the keyboard chosen makes `keyboard se` impossible to type.

- The Bulgarian, Ukrainian, Hebrew and Yiddish keyboards type Latin
  letters too: Alt and Shift together switch between the two. Ctrl
  shortcuts always go by the Latin letter on the key.

- The accent keys work: ´ then e is é, ¨ then u is ü, ^ then o is ô, on
  every keyboard that has them, Swedish included.

- Hebrew and Yiddish are drawn right to left, everywhere text is shown:
  the shell, scribe, browzer, the HUD. Numbers and brackets inside them
  come out the right way round.

- The font has the letters all these keyboards type, in both text sizes:
  æ ø ü ß é è ê à ç ñ ă ș ț and the rest with their capitals, £ € § ° µ
  ¡ ¿, the Cyrillic alphabet as Bulgarian and Ukrainian use it, and the
  Hebrew alphabet with Yiddish's pointed letters. scribe saves and opens
  them as UTF-8; browzer shows them on pages (Hebrew station names, which were "(a name
  in Hebrew)" before) and types them into boxes.

- Under the hood, a character is no longer one byte: the screen, windows,
  the HUD, the command line, scribe's documents and browzer's pages hold
  two. Apps rebuild with the new `app.h`; special keys (`K_UP` and the
  rest) have new numbers.

## 1.41.2 — 2026-11-14

- maelcum: the tape sleeve's two cards touch on the page, so they are cut
  out as one piece and the inside is folded back behind the outside. The
  inside is printed upside down for that, and shows its folds as dashed
  lines like the outside.

- maelcum: on the screen for ticking what to write, the key line was too
  long and Esc was cut off; it fits again. The R line on the start screen
  is no longer cut short, and a long message beside the cassette wraps
  onto the next line instead of running into the frame.

## 1.41.1 — 2026-11-14

- maelcum: once a header from another save on the same tape is heard,
  its pieces are ignored until a header of the save being read comes
  again. Before, a piece of an older save could be taken into a file of
  the new one; the file's check then failed and the tape had to be
  played again. Tapes are written as before.

## 1.41.0 — 2026-11-14

- maelcum: the file list is empty until there is something to do. **W**
  shows the floppy's files to tick (W again writes them); **R** shows the
  cassette's files as they are heard, to tick and restore. Its title says
  which.
- maelcum: a read is restored by default into a folder of its own, named
  by the day the reading ended (`14112026`, DDMMYYYY), with everything
  that came whole ticked. **D** still offers where each came from, the
  top, any folder, or a folder you name.
- maelcum: the tape sleeve is one `SLEEVE.PDF`: one A4 page with the
  outside above the inside, each at its true size, printed on one side;
  the screen says where it will go. Its pictures are packed by run
  length, so it is about 23 KB and goes on the tape with the next backup.
  Replaces `SLEEVE.BMP` and `INSIDE.BMP`.
- maelcum: reading no longer cries "Sorry! Sorry!" over blocks one ear
  lost and another read. A failure is now counted only when every ear
  that was in the block has given it up (tools/tapetest checks that a
  tape read whole counts no losses). The verdict judges every file of the
  save, not only those heard, and the screen is drawn before the tune.
- maelcum: a glint sweeps across the title, and the verdict after a read
  shines the same way.
- maelcum: the bottom line no longer repeats keys shown above (W, R, G,
  D); Run 'Tingz starts on the line under its heading, like the others.
- maelcum: no smiley after "SELECTA!".
- scribe: Ctrl-A, Ctrl-C, Ctrl-X, Ctrl-V, Ctrl-Z, Ctrl-Y, Ctrl with the
  arrows, Home, End, Backspace and Delete, as on any computer.
- `version` says "Cyberdelic Neurodivergent dream".

## 1.40.0 — 2026-11-13

- **maelcum: `G` generates a tape sleeve**, the paper that goes in the
  cassette's box. Two pictures are saved on the floppy, in the folder
  restored into (or the top of the inventory): `SLEEVE.BMP`, the outside,
  with the date and time the write or read finished (DD-MM-YYYY HH:MM),
  what was said of the tape in quotes, and how much is on it; and
  `INSIDE.BMP`, the list of files, for the back of the same sheet. Each
  is exactly a standard sleeve's size, 101.6 mm square laid flat, when
  printed at 100% on A4: cut along the outer line and fold along the
  dashed ones. Black on white, 45 KB each. An encrypted save's names are
  not printed.
- For programs: `clock` (the date and time) and `glyph` (a letter's
  shape). Both only read.

## 1.39.0 — 2026-11-13

- The version after a program's name is drawn quietly: grey, or dim gold
  in maelcum.
- maelcum:
  - Named "Magnetic Rust: Riddim 'n' Vibrationz Engine" at the top, in
    red, gold and green that run through the letters. It shines by the
    palette turning, as the frames do: nothing is drawn again, so it
    costs no speed.
  - The settings have room: in pairs, with a blank line between pairs
    and between sections. The cassette is a little smaller and sits
    with `W` and `R` in a section of its own, "Run 'Tingz".
  - **Reading is restoring.** `R` reads the whole save into memory and
    then shows what the tape holds, marked new, different or same. Tick
    what you want and `Enter` puts it on the floppy. The new files start
    ticked; a different one must be ticked on purpose, since it replaces
    the floppy's copy. `P` is gone.
  - Reading takes everything on the tape, whatever is ticked on the
    floppy's side.

## 1.38.0 — 2026-11-13

- **Text never runs over a window's frame**, in any program: what does
  not fit is left out, and maelcum's long lines were shortened or broken.
  A program's name still sits in the top edge.
- **Every program shows the FLOPOS version after its name**, top left.
- maelcum:
  - Titled "Data Tape Backup & Restoration Software".
  - Settings in two groups, **Write Options** and **Read Options**. The
    value not in force is grey; dim gold is kept for the tape legend.
  - The file list works like `inventory`: one folder at a time, `Enter`
    goes in, `Backspace` comes out.
  - `D` (Restore into) opens a list: where each file came from, the top,
    any folder, or a new folder. A new folder is **made at once**, so
    `inventory` shows it. (Before, it was made only when files were put
    back, and never if the tape read badly.) `N` is gone.
  - Reading is about restoring: it says how many files are new, different
    or the same, and `P` puts the new and the different ones on the
    floppy. A tape from another floppy restores just as well.
  - **Fast speed reads better from a dull recorder**: a fast tape is also
    listened to with its treble lifted.
  - A block counts as spoiled only when none of the listeners has it. One
    losing a block another had was counted, which could say "Sorry!
    Sorry!" over a tape that was reading.
  - A tape that reads badly now ends with a short sad tune; before, it
    ended in silence.

## 1.37.0 — 2026-11-13

- **Encrypted backups in maelcum**, off by default (`E`). A passphrase,
  typed twice when writing and once when reading, seals each file's name
  and contents. A wrong passphrase or an altered tape is refused whole.
  The passphrase is kept nowhere.
- A list of three ciphers (`K`): ChaCha20-Poly1305, AES-256-GCM (new) and
  AES-128-GCM, each labelled with its key size and whether it holds
  against quantum computers.
- A new folder to restore into (`N`).
- `crypto` checks AES-256-GCM and PBKDF2 against results from the PC.
- For programs: `crypt_key`, `crypt_seal`, `crypt_open`, `random`.
- The build now rebuilds every part of the system when a header changes;
  a stale part had made a program crash in testing (the wall closed it).

## 1.36.1 — 2026-11-13

- **maelcum in red, gold and green.** No grey: text is cream and gold,
  quiet text and the legend a dim gold, the frame runs red to gold to
  green. Keys stay yellow.
- **Verify no longer sounds finished too early.** With Verify on, the
  tune waits until the check has passed, and the screen says the buttons
  to press are the recorder's: "press STOP, rewind, then press PLAY."
- **No false alarm when stopped by hand.** Stopping a read with S no
  longer brings a poor verdict unless pieces were spoiled.
- The tune's last notes were cut off; it now plays to the end.
- Playback starts with "Now hear 'dis!"; "Buckle up!" is for recording.

## 1.36.0 — 2026-11-13

- **`tape` is now `maelcum`**, after the pilot in William Gibson's
  *Neuromancer*: at the prompt (`maelcum`, `maelcum save`, `maelcum
  load`) and on the floppy (`MAELCUM.APP`). `tape` is no longer a command.
- maelcum's colours: red and orange where teal and cyan were, keys in
  yellow instead of pink (in this program only), and a darker neon green
  for what is on.
- For programs: `win_cool` became `win_ember`.

## 1.35.0 — 2026-11-13

- **`write` is now `scribe`.** The word processor has a new name, at the
  prompt and on the floppy (`SCRIBE.APP`). `write` is no longer a command.
- **The backup screen keeps to grey, white, teal and cyan**, frame
  included; keys stay pink, the progress bar red, and what is on green.
- A line at the bottom of the backup screen says how it is going, in a
  person's words, from the spoiled pieces of the last ten seconds and
  the tape's speed.
- When a tape has been read, the summing-up is one of ten sayings: five
  for a tape that came whole, five for one that did not.
- The cassette picture is taller and stands level with the text beside
  it. The legend is four lines: "Cassette Storage Legend (per tape
  side)", then C30, C60 and C90.
- Programs can ask for cool-coloured frames (`win_cool`); five fixed
  colours (a light grey, white, cyan, teal, dark teal) joined the palette.

## 1.34.0 — 2026-11-13

- **`tape` is a full-screen backup program.** The settings at the top,
  a cassette whose wheels turn while it works, a red progress bar with a
  percentage, and your files and folders below with a tick by each.
- A job list: Standard Backup (everything of yours) or Pick & Mix (only
  what you tick). Folders can be ticked whole.
- Compress (LZSS), on by default: text takes about half the tape.
- Speed: normal or fast. Fast takes half the tape and is easier to
  spoil; on the pretend tape it fails the dull and worn cases that
  normal passes. Reading hears which speed a tape is.
- Verify after writing, optional: reads the tape back straight away.
- Accuracy beside each file while reading, and for the whole tape.
- Restore into where each file came from, or one chosen folder.
- Reading ends when everything is in, or after ten seconds of silence.
- A tune when a tape is written or read whole.
- Settings that are on are green: a green and a red have their own
  places in the palette.
- Tapes written by 1.33.0 and earlier still read.
- Emulator only so far: not yet run on the laptop.

## 1.33.0 — 2026-11-12

- **A wall between apps and the system.** An app can now touch only its
  own memory and has to ask FLOPOS for everything else. One that reaches
  outside, or tries something only the system may do, is closed with a
  line saying why, and FLOPOS carries on. `sys` says whether the wall is
  up. Apps moved to 12MB and need 20MB of memory. Speed in the emulator:
  sound takes 7.1% of the machine instead of 6.8% while the browser plays
  a song, and the screen keeps its 30 frames a second. Not yet run on the
  laptop.
- **The tape reader heard nothing from a real recorder.** It timed each
  half of a wave, and a recorder gives the halves back unequal. It now
  times whole waves, either way up. The pretend tape in
  `tools/tapetest` has three lopsided cases, which the old reader failed
  entirely. Still to be tried on the real recorder.
- `tape load`: the level bar is twice as fine and follows the sound
  more closely; sound that is not a tape's shows its pitch. The key line
  says which socket is listening (`Tab` line-in socket active). The
  instructions start "Plug a cable from".
- `tape help` is five short lines.
- `tools/walltest` checks the wall with an app that breaks each rule.

## 1.32.2 — 2026-11-12

- **A song that never ended loading.** A damaged XM song could claim a
  tempo so fast that a tick had no length, and the tracker then turned
  for ever, which on the laptop is a frozen machine. Found by the
  garbage-feeding tool; the tempo is now bounded.

## 1.32.1 — 2026-11-12

- **Three faults found by feeding the readers garbage, and fixed.** A
  damaged XM song could make the tracker write outside its memory; so
  could an IT song with more than 128 samples; and a web page with a
  very long attribute name wrote one byte too far in browzer.
- **A tool that looks for such faults** (`tools/fuzz`): songs,
  recordings, certificates, secure connections and web pages, damaged at
  random by the hundred thousand and read by FLOPOS's own code on the PC.

## 1.32.0 — 2026-11-12

- **A shorter `help`.** `teach`, `forget`, `learned` and `echo` are gone.
  `browzer` and `inventory` are no longer listed among the commands: they
  are programs, `apps` lists them, and typing their names still runs them.
  Words taught on an older floppy are left where they are in `STATE.BIN`
  and no longer answer.
- **Name lookups are harder to fake** (security roadmap): two random
  numbers to guess, and only the name server that was asked may answer.
- **A tripwire on the stack**: if something runs wild through memory,
  FLOPOS stops on the crash screen and says so.

## 1.31.4 — 2026-11-12

- **Four network weak spots closed** (see the security roadmap): a
  connection can no longer be cut by a guessed "hang up"; nobody can take
  the router's place once it is known; a busy or hostile network can no
  longer run the stack out; and TCP's starting numbers and port come from
  the random pool. No cost in speed.
- **The crash screen names its keys in pink**, like everything else.

## 1.31.3 — 2026-11-12

- **`version` says where FLOPOS comes from:** "Cyberdelic Neurodivergent dream by
  Max, vibe-coded and synthesized with Claude." and the year the release
  was built.

## 1.31.2 — 2026-11-12

- **The version number is at the top of the terminal**, next to the name:
  `FLOPOS 1.31.2`.

## 1.31.1 — 2026-11-12

- **The inventory's program is named in full on the floppy.** It was
  `INVENTOR.APP`, one letter short, because a FAT12 name holds eight
  letters. It is now `INVENTORY.APP`, written as a long name, which is
  what another computer shows. Nothing says "inventor" any more.

## 1.31.0 — 2026-11-12

- **The inventory: your files, in real folders.** `inventory` lists what
  you have made and lets you open, rename, move (pick up with M, put down
  with P) and drop it, and make folders. On the floppy it is the folder
  `INVENTORY`, plain files any computer reads; the system's files beside
  it are never touched. See `docs/inventory.md`.
- **FLOPOS now writes the floppy's filesystem properly** (FAT12): files
  are made, grown, renamed, moved and removed, in an order that leaves
  every file whole, old or new, if the power goes mid-save. Tested on the
  PC with 9,000 random changes checked by the PC's own disk tools and 450
  simulated power cuts. **Not yet run on the laptop's real drive.**
- **`write` saves under a name you choose**, as many documents as fit,
  each taking only the room it needs. `write letter` opens or starts
  `LETTER.MD`; Ctrl-O goes to the inventory. The eight fixed documents
  are gone, which frees 130 KB.
- **`tape` backs up the whole inventory**, folders and all, and makes
  again what the floppy no longer has.
- **Writing a new release keeps the inventory**, and moves documents from
  older floppies (`WRITE1.MD` to `WRITE8.MD`) into it. A copy of your
  files stays on the PC until the floppy is verified.
- The kernel's low memory was down to its last 752 bytes; two buffers
  moved to the heap give 28 KB back.

## 1.30.0 — 2026-11-12

- **`tape`: your files on an audio cassette.** `tape save` plays the
  documents, the bookmarks, the journal and the taught words out of the
  headphone socket as two tones, for a cassette recorder; `tape load`
  listens to the tape through the line-in socket, shows whether each file
  is the same as the floppy's, and puts the different ones back if asked.
  Missing pieces are filled in by playing the tape again. See
  `docs/tape.md`. **Tried only in the emulator, against a pretend tape;
  not yet with a recorder, and listening has never run on the laptop.**
- **The sound card listens** as well as plays (line-in or microphone),
  for apps: version 5 of the app interface.
- **`fetch` is gone** from the commands and from `help`. `browzer fetch
  address` does what it did.

## 1.29.0 — 2026-11-12

- **`browzer fetch address`** reads a page out into the terminal. `fetch`
  alone still works.
- **The brain looks like a brain**: two folded halves with a cleft between
  them from front to back, where 1.26.0 had one smooth balloon. The folds
  are slow swells and crooked lines, with no spikes.
- **A black screen at the start**, with everything running behind it:
  not certainly fixed. The panel now gets 0.4 s to settle before and after
  it is asked for 800x600, the three VGA switches that can blank a
  running screen are put right after the change, and the panel's
  registers go down the serial line at every start, so a black start can
  be compared with a lit one. Typing `screen big` blind still cures it.
- **Radio that falls behind** gathers up to 24 seconds before going on
  (it was 12). A station that sends slower than it plays still pauses,
  less often.
- Tried on the laptop in 1.28.1: the wizard answers there, and radio holds
  a steady 15 seconds in hand with 30 frames a second.

## 1.28.1 — 2026-11-12

- The boot line for the wizard is kinder: "Pouring the wizard his morning
  tea...", and he is humming.

## 1.28.0 — 2026-11-12

- **The wizard answers.** A word the shell doesn't know now gets a line
  from the wizard, in his own words: he has a very small language model
  (a quarter of a megabyte, `WIZARD.WIZ`) and speaks in six moods, with
  your word put into his sentence. About one line in twenty is old-book
  gibberish. He is read from the floppy once at boot (about eight seconds
  more), thinks between frames, and never holds up the screen or a song:
  30 frames a second while he answers with music playing, at the laptop's
  speed. In the HUD too.
- He was taught on another computer, from Shakespeare and the Jargon File
  (both public domain) and from lines written for FLOPOS. `tools/wizard`
  holds the tools; "Teaching the wizard" in docs/building.md has the
  steps.
- The floppy keeps 529 KB free for files.
- Not yet tried on the laptop itself.

## 1.27.0 — 2026-11-12

- **Window frames shine in neon**, in every program: each frame and rule
  is coloured along a rainbow that runs down the screen's diagonal, and
  the colours turn slowly. The turning is done in the palette, so it
  costs no drawing.
- The HUD repaints only its top when a meter changes, not the whole of
  its grown window: 1.4 ms a frame, grown or small.

## 1.26.0 — 2026-11-11

- **Stations on radio directory pages can be picked**, any country's.
  Pages that name the stations as plain text and leave the rest to a
  script are read too: browzer now asks the same open directory the
  script asks (Radio Browser) and lists the 40 most voted stations as
  links, with format and bit rate.
- **Holding a key repeats it**: Tab runs through a page's links, and the
  arrows, PgUp, PgDn, Backspace and Delete repeat too, everywhere.
- **The HUD's window grows** for an answer longer than its four lines,
  such as `help`'s, and **pgup**, **pgdn** and the arrows scroll it.
  **esc** makes it small again. Grown, it costs 1.8 ms a frame; small,
  1.4 ms, less than before.
- **The brain is smooth**: the spikes are gone.
- **'Thunking was reading too low while sound played.** Sound is decoded
  in the timer's interrupt, and that time was counted as idle whenever
  the interrupt woke a halted processor. It is counted as work now, and
  agrees with the decoder's own count. 'Membering: was checked against
  the sizes it adds up, and was right.
- A full stop after an address written in a sentence is no longer taken
  as part of the address.

## 1.25.0 — 2026-11-10

- **Songs play on round their loop** until **s** stops them. They used
  to stop where they jump back to the start, three or four minutes in;
  chiptunes are made to go round.
- **Keys are shown in pink** wherever they are named: browzer's pages and
  its bottom edge, write, the network menu, the HUD. On web pages,
  `<kbd>`, the tag for keys, is pink too.
- **The HUD's brain is one brain** now, not two halves, and its command
  line breaks lines between words.

- **TLS 1.2**, for secure sites that have nothing newer, but only its
  modern half: the same two ciphers as TLS 1.3, a fresh key exchange for
  every connection (X25519, or P-256 for servers too old for that) signed
  by the server, and the extended master secret where the server has it.
  Nothing older is taken: no RSA key exchange, no CBC, no SHA-1, no
  renegotiation, and nothing before 1.2. A site that speaks TLS 1.3 but
  answers in 1.2 is refused, because that means someone in between
  tampered with the connection to force the older one.

## 1.24.0 — 2026-11-10

- **Songs from The Mod Archive play.** XM songs came out as noise:
  browzer loaded every song twice, and loading changes an XM where it
  lies. Now **S3M and IT songs play too**, Impulse Tracker's packed
  samples included, and long songs load: the score is read from the file
  as it plays instead of being unpacked first. A song with 64 channels
  takes under a fifth of the machine and the screen keeps its 30 frames.
- **Sound a page names without linking to it** is listed at the end of the
  page as links, without running anything: stations named in a page's
  data, and stream addresses written out as text. An address written
  as text anywhere is now a link. **Chiptune Radio** and other AzuraCast
  stations are asked for their streams.
- **DuckDuckGo works**: its front page sends a browser without JavaScript
  on to its plain page, and browzer now follows, without first reading the
  200 KB of script behind it.
- **The home page shows your bookmarks** and nothing else.
- **A download says how far it has got.**
- **The HUD, on ctrl-shift-f10**: bars that move with whatever is playing,
  the battery, 'Thunking (how busy the processor is), 'Membering: (how much
  memory is in use), a brain that turns, blue when the machine is idle and
  red when it is working hard, and a command line that takes the shell's
  commands. esc gives the keys back; ctrl-shift-f10 again closes it.
- **browzer draws much less.** Its window was drawn whole every time it
  looked for a key; now only what changed is drawn. Of each frame, the
  drawing fell from about 13 ms to 3 ms on the slowed test machine.
- The test tools are kept in `tools/audiotest` and `tools/bench`.

## 1.23.0 — 2026-11-10

- **"The timer had stopped" is gone.** It was a false alarm, raised two or
  three times a second while music played: the timer's watchdog could be
  interrupted halfway through looking by the music's own refill, and then
  misread the time. Each false alarm also emptied the keyboard's buffer.
- **browzer for chat sites and other pages without JavaScript:**
  - cookies, kept in memory only and sent only to the site that set them
  - pages that reload themselves every 30 seconds or less, coming back
    where they were scrolled to
  - frames, shown as links
  - a minute's patience for an answer that is slow to begin
- **SomaFM's front page works**: up to 2,048 links a page instead of 240,
  and picking among them is now instant (it used to search the whole page
  for every link). A station's playlist falls back to its next server if
  the first doesn't answer.
- **The clock checks its own battery**: `time` says whether the clock chip
  and the BIOS report its battery good, or that it lost its power while
  switched off, and how far out the network found the clock. Both go in
  the journal.

## 1.22.0 — 2026-11-10

- **Radio plays without pausing**, on the laptop as well as the emulator.
  The sound card is now refilled from the system timer, the way the Mac's
  Sound Manager did it, so drawing, pages and the network no longer starve
  it. The network card is emptied a thousand times a second, so a 3Com
  that fills up no longer loses packets. And TCP keeps pieces that arrive
  out of turn and tells the server exactly what is missing (SACK), so one
  lost packet costs one resend, not seconds of silence. browzer keeps up
  to a megabyte of a stream in hand, and the player shows how many seconds
  that is.
- **No slower screen while music plays.** When a window covers the whole
  screen, as browzer does, the hidden background is not drawn, and only
  what changed is redrawn and sent to the card. The screen stays at its
  full 30 frames a second with radio playing.
- **Decoders two to five times faster, same sound**: bits read 25 at a
  time, Huffman codes by table, mp3's synthesis as a fast cosine
  transform, silence skipped, and Vorbis's window worked out once. In
  two new shared libraries, `apps/lib/bits` and `apps/lib/dsp`, for
  pictures and compression later.
- **Less crackle**: 8-bit output is noise-shaped, so quiet passages lose
  their grit.
- Fixed: the kernel's memory had grown into the floppy's buffer at
  0x70000; the build now refuses that.
- New pages: docs/techniques.md, and docs/crimes-against-tokenomics.md.

## 1.21.0 — 2026-11-10

- **AAC in browzer**: AAC and HE-AAC (AAC+) stations and .aac files, such
  as SomaFM's. HE-AAC plays its 22 kHz core, which is all the Sound
  Blaster plays anyway. Checked against FAAD2: the same sound.
- **Radio no longer stutters.** The 3Com card holds only a few packets,
  so browzer used to let a server send just 4 KB at a time, and on the
  laptop that barely kept up with a 128 kbps station. Now the system timer
  empties the card into memory 250 times a second, the window is four
  times bigger, and the player gathers three seconds before it plays, and
  again if it ever runs dry, instead of stuttering. A station that hangs
  up is called back.
- **The clock sets itself**, so certificates can be checked on a laptop
  whose clock battery has died: from a time server when the network comes
  up (in Swedish time, summer time included), or else from the time saved
  at the last `bye`.
- **`wizard`** locks the screen behind the wizard's riddle. Sometimes he
  is out, and has left a note asking you to riddle yourself.
- browzer: **g** keeps the address to edit (arrows move, Ctrl-A picks it
  all), and **Esc** asks before leaving, since the laptop's Esc key can
  bounce.
- The RAGA rhythm is gone.

## 1.20.0 — 2026-11-10

- **Tick boxes, choices and drop-down lists in browzer's forms**, all plain
  HTML. Enter ticks a box or picks a choice; on a list, Enter opens it, up
  and down change it, a letter jumps to a choice starting with it.

## 1.19.0 — 2026-11-10

- **Ogg Vorbis and FLAC in browzer**, as files and as radio. Vorbis is
  checked against libvorbis and FLAC against libFLAC: the same sound, to
  within rounding. FLAC inside Ogg (how most FLAC stations send it) plays
  too, and so does a FLAC station joined part way through.
- 44.1 kHz FLAC and WAV sound cleaner: neighbouring samples are averaged
  before they go to the Sound Blaster's 22 kHz, so the highs it can't play
  no longer fold back down as noise.
- Loud mp3s no longer clip: a little headroom in the decoder, and the
  master volume a step below full.
- A long page title no longer runs into "playing ..." at the top.
- Esc on a station always says "Stopped." now; sometimes it said "The
  stream ended."
- Fixed: a WAV file of a very low rate could make browzer write past a
  buffer. Recordings slower than 4 kHz are not played now.
- The new decoders were fed thousands of deliberately broken files on the
  PC, with memory checking on, before release.

## 1.18.0 — 2026-11-09

- **mp3 and radio in browzer.** mp3 files play (MPEG-1, -2 and -2.5, any
  bitrate), and so does internet radio: mp3 streams from Shoutcast and
  Icecast stations, followed from the .m3u and .pls playlists stations link
  to. Radio gets a player screen with the station, what it says is on now,
  and how long you have been listening; esc stops it. Small files play in
  the background while you browse, like songs.
- **.wav files play** too.
- The mp3 decoder is written here, for a 120 MHz Pentium: mono, and at half
  rate for 44.1 and 48 kHz files, since the Sound Blaster plays 22 kHz
  anyway. Checked against mpg123: the same sound, to within rounding.
- Apps have 8 MB of memory now (they need 18 MB in the machine; the laptop
  has 32).

## 1.17.0 — 2026-11-09

- **Music in browzer.** A link to a .mod or .xm song (modarchive.org has
  thousands) downloads it and plays it through the Sound Blaster, and it
  keeps playing while you browse. The top edge says what is playing; s
  stops it.
- **The Sound Blaster's sample player is driven now**, not just its FM
  chip: anything can play recorded sound (app interface version 3). `dsp`
  checks it and measures it, and `report` includes that.
- The song player lives in a library apps share (apps/lib/audio), so the
  next program that makes sound can use it too.

- browzer's home page heads your pinned bookmarks "Your bookmarks", and
  says plainly how to add them.

## 1.16.0 — 2026-11-09

- **Bookmarks in browzer.** b keeps the page you are on, m lists them, and
  on the list p puts one on the home page (under "Yours") and d twice
  deletes it. They live in BOOKMARK.TXT on the floppy, readable anywhere,
  and survive writing a new version to the floppy.

## 1.15.0 — 2026-11-09

- **Forms in browzer.** Text boxes, big text areas, hidden values and
  buttons work: Tab to a box, Enter, type, Enter sends. Search boxes,
  logins and chat pages can be used. Pages are sent both
  ways sites ask for (GET and POST), and the redirect after a form is
  followed. Not yet: checkboxes, radio buttons, drop-down lists.
- **browzer is an app now**, BROWZER.APP on the floppy, like `write`. The
  system keeps what any program could use -- the network and secure
  connections, certificate check included -- and browzer has everything
  that is only about the web. `fetch` is browzer too, reading the page out
  instead of showing it. The system is 20 KB smaller.
- Apps can open network connections, plain or secure, through the system
  (app interface version 2). Apps built for version 1 still run.
- browzer starts at its home page each time now, rather than where you
  left it.

## 1.14.0 — 2026-11-09

- **Secure sites have to prove who they are.** FLOPOS now checks a site's
  certificate chain up to one of 119 trusted authorities (Mozilla's list,
  on the floppy as ROOTS.DAT), checks the name and the dates, and makes the
  site sign the handshake with its own key. Before, a secure page was
  encrypted but could have been anybody.
- `fetch` and browzer say who vouched for a site ("Let's Encrypt vouches
  for it").
- A site that can't prove itself is refused, with the reason. In browzer,
  **a** goes there anyway, and the page is marked "NOT checked".
- New code for it, all written here: RSA (both kinds of signature), ECDSA
  on P-256 and P-384, SHA-384 and SHA-512, and a certificate reader.
  `crypto` checks and times all of it, so the laptop can say what a check
  costs.
- The laptop's clock matters now: certificates are dated by it.

## 1.13.0 — 2026-11-09

- **The wizard.** Before the shell there is now a very tired wizard,
  sitting on top of the login box: "Hold, explorer! For you must answer
  this riddle before you can enter cyberdelia!" The riddle is made up on
  the spot from word lists, chosen by the clock, so no two boots ask the
  same thing. Every answer is correct. He has stopped checking.
- **Cyberdelia**, the shell, has an entrance: a little chip-tune jingle (a
  secret, found), then the screen ripples, glows softly and drifts through
  cyan and magenta while bells climb. The glow is gentle on purpose: slow,
  never close to white, never red.
- The riddle and your answer go into the journal.
- The riddle comes down the serial line too, and can be answered from
  there.
- Tools: the test scripts answer the riddle ("a turnip").

## 1.12.0 — 2026-11-09

- **Saffran shows while the machine really loads.** The coloured stripes
  are gone: he is now the first thing read off the floppy, painting in
  from the top as fast as the drive delivers him, with a thin green bar
  along the bottom while the system loads underneath him.
- **The boot messages are true now.** Every line is a real step, typed just
  before it happens, in the same dialect as before, with what the step
  found at the end of the line (memory, processor speed, boot count). All
  ten of the old phrases are kept, each attached to the step it fits:
  "Aligning the realm to the beat of the crystal..." starts the timer,
  "Flippely-Flopping" reads the floppy, "Being generally cool." is the
  last thing before the prompt. Once the serial line is up, the lines go
  down the cable too.
- The system itself is 63 KB smaller, because the picture is now its own
  file on the floppy instead of being built into the kernel.

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
