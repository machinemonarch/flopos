# The network

FLOPOS can get on a wired network, find names, ping, read web pages and
fetch secure (https) pages, all on the laptop's own processor.

## The card

The laptop has no network port of its own. FLOPOS drives a **3Com 3C589**
Ethernet PC Card (EtherLink III) in either PC Card slot, with the usual
dongle and a network cable.

It switches the card on the first time anything needs the network. If the
card is found but no cable signal is seen, it says *"The card is up, but
nothing is plugged into it."* If that happens with a cable in, check that
the dongle is pushed fully into the card and that the light on the switch
port is on.

The other card in the laptop, a LASAT Credit 560 modem, is recognised by
`card` but not driven yet.

## `network`: the menu

    here> network

A window showing:

- the **card** and its **link**: whether a cable is connected
- the **setting**: *automatic, from the network* (DHCP) or *by hand*
- **address**, **mask**, **way out** (the gateway, the router to the rest
  of the internet) and **names** (the name server that turns
  `example.com` into an address)
- **apply**, **ask again** and **close**

Up and Down move, Enter changes or edits, Esc leaves. Over the serial line
the arrow keys work too, and Tab moves down. Settings made by hand are
saved to the floppy and still there after switching off.

Without the menu, `ip` shows the same information and
`ip 192.168.1.50 255.255.255.0 192.168.1.1` sets it by hand.

## `ping`

    here> ping example.com

Finds the machine's address, asks it four times whether it is there, and
times each answer. FLOPOS also answers pings itself.

## `fetch`

    here> fetch example.com
    here> fetch https://www.rfc-editor.org/

Reads a web page and prints it as text in the terminal: headings stand out,
links are marked, words are wrapped to the screen, Swedish letters come out
right, and redirects are followed.

## browzer

    here> browzer

A small web browser that takes the whole screen. The address is along the
top, the page in the middle and a status line along the bottom.

| Key | What it does |
|---|---|
| Up / Down | Scroll a line |
| Page Up / Page Down / Space | Scroll a screen |
| Tab / Shift-Tab | Next / previous link |
| Enter | Follow the selected link |
| Backspace or Left | Back |
| `g` | Go somewhere: type an address |
| `r` | Load the page again |
| `h` | Home |
| Esc | Cancel a page that is taking too long, or leave |

**Typing words instead of an address searches**, through FrogFind. It is a
search engine made for old computers, and it answers with plain, simple
pages that suit a machine like this one.

What it does with modern pages:

- **Images** become boxes the size of the original picture, saying
  "Image of …" with the picture's description. Small icons stay inline as
  text, and invisible tracking pixels disappear.
- **JavaScript** is not run, on purpose (see
  [Philosophy](philosophy.md)). Where a page offers a no-JavaScript
  version of itself, browzer shows that.
- **A page that fails to load says why**, on the page.
- Status messages also go down the serial line.

Pages built entirely by JavaScript will come up mostly empty. Pages with
real text in them work.

## How private it is

Secure pages (`https://`) use **TLS 1.3**, the current version of the
protocol that puts the padlock in a browser.

- **The key exchange is post-quantum.** FLOPOS offers X25519MLKEM768, which
  combines the classic X25519 with ML-KEM-768, the method NIST
  standardised to resist future quantum computers. Traffic recorded today
  stays private even if such a computer is built later. Sites that don't
  support it fall back to plain X25519 automatically.
- **The encryption** is ChaCha20-Poly1305 or AES-128-GCM, whichever the
  site prefers.
- **Every piece is checked against published test answers.** `crypto` runs
  those checks on the laptop and times the slow parts. One X25519 key
  exchange takes about 0.3 seconds on the 430CDT.

**What it doesn't do yet: check the site's certificate.** The connection is
encrypted, but FLOPOS can't yet tell the real site from an impostor sitting
in between. Reading is fine. Don't type a password into anything over it
until certificate checking exists. FLOPOS says this on the screen too.

Sites that only speak older versions of TLS won't open.

A secure page that is stuck can be cancelled with Esc, and a server that
goes silent is given up on after 15 seconds, with the reason shown.

## Testing without the laptop

In the QEMU emulator there are no PC Card slots, so FLOPOS also has a driver
for an emulated **NE2000** card. It is only looked for when the machine has
no PC Card adapter. See [Building](building.md).
