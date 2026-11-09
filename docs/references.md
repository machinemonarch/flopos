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
| [draft-ietf-tls-ecdhe-mlkem](https://datatracker.ietf.org/doc/draft-ietf-tls-ecdhe-mlkem/) | The X25519MLKEM768 hybrid key exchange for TLS 1.3 | `tls.c` |
| [FIPS 203](https://csrc.nist.gov/pubs/fips/203/final) | ML-KEM, the post-quantum key agreement | `mlkem.c` |
| [FIPS 202](https://csrc.nist.gov/pubs/fips/202/final) | SHA-3 and SHAKE | `sha3.c` |
| [FIPS 180-4](https://csrc.nist.gov/pubs/fips/180-4/upd1/final) | SHA-256 | `sha256.c` |
| [RFC 2104](https://www.rfc-editor.org/rfc/rfc2104) | HMAC | `sha256.c` |
| [RFC 5869](https://www.rfc-editor.org/rfc/rfc5869) | HKDF | `sha256.c` |
| [RFC 8439](https://www.rfc-editor.org/rfc/rfc8439) | ChaCha20 and Poly1305 | `chacha.c` |
| [FIPS 197](https://csrc.nist.gov/pubs/fips/197/final) | AES | `aes.c` |
| [NIST SP 800-38D](https://csrc.nist.gov/pubs/sp/800/38/d/final) | GCM, the mode AES runs in for TLS | `aes.c` |
| [RFC 7748](https://www.rfc-editor.org/rfc/rfc7748) | X25519 | `x25519.c` |
| [IANA TLS parameters](https://www.iana.org/assignments/tls-parameters/tls-parameters.xhtml) | The numbers for cipher suites and key exchange groups | `tls.c` |

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
| [RFC 1945](https://www.rfc-editor.org/rfc/rfc1945) | HTTP/1.0 | `http.c` |
| [RFC 3986](https://www.rfc-editor.org/rfc/rfc3986) | URLs, and joining a link to a page | `url.c` |

### Files

| Document | Used in |
|---|---|
| Microsoft's FAT specification ("FAT: General Overview of On-Disk Format", fatgen103) | `fs.c`, `tools/mkimage.py` |
| [CommonMark](https://commonmark.org/) and [Markdown](https://daringfireball.net/projects/markdown/) | the document format in `apps/write/doc.c` |

## The PC and its chips

The interfaces every IBM-compatible PC shares, each documented publicly
for decades:

- **The BIOS calls**: `INT 10h` (video), `INT 13h` (disk) and `INT 15h`
  (memory size, APM power), and the BIOS data area. Catalogued in
  [Ralf Brown's Interrupt List](https://www.cs.cmu.edu/~ralf/files.html).
- **VESA BIOS Extension (VBE) Core Functions, version 2.0**: the 800×600
  mode and the flat frame buffer (`vbe.c`, `vga.c`).
- **Intel 8259A** interrupt controller, **Intel 8254** timer, **8042**
  keyboard controller and the A20 gate, **16550** serial chip, **MC146818**
  CMOS clock, **VGA** registers, **Yamaha YM3812 (OPL2)** FM sound.
  [The OSDev wiki](https://wiki.osdev.org/) covers all of these well.
- **APM BIOS Interface Specification 1.2**: switching the power off
  (`cpu.c`).

## The laptop's own parts

| Document | Used for |
|---|---|
| Intel 82365SL PC Card Interface Controller (PCIC) datasheet: the "ExCA" register set | `pcic.c`: powering the slots, memory and I/O windows |
| PC Card Standard (PCMCIA), the Card Information Structure (CIS) | `pcic.c`: reading what a card says it is |
| 3Com EtherLink III technical reference (3C5x9 family) | `el3.c`: the 3C589's registers and how to bring it up |
| National Semiconductor DP8390 / NE2000 | `ne2000.c`: the card QEMU emulates |
| Chips & Technologies 65550 flat-panel registers | `panel.c`: stretching 320×200 to fill the panel |
| VEXP 1.3 by Adrian Gradwell (freeware, 1997), notes on the 65550's panel stretching | `panel.c`: `wide`, and why it is set after the mode change |
| [Scale2x](https://www.scale2x.it/) | Smoothing the letters at 800×600 (`term.c`) |

## Toshiba's material (consulted, not included)

Max supplied this material for the Satellite Pro 430CDT:

- two Toshiba Atlas support CDs (1995 and April 1997)
- the Satellite Pro 430/435 driver collection
- Toshiba's 430CDT specification sheets, including Toshiba Canada's
  product page
- the 430CDT page from [minus zero degrees](https://www.minuszerodegrees.net/),
  with photographs of the machine

They were read for **facts only**: which chips are in the machine, which
ports they sit on, that the serial port is 16550 compatible, and how the
floppy and panel are wired.

They are copyrighted. They live in `reference/`, which is never committed,
and no Toshiba code, firmware or text is in FLOPOS.

## Checking the work

| Tool | Used for |
|---|---|
| [NIST ACVP test vectors](https://github.com/usnistgov/ACVP-Server) | The official answers ML-KEM-768 is checked against |
| [OpenSSL](https://www.openssl.org/) | Checking SHA-256, HMAC, HKDF, ChaCha20-Poly1305, X25519 and AES-GCM against thousands of random inputs, and as a local TLS 1.3 test server (`openssl s_server`) |
| [Python](https://www.python.org/) `hashlib` | Checking SHA-3 and SHAKE |
| [QEMU](https://www.qemu.org/) | Running FLOPOS without the laptop, including its NE2000 network card |
| [QEMU's PC Card code](https://github.com/proxmox/qemu/blob/master/hw/pcmcia/pcmcia.c) | Looked at (Max's find) to see whether QEMU could emulate the 3Com card. It can't on a PC. |
| [86Box](https://86box.net/) | Looked at as an emulator closer to 1990s hardware |
| [mtools](https://www.gnu.org/software/mtools/) | Building and reading the floppy image |
| [picocom](https://github.com/npat-efault/picocom) | The serial line from the Linux PC |

## Services browzer uses

| Service | Used for |
|---|---|
| [FrogFind](http://frogfind.com/) | Searching, when words are typed instead of an address. Made by Action Retro for vintage computers. |
