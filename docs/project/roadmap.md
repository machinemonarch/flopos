# Roadmap

Ideas waiting their turn. None is started until Max picks it, and each
gets a short plan with a rough cost first. What is done is in the
CHANGELOG.

## Planned

- **Packed files, the release after next:** The kernel, the apps and the pictures kept packed on the floppy with a simple, fast LZ, and unpacked as they load. Nothing is packed today. A strong packer halves the kernel and browzer; a simple one saves about a third, roughly 250 KB of floppy, and loads a third faster, since the Pentium unpacks far faster than the drive reads. It needs an unpacker in the boot stub and the app loader, and testing on the laptop.
- **Stereo, later:** The card is Sound Blaster Pro compatible, which plays 8-bit stereo up to 22 kHz (`dsp` says which card: 4.xx would allow 16-bit). FLOPOS plays mono, adding the channels before the costly last step of decoding; stereo roughly doubles that step (Opus from about 10% of the machine to 15-20%). It needs a stereo mode in `audio.c` and each decoder keeping its channels apart.

## Waiting to be picked

- **A chat app:** Like Discord, IRC, WhatsApp or Signal; which is open. IRC is easiest for this machine, given a channel worth being in.
- **A mystical protocol for sharing data:** A client on the Toshiba and a server elsewhere, doing what SFTP, NFS or SMB do without reinventing netcat. It should feel obscure and enticing, like a forgotten metro tunnel, all the way down to how it checks data and starts a session. What it is for is undecided, so it starts with an interview of its product owner.

## Security, waiting for a decision

Nothing at the moment. What was fixed is in Security[^1].

[^1]: [Security](../design/security.md)
