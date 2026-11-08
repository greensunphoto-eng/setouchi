#!/usr/bin/env python3
"""Turns the screens dumped by the host tests into PNG previews.

    make -C ../tests                      # writes tests/build/screens.json
    python render_preview.py ../tests/build/screens.json \
        /path/to/libnds/graphics/default_font.png out/

The font is libnds' default console font (graphics/default_font.png in
https://github.com/blocksds/libnds, 256x24 pixels = 32x3 characters of 8x8).
The extra glyphs come from the JSON (dumped by the host tests) and the palette
is copied from main.c, so the preview matches what the DS draws.
Needs Pillow (pip install pillow).
"""

import argparse
import json
import os
import sys

from PIL import Image

def rgb15(r, g, b):
    return (r * 255 // 31, g * 255 // 31, b * 255 // 31)


# libnds console colors, with the changes made by tune_palette() in main.c.
PALETTE = [
    rgb15(0, 0, 0), rgb15(15, 0, 0), rgb15(0, 15, 0), rgb15(15, 15, 0),
    rgb15(7, 10, 20), rgb15(31, 16, 6), rgb15(0, 15, 15), rgb15(24, 24, 24),
    rgb15(14, 15, 18), rgb15(31, 0, 0), rgb15(0, 31, 0), rgb15(31, 31, 0),
    rgb15(12, 18, 31), rgb15(31, 0, 31), rgb15(0, 31, 31), rgb15(31, 31, 31),
]
BACKGROUND = rgb15(1, 2, 5)


def load_font(path):
    img = Image.open(path).convert("RGB")
    off = img.getpixel((0, 0))
    font = {}
    for code in range(32, 128):
        idx = code - 32
        sx, sy = (idx % 32) * 8, (idx // 32) * 8
        rows = []
        for y in range(8):
            row = 0
            for x in range(8):
                if img.getpixel((sx + x, sy + y)) != off:
                    row |= 0x80 >> x
            rows.append(row)
        font[code] = rows
    return font


def draw_screen(grid, font, glyphs):
    img = Image.new("RGB", (256, 192), BACKGROUND)
    px = img.load()
    for cy, row in enumerate(grid):
        for cx, cell in enumerate(row):
            ch, color = cell & 0xFF, (cell >> 8) & 0xF
            bitmap = glyphs.get(ch) if ch >= 128 else font.get(ch)
            if not bitmap:
                continue
            rgb = PALETTE[color]
            for y in range(8):
                bits = bitmap[y]
                for x in range(8):
                    if bits & (0x80 >> x):
                        px[cx * 8 + x, cy * 8 + y] = rgb
    return img


def compose(top, bottom, scale):
    gap = 12
    shell = (36, 38, 44)
    img = Image.new("RGB", (256 + 16, 192 * 2 + gap + 16), shell)
    img.paste(top, (8, 8))
    img.paste(bottom, (8, 8 + 192 + gap))
    return img.resize((img.width * scale, img.height * scale), Image.NEAREST)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("screens_json")
    ap.add_argument("font_png")
    ap.add_argument("out_dir")
    ap.add_argument("--scale", type=int, default=2)
    args = ap.parse_args(argv)

    font = load_font(args.font_png)
    with open(args.screens_json, encoding="utf-8") as fh:
        data = json.load(fh)
    glyphs = {int(code): rows for code, rows in data["glyphs"].items()}
    screens = data["screens"]
    os.makedirs(args.out_dir, exist_ok=True)
    for screen in screens:
        top = draw_screen(screen["top"], font, glyphs)
        bottom = draw_screen(screen["bottom"], font, glyphs)
        out = os.path.join(args.out_dir, "%s.png" % screen["name"])
        compose(top, bottom, args.scale).save(out)
        print(out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
