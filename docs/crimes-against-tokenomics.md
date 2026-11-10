# Crimes against tokenomics

A record of the most unreasonable things asked of this project, and of
what became of them. Only requests that were actually carried out are
listed. The ones still waiting have to earn their place.

## The founding crime

**"An operating system on one floppy disk, for my 1996 Toshiba."** Not a
demo, an operating system: one that boots a 120 MHz Pentium with 32 MB of
memory straight into a place called `here>`. 1.44 MB, of which it still
leaves more than half free.

**"`whoami` should not say a username."** It says *You're alright.*

## Crimes of character

**"Before you can log in, a very tired wizard must ask you a riddle."**
The riddles are assembled from word lists, chosen by the clock, because
there is nothing on the machine clever enough to write them. Every
answer is correct; he stopped checking centuries ago.

**"...and when you get in, the screen should ripple and shimmer, gently
enough not to hurt anyone with epilepsy, with magical sounds."** And a
chest-opening jingle in the manner of a certain Game Boy game, written
from scratch.

**"The boot screen should be a photograph of Saffran, our cat."** He
fades up while the kernel really loads, not while it pretends to. Every
boot message names something the machine is actually doing, in words like
*Schnoodling the Schnabbleyblobs* and *Negotiating with the silicone*.

**"A `wizard` command that locks the screen. Sometimes he isn't there,
because he's off doing silly things, and he has left a note telling you to
riddle yourself."** One time in three he is out: teaching the moon to
whistle, say, or helping Saffran keep an eye on a sunbeam.

**"Remove the RAGA background music. It is awful."** Gone.

## Crimes against the 1990s

**"Can we do HTTPS certificate validation?"** On a 120 MHz Pentium:
TLS 1.3 only, a post-quantum key exchange, RSA and elliptic-curve
signatures, and full certificate chains checked against 119 authorities,
every piece written here.

**"The laptop's clock is wrong, so every certificate fails."** The clock
battery is thirty years old. Now the clock asks a time server whenever the
network comes up, keeps Swedish summer time, and with no network falls back
to the time it last said goodbye.

**"I want to listen to internet radio in the browser."** mp3 first. Then
*"Wav, ogg, etc."*, said in passing, which became Ogg Vorbis and FLAC,
each decoder written from its specification. Then *"AAC is a must, even if
it runs out my usage"*: written from the standard, and it matches the
reference decoder to within rounding.

## The great crime

**"128 kbps AAC plays three seconds and stops. FIX IT FOR GOOD. Make it
work all the time, across any possible format. If you have to rewrite the
entire audio and network stack, do so."** Then, mid-fix:
**"There will be no slowing of the screen while the music plays. If you
have to invent new ways of compressing data, if you have to invent new
MATH, then do it. Do what humanity has never been able to do before on
this hardware. Amaze us!"**

What it took, in [techniques.md](techniques.md):

- the sound card refilled from the timer interrupt, the way the Mac's
  Sound Manager did it
- the network card emptied a thousand times a second
- TCP that keeps lost-and-found packets and tells the server exactly what
  went missing
- a screen that draws only what you can see and only what changed
- decoders made two to five times faster on the very same arithmetic
- a hiss in place of the grit of an 8-bit sound card

The screen stays at its full 30 frames a second while the radio plays.

Then it was tried on the laptop: SomaFM's Drone Zone, AAC over https,
fifteen minutes without a pause, with sixteen seconds in hand. *"WELL
DONE, CLAUDE. ABSOLUTELY FANTASTIC."*

**"Yeah can we have that go away? It's super annoying."** The serial
cable, once plugged in, showed a watchdog crying wolf two or three times
a second. Gone.

**"And keep a log of my most silly, stupid and unimaginable requests."**
This is it.
