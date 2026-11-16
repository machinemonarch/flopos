#!/usr/bin/env python3
"""Turn a photograph into data/saffran.pix -- a 320x200, 256-colour boot screen.

    tools/mksplash.py photo.jpg data/saffran.pix

The file is the palette (768 bytes, six bits per gun, as the VGA DAC wants
it) and then the 64000 pixels.  The boot sector loads it off the floppy
before anything else and paints it on as it arrives; the kernel then reads
it back off the screen.

Two things make this look better than a naive resize:

Pixel aspect. Mode 13h is a 320x200 buffer shown on a 4:3 screen, so its
pixels are 1.2 times taller than they are wide. A photo resized to fill the
buffer would come out squashed; it is scaled to 1.2x its true width ratio
instead, and centred with black either side.

Dithering. 256 colours is not many for a photograph, and flat quantisation
bands badly on skin, fur and gradients. Floyd-Steinberg trades those bands
for fine noise, which at this size reads as grain rather than error.
"""
import sys
from PIL import Image

W, H = 320, 200
PIXEL_ASPECT = 1.2                      # mode 13h pixels are taller

src, out_path = sys.argv[1], sys.argv[2]

im = Image.open(src).convert("RGB")

# Scale to fill the height, correcting for the non-square pixels, then
# centre. Anything wider than the buffer gets cropped rather than squashed.
want_w = int(round(H * (im.width / im.height) * PIXEL_ASPECT))
im = im.resize((want_w, H), Image.LANCZOS)

canvas = Image.new("RGB", (W, H), (0, 0, 0))
if want_w <= W:
    canvas.paste(im, ((W - want_w) // 2, 0))
else:
    canvas.paste(im, (-((want_w - W) // 2), 0))

# Quantise to 248, not 256: the top eight entries are kept back for text
# drawn over the photograph. Without that reservation the picture owns the
# whole palette and there is no colour left that is legibly not-cat.
q = canvas.quantize(colors=248, method=Image.MEDIANCUT, dither=Image.FLOYDSTEINBERG)
pal = q.getpalette()[:744]
pix = q.tobytes()

# The VGA DAC is six bits per gun, not eight.
pal6 = [p >> 2 for p in pal]

# 248 shadow, 249..254 a green ramp, 255 white (then swapped below). Green because the text
# going over this is meant to read as a terminal, not as a caption.
RESERVED = [
    (2, 3, 2),        # 248  shadow, for the drop under every glyph
    (0, 20, 8),       # 249  dim
    (0, 30, 12),      # 250
    (4, 42, 18),      # 251
    (10, 52, 24),     # 252
    (20, 60, 32),     # 253  body text
    (34, 63, 42),     # 254  bright
    (63, 63, 63),     # 255  white
]
for r, g, b in RESERVED:
    pal6 += [r, g, b]
assert len(pal6) == 768

# Colour 0 is what the screen shows wherever the picture has not arrived
# yet, while the boot sector is still loading it.  The photo's own colour
# 0 is whatever median-cut made it, often a mid grey, so it trades places
# with the near-black shadow: the shadow becomes 0 (SPL_SHADOW in the
# kernel) and the photo's colour moves to 248.
pal6[0:3], pal6[744:747] = pal6[744:747], pal6[0:3]
pix = bytes(248 if p == 0 else p for p in pix)

open(out_path, "wb").write(bytes(pal6) + pix)
print("wrote %s: palette and %d pixels" % (out_path, W * H))
