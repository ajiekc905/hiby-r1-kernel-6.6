#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2026 ajiekc905
"""mksplash.py — the boot splash of the card-player-boot bundle (dev-time only, needs Pillow).

S11jpeg_display_shell draws /etc/logo{,1,2}.jpeg (by the theme byte in mtd5)
the moment /dev/fb0 exists; it stays on the glass until the player on the
card draws its first frame. Colours = the player's default theme tokens,
the note = Material Symbols Rounded music_note (Apache-2.0, MaterialSymbolsRounded.ttf in $R1_FONT_DIR).

    R1_FONT_DIR=/path/to/fonts python3 mksplash.py ..
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FONTS = os.environ.get("R1_FONT_DIR", os.path.join(REPO, "player", "data", "fonts"))
W, H = 480, 800
BG, ACCENT = (0, 0, 0), (0xe0, 0xa1, 0x00)


def splash():
    im = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(im)
    icon = ImageFont.truetype(os.path.join(FONTS, "MaterialSymbolsRounded.ttf"), 150)
    note = ""
    x0, y0, x1, y1 = d.textbbox((0, 0), note, font=icon)
    cx, cy = W // 2, H // 2
    d.ellipse((cx - 110, cy - 110, cx + 110, cy + 110), outline=ACCENT, width=6)
    d.text((cx - (x0 + x1) // 2, cy - (y0 + y1) // 2), note, font=icon, fill=ACCENT)
    return im


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "."
    im = splash()
    for n in ("logo.jpeg", "logo1.jpeg", "logo2.jpeg"):
        im.save(os.path.join(out, n), "JPEG", quality=92, optimize=False, progressive=False, subsampling=0)


if __name__ == "__main__":
    main()
