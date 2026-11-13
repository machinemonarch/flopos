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

## The crime of the heads-up display

**"I want a HUD. CPU is displayed by "'Thunking" and RAM is called
"'Membering:". An average of both is displayed by a rotating human brain
in 3D going from blue-green-yellow-orange-red. Toggled with
ctrl-shift-f10. It integrates nicely with whatever else is on screen."**
And then: **"HUD also has a small cli that takes the same commands as the
regular cli."** A brain of 226 points and 414 lines, turning fifteen
times a second, folded with sines. 'Thunking is measured in the time the
processor spends halted, and 'Membering: in bytes. The command line hears
everything the shell says. It all costs 1.6 ms a frame. Building it found
that browzer had been drawing its whole window on every frame since 1.22.

**"Well I didn't HEAR any music from the mod and xm files."** Every XM was
being turned into noise on its way in. Then S3M and IT, with Impulse
Tracker's own compression, because the Mod Archive is full of them.

**"And start working on that cassette-based data backup software."** A
1.44 MB floppy, backed up to a C60 at 110 bytes a second, through the
headphone socket of a 1996 laptop into a PYE data recorder. A full
document takes two and a half minutes. The sound card had only ever
played; it had to be taught to listen first.

**"Do we even have a true filesystem yet? Anyways, I need some kind of
abstraction for storing my files in. Let's call it 'inventory'."** It
turned out the floppy had been a proper DOS disk all along, and FLOPOS
had been getting by on eight blank documents made in advance. So: the
rest of FAT12, folders, a move that is picking things up and putting
them down, and several hundred power cuts on purpose.

**"and implement all the security stuff you had left"** Seven words, sent
while the previous seven jobs were still running. Among the things left:
a tool that damages songs by the hundred thousand to see what breaks.
Three things broke.

**"you havent started on this even?"** Asked about the largest item on
the security list, the one filed as "large", an hour after ordering all
of them in seven words. A fair question. The apps now live behind a wall
and none of them has noticed.

**"the silliest little 'trudelutt' you can possibly imagine"** A formal
requirement for the backup program's completion signal. Delivered: an
arpeggio, a slide whistle, two boops and a low wobble, synthesised from
square waves by the same program that had just finished checksumming.
The cassette's wheels also had to spin.

**"Bomboclat! Das rough!"** The backup program was renamed after a
Rastafarian tug pilot from a 1984 novel, repainted three times in one
afternoon (teal, then embers, then red, gold and green), and taught to
comment on tape quality in patois. It checks every piece with a CRC and
then says "Irie!".

**"And keep a log of my most silly, stupid and unimaginable requests."**
This is it.
