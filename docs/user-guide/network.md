# The network

FLOPOS can get on a wired network, find names, ping, and fetch secure
(https) pages, all on the laptop's own processor.  The web browser that
reads them, browzer, has a page of its own.[^1]

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

![The network menu, in the emulator: the address came from the network](../images/network.png)

Tab and Shift-Tab go round every setting and button, Up and Down move too,
Enter changes or edits, Esc leaves. Over the serial line the arrow keys and
Tab work too. Settings made by hand are
saved to the floppy and still there after switching off.

Without the menu, `ip` shows the same information and
`ip 192.168.1.50 255.255.255.0 192.168.1.1` sets it by hand.

## `ping`

    here> ping example.com

Finds the machine's address, asks it four times whether it is there, and
times each answer. FLOPOS also answers pings itself.

## How private it is

Secure pages (`https://`) use **TLS 1.3**, the current version of the
protocol that puts the padlock in a browser, or **TLS 1.2** for a site that
has nothing newer.

- **TLS 1.2 no older than Chrome's**: the same ciphers as TLS 1.3, and a
  new key for every connection (X25519, or P-256 or P-384 for older servers),
  signed by the site. For servers with nothing newer, the two older ciphers
  Chrome still offers (AES in CBC mode, checked with SHA-1), tried last.
  The rest of its older half is refused: RSA key exchange, renegotiation,
  and every version before it. A site that speaks
  TLS 1.3 but answers in 1.2 is refused too: TLS 1.3 sites mark such an
  answer, and it means someone in between forced the older version.
  On the laptop a TLS 1.2 page takes about as long as a 1.3 one.

- **The key exchange is post-quantum.** FLOPOS offers X25519MLKEM768, which
  combines the classic X25519 with ML-KEM-768, the method NIST
  standardised to resist future quantum computers. Traffic recorded today
  stays private even if such a computer is built later. Sites that don't
  support it fall back to plain X25519 automatically.
- **The encryption** is ChaCha20-Poly1305 or AES-128-GCM, whichever the
  site prefers.
- **Every piece is checked against published test answers.** `crypto` runs
  those checks on the laptop and times the slow parts. One X25519 key
  exchange takes about 0.3 seconds on the 430CDT. Checking a site's
  certificates takes a few signature checks more, and `crypto` times those
  too.

- **The site has to prove who it is.** It sends its certificate and the
  certificates of the authorities that vouch for it, and FLOPOS checks the
  chain up to one of the 119 authorities it trusts. These are Mozilla's
  list, the same one Firefox and most Linux systems use, kept on the floppy
  as `ROOTS.DAT`. Then the site has to sign the handshake with the key in
  its own certificate, which proves the certificate is really its own.

  `browzer fetch` says who vouched ("Let's Encrypt vouches for it"), and so does
  browzer's status line.

### When a site can't prove who it is

FLOPOS refuses the page and says why:

- **its certificate is for a different site**: the certificate names some other site
- **its certificate ran out on 2026-01-27 / is not valid until …**: out of date, or the laptop's clock is wrong
- **nobody this machine trusts vouches for it**: a self-made certificate, an unknown authority, or a site that forgot to send its authority's certificate
- **a signature in its certificate chain does not check out**: something in the chain was altered
- **it could not prove the certificate is its own**: it showed a certificate but couldn't sign with its key
- **this laptop's clock says 1995 …**: the clock is wrong, so nothing can be dated; see below

**The clock.** Certificates are only good between two dates, so the laptop
has to know roughly what day it is, and its clock battery may long since
have died. So the clock sets itself: whenever the network comes up, it asks
a time server (the one the network names, or pool.ntp.org) and keeps
Swedish time, summer time included. With no network, it starts from the
time `bye` wrote down at the last goodbye, which is near enough for
certificates. It never lets a time server put it back before that goodbye.

Any of these can mean somebody is pretending to be the site. In browzer, the
refusal page offers **a** to go there anyway. The page then loads with
"NOT checked, could be anybody" in the status line. Type nothing private
into it.

**Dates come from the laptop's own clock**, so it needs to be roughly right.
A clock an hour off makes no difference; a clock years off makes every
site fail.

**Not done:** asking whether a certificate has been revoked (that would
mean another connection, to the authority, for every site), and fetching an
authority's certificate that a badly set-up site forgot to send.

**Keeping the list up to date:** `tools/mkroots.py` rebuilds
`data/roots.dat` from a Linux PC's trusted list
(`/etc/ssl/certs/ca-certificates.crt`). The current file was made from
Ubuntu's ca-certificates 20260816. Authorities with keys FLOPOS can't
check (one, on the P-521 curve) are left out.

Sites that only speak older versions of TLS won't open.

A secure page that is stuck can be cancelled with Esc, and a server that
goes silent is given up on after 15 seconds, with the reason shown.

[^1]: [browzer](browzer.md)
