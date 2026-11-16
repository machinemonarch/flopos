# References

What was consulted while building FLOPOS. Everything in FLOPOS is written
from scratch. These documents were read for facts (how a protocol works,
what a register does, which answer a test should give), and no code or text
was copied from any of them.

## Standards FLOPOS implements

### Encryption

| Standard | What it is | Used in |
|---|---|---|
| [RFC 8446](https://www.rfc-editor.org/rfc/rfc8446) | TLS 1.3 | `tls.c` |
| [draft-ietf-tls-ecdhe-mlkem](https://datatracker.ietf.org/doc/draft-ietf-tls-ecdhe-mlkem/) | The hybrid X25519MLKEM768 key exchange | `tls.c` |
| [FIPS 203](https://csrc.nist.gov/pubs/fips/203/final) | ML-KEM, the post-quantum key agreement | `mlkem.c` |
| [FIPS 202](https://csrc.nist.gov/pubs/fips/202/final) | SHA-3 and SHAKE | `sha3.c` |
| [FIPS 180-4](https://csrc.nist.gov/pubs/fips/180-4/upd1/final) | SHA-256 | `sha256.c` |
| [RFC 2104](https://www.rfc-editor.org/rfc/rfc2104) | HMAC | `sha256.c` |
| [RFC 5869](https://www.rfc-editor.org/rfc/rfc5869) | HKDF | `sha256.c` |
| [RFC 8439](https://www.rfc-editor.org/rfc/rfc8439) | ChaCha20 and Poly1305 | `chacha.c` |
| [FIPS 197](https://csrc.nist.gov/pubs/fips/197/final) | AES | `aes.c` |
| [NIST SP 800-38D](https://csrc.nist.gov/pubs/sp/800/38/d/final) | GCM, the mode AES runs in for TLS | `aes.c` |
| [RFC 7748](https://www.rfc-editor.org/rfc/rfc7748) | X25519 | `x25519.c` |
| [IANA TLS parameters](https://www.iana.org/assignments/tls-parameters/tls-parameters.xhtml) | Cipher suite and key group numbers | `tls.c` |
| [RFC 5280](https://www.rfc-editor.org/rfc/rfc5280) | X.509 certificates and chains | `x509.c` |
| [RFC 6125](https://www.rfc-editor.org/rfc/rfc6125) | A certificate's names against a host | `x509.c` |
| [RFC 8017](https://www.rfc-editor.org/rfc/rfc8017) | RSA signatures, PKCS#1 and PSS | `rsa.c` |
| [FIPS 186-5](https://csrc.nist.gov/pubs/fips/186-5/final) | ECDSA | `ecdsa.c` |
| [SEC 2](https://www.secg.org/sec2-v2.pdf) | The P-256 and P-384 curves | `ecdsa.c` |
| [Mozilla's CA program](https://wiki.mozilla.org/CA) | The trusted authorities | `data/roots.dat` |

### The network

| Standard | What it is | Used in |
|---|---|---|
| [RFC 826](https://www.rfc-editor.org/rfc/rfc826) | ARP | `ip.c` |
| [RFC 791](https://www.rfc-editor.org/rfc/rfc791) | IPv4 | `ip.c` |
| [RFC 792](https://www.rfc-editor.org/rfc/rfc792) | ICMP (ping) | `ip.c` |
| [RFC 768](https://www.rfc-editor.org/rfc/rfc768) | UDP | `ip.c` |
| [RFC 2131](https://www.rfc-editor.org/rfc/rfc2131) | DHCP | `ip.c` |
| [RFC 1035](https://www.rfc-editor.org/rfc/rfc1035) | DNS | `ip.c` |
| [RFC 9293](https://www.rfc-editor.org/rfc/rfc9293) | TCP | `tcp.c` |
| [RFC 1945](https://www.rfc-editor.org/rfc/rfc1945) | HTTP/1.0 | `apps/browzer/web.c` |
| [RFC 3986](https://www.rfc-editor.org/rfc/rfc3986) | URLs, joining a link to a page | `apps/browzer/url.c` |

### Files

| Document | Used in |
|---|---|
| Microsoft's FAT specification (fatgen103) | `fs.c`, `tools/mkimage.py` |
| [CommonMark](https://commonmark.org/) and [Markdown](https://daringfireball.net/projects/markdown/) | `apps/scribe/doc.c` |

## The PC and its chips

The interfaces every IBM-compatible PC shares, each documented publicly
for decades:

- **The BIOS calls**: `INT 10h` (video), `INT 13h` (disk) and `INT 15h`
  (memory size, APM power), and the BIOS data area. Catalogued in
  Ralf Brown's Interrupt List[^1].
- **VESA BIOS Extension (VBE) Core Functions, version 2.0**: the 800×600
  mode and the flat frame buffer (`vbe.c`, `vga.c`).
- **Intel 8259A** interrupt controller, **Intel 8254** timer, **8042**
  keyboard controller and the A20 gate, **16550** serial chip, **MC146818**
  CMOS clock, **VGA** registers, **Yamaha YM3812 (OPL2)** FM sound.
  The OSDev wiki[^2] covers all of these well.
- **APM BIOS Interface Specification 1.2**: switching the power off
  (`cpu.c`).

## The laptop's own parts

| Document | Used for |
|---|---|
| Intel 82365SL datasheet (ExCA registers) | `pcic.c`: the slots |
| PC Card Standard: Card Information Structure | `pcic.c`: what a card is |
| 3Com EtherLink III technical reference | `el3.c`: the 3C589 |
| National Semiconductor DP8390 / NE2000 | `ne2000.c`: QEMU's card |
| Chips & Technologies 65550 panel registers | `panel.c`: `wide` |
| Creative Sound Blaster hardware programming guide | `audio.c`: the DSP |
| Intel 8237A DMA controller datasheet | `audio.c`: channel 1, auto-init |
| ProTracker MOD format notes | `apps/lib/audio/tracker.c` |
| XM format notes (XM.TXT, Triton) | `apps/lib/audio/tracker.c` |
| ISO/IEC 11172-3 and 13818-3 (MPEG audio) | `apps/lib/audio/mp3.c` |
| [PDMP3](https://github.com/technosaurus/PDMP3), public domain | `mp3tab.h`: the standard's tables |
| ISO/IEC 13818-7 and 14496-3 (AAC) | `apps/lib/audio/aac.c` |
| [VisualOn AAC encoder](https://github.com/mstorsjo/vo-aacenc), Apache 2.0 | `aactab.h`: the codebooks |
| [Vorbis I specification](https://xiph.org/vorbis/doc/Vorbis_I_spec.html) | `apps/lib/audio/vorbis.c` |
| [Ogg, RFC 3533](https://www.rfc-editor.org/rfc/rfc3533) | `vorbis.c`: pages and packets |
| [FLAC, RFC 9639](https://www.rfc-editor.org/rfc/rfc9639) | `apps/lib/audio/flac.c` |
| [Ogg FLAC mapping](https://xiph.org/flac/ogg_mapping.html) | `vorbis.c`: FLAC inside Ogg |
| SHOUTcast/Icecast metadata (ICY) | browzer: station titles |
| Inside Macintosh: Sound, the double-buffer callback | `audio_feeder`: refilling at interrupt time |
| B. G. Lee, "A new algorithm to compute the DCT" (1984) | `dsp_dct2`; mp3's synthesis |
| [TCP SACK, RFC 2018](https://www.rfc-editor.org/rfc/rfc2018) | `tcp.c`: saying which pieces came |
| [NTP, RFC 5905](https://www.rfc-editor.org/rfc/rfc5905) | `ip.c`, `rtc.c`: setting the clock |
| VEXP 1.3 by Adrian Gradwell (freeware, 1997) | `panel.c`: `wide` |
| [Scale2x](https://www.scale2x.it/) | `term.c`: smooth letters |

## Toshiba's material (consulted, not included)

Material on the Satellite Pro 430CDT:

- two Toshiba Atlas support CDs, from 1995 and 1997
- a collection of Toshiba's drivers for the Satellite Pro 430 and 435
- Toshiba's specification sheets for the 430CDT
- photographs of the machine

They were read for **facts only**: which chips are in the machine, which
ports they sit on, that the serial port is 16550 compatible, and how the
floppy and panel are wired. They are copyrighted, they are not in this
repository, and no Toshiba code, firmware or text is in FLOPOS.

## Checking the work

| Tool | Used for |
|---|---|
| [NIST ACVP test vectors](https://github.com/usnistgov/ACVP-Server) | Official ML-KEM-768 test answers |
| mpg123, LAME, libvorbis, libFLAC, FAAD2 | Reference decodes, test audio |
| SomaFM and Radio Paradise | Recordings of real AAC stations to test with |
| [OpenSSL](https://www.openssl.org/) | Cross-checking the encryption; a local TLS test server |
| [Python](https://www.python.org/) `hashlib` | Cross-checking SHA-3, SHAKE, SHA-384, SHA-512 |
| [pyca/cryptography](https://cryptography.io/) | Test signatures and test certificate chains |
| [QEMU](https://www.qemu.org/) | Running FLOPOS without the laptop |
| [QEMU's PC Card code](https://github.com/proxmox/qemu/blob/master/hw/pcmcia/pcmcia.c) | Whether QEMU could emulate the 3Com card (no) |
| [86Box](https://86box.net/) | An emulator closer to 1990s hardware |
| [mtools](https://www.gnu.org/software/mtools/) | Building the floppy image |
| [picocom](https://github.com/npat-efault/picocom) | The serial line |

## Services browzer uses

| Service | Used for |
|---|---|
| [DuckDuckGo Lite](https://lite.duckduckgo.com/lite/) | Searching, when what is typed in the address line is not an address |
| [Radio Browser](https://www.radio-browser.info/) | The stations on 72fm.com's country pages *(72fm: Max)* |

## Inspiration for browzer

Ideas only; browzer's code is written from scratch.

- **[Chawan](https://sr.ht/~bptato/chawan/), a text-mode browser by bptato**: Laying tables out in columns on a grid of letters, every size rounded to whole cells. Also the push to make scripts and styles work on real pages.

## Inspiration for the wizard

The wizard's lines were written by Claude in the spirit of these, chosen
by Max for taste and vibe. Nothing was copied from them, and none of their
text is in his training, except the two public-domain books.

- **The Jargon File 4.2.2 (public domain, Project Gutenberg)**: Read in training: hacker words and their manner.
- **The complete works of Shakespeare (public domain, Project Gutenberg)**: Read in training: the old-book gibberish he sometimes speaks.
- **[Adventure Time](https://www.imdb.com/title/tt0421357/)**: The bright land of sweets and quests.
- **Kramer, from [Seinfeld](https://www.imdb.com/title/tt0098904/)**: The neighbour with a scheme.
- **Cereal Killer, and Razor and Blade, from the film [Hackers (1995)](https://www.imdb.com/title/tt0113243/)**: The pirates of the phone lines.
- **Tom Bombadil, from The Lord of the Rings**: The singing fellow of the woods.
- **Dungeons & Dragons, its game masters and its rules**: The game master reading you the rules.

[^1]: [Ralf Brown's Interrupt List](https://www.cs.cmu.edu/~ralf/files.html)
[^2]: [The OSDev wiki](https://wiki.osdev.org/)
