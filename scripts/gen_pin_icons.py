#!/usr/bin/env python3
"""Bake the map-pin shape, its white fill and the type glyphs into one header.

    python3 scripts/gen_pin_icons.py --height 48

Why this exists rather than freeink-sdk's gen_icons.py: that generator emits one
1bpp *ink* array per asset, square, and nothing else. A pin needs three things it
cannot give:

  * a **fill mask**. `freeink::Icon` is ink-only -- bit 1 means "leave this pixel
    alone" -- so a hollow outline drawn over a map lets road lines through the
    head. The mask is the shape's silhouette (outline plus interior), painted
    white before the ink goes down.
  * **non-square** dimensions. A teardrop is taller than it is wide.
  * the **type glyph composited inside the head**, so one bitmap is one pin and
    the device draws two arrays instead of doing vector geometry per frame.

The head's centre and its clear radius are measured off the rasterised silhouette,
not hardcoded: hand a different pin-shape.svg to this script and the glyph still
lands in the middle of whatever the head turns out to be.

Rasteriser: rsvg-convert (librsvg) when present, cairosvg otherwise. The two agree
closely enough at these sizes, and the machine this was written on has only the
second one.
"""
import argparse
import io
import os
import re
import shutil
import subprocess
import sys
from collections import deque

from PIL import Image, ImageDraw, ImageFont

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SHAPE_SVG = os.path.join(REPO, "src/components/icons/pin-shape.svg")
LUCIDE_DIR = os.path.join(REPO, "freeink-sdk/libs/assets/Icons/lucide/icons")
MANIFEST = os.path.join(REPO, "src/components/icons/pins.icons.txt")
OUT = os.path.join(REPO, "src/components/icons/pins_shape.h")

# Same threshold gen_icons.py uses, so a glyph baked here and one baked there look
# the same on the panel.
THRESHOLD = 110

# For `text:` entries (`digit:` is the same thing, kept because a numeral is what
# it is mostly used for). Baked here, so the device needs no font for a pin's
# number -- and no icon says "3" as clearly as a 3, or "parking" as clearly as a P.
DIGIT_FONTS = (
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
)


def text_grid(text, size):
    """Short text, rendered as large as fits a size x size box, as an ink grid."""
    path = next((p for p in DIGIT_FONTS if os.path.exists(p)), None)
    if path is None:
        sys.exit("ERROR: no bold sans font for text glyphs; install dejavu or liberation")
    # Grow the point size until the drawn ink no longer fits, then step back.
    best = None
    for points in range(size, size * 3):
        font = ImageFont.truetype(path, points)
        img = Image.new("L", (size * 3, size * 3), 255)
        ImageDraw.Draw(img).text((size, size), text, font=font, fill=0)
        grid = ink_grid(img)
        ys = [y for y in range(len(grid)) if any(grid[y])]
        xs = [x for x in range(len(grid[0])) if any(row[x] for row in grid)]
        if not ys:
            continue
        w, h = xs[-1] - xs[0] + 1, ys[-1] - ys[0] + 1
        if w > size or h > size:
            break
        best = [row[xs[0]:xs[-1] + 1] for row in grid[ys[0]:ys[-1] + 1]]
    if best is None:
        sys.exit(f"ERROR: could not fit '{text}' into {size}px")
    # Centre it in a size x size box, so it sits where a Lucide glyph would.
    out = [[False] * size for _ in range(size)]
    oy, ox = (size - len(best)) // 2, (size - len(best[0])) // 2
    for y, row in enumerate(best):
        for x, v in enumerate(row):
            if v:
                out[oy + y][ox + x] = True
    return out


def dilate(grid, radius):
    """Grow a grid by `radius` pixels in every direction (a chebyshev dilation)."""
    if radius <= 0:
        return grid
    h, w = len(grid), len(grid[0])
    out = [[False] * w for _ in range(h)]
    for y in range(h):
        for x in range(w):
            if not grid[y][x]:
                continue
            for dy in range(-radius, radius + 1):
                for dx in range(-radius, radius + 1):
                    ny, nx = y + dy, x + dx
                    if 0 <= ny < h and 0 <= nx < w:
                        out[ny][nx] = True
    return out


def rasterize(svg_path, width, height):
    if shutil.which("rsvg-convert"):
        png = subprocess.run(
            ["rsvg-convert", "-w", str(width), "-h", str(height), svg_path],
            capture_output=True,
            check=True,
        ).stdout
    else:
        import cairosvg

        png = cairosvg.svg2png(url=svg_path, output_width=width, output_height=height)
    img = Image.open(io.BytesIO(png)).convert("RGBA")
    bg = Image.new("RGBA", img.size, (255, 255, 255, 255))
    bg.paste(img, mask=img.split()[3])
    return bg.convert("L")


def ink_grid(img):
    """True where the artwork is drawn."""
    w, h = img.size
    px = img.load()
    return [[px[x, y] < THRESHOLD for x in range(w)] for y in range(h)]


def crop_to_ink(grid):
    h, w = len(grid), len(grid[0])
    ys = [y for y in range(h) if any(grid[y])]
    xs = [x for x in range(w) if any(grid[y][x] for y in range(h))]
    if not ys or not xs:
        sys.exit("ERROR: the shape rasterised empty")
    return [row[xs[0] : xs[-1] + 1] for row in grid[ys[0] : ys[-1] + 1]]


def silhouette(ink):
    """Outline plus everything the outline encloses.

    Flood fill the not-ink pixels from the border: whatever the flood cannot reach
    is inside the shape. Works for any closed outline, which is the only kind a pin
    can be.
    """
    h, w = len(ink), len(ink[0])
    outside = [[False] * w for _ in range(h)]
    queue = deque()
    for x in range(w):
        for y in (0, h - 1):
            if not ink[y][x] and not outside[y][x]:
                outside[y][x] = True
                queue.append((x, y))
    for y in range(h):
        for x in (0, w - 1):
            if not ink[y][x] and not outside[y][x]:
                outside[y][x] = True
                queue.append((x, y))
    while queue:
        x, y = queue.popleft()
        for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            nx, ny = x + dx, y + dy
            if 0 <= nx < w and 0 <= ny < h and not ink[ny][nx] and not outside[ny][nx]:
                outside[ny][nx] = True
                queue.append((nx, ny))
    return [[ink[y][x] or not outside[y][x] for x in range(w)] for y in range(h)]


def head_circle(ink, mask):
    """Centre and clear radius of the head, measured off the shape itself.

    The head is the widest enclosed run in the upper half: for each row, take the
    longest stretch of interior (mask and not ink) and keep the widest one. Its
    centre is the head's centre and half its length is the room a glyph has.
    """
    h, w = len(ink), len(ink[0])
    best = (0, 0, 0)  # length, cx, cy
    for y in range(h // 2 + 1):
        run = 0
        for x in range(w + 1):
            inside = x < w and mask[y][x] and not ink[y][x]
            if inside:
                run += 1
                continue
            if run > best[0]:
                best = (run, x - run + run // 2, y)
            run = 0
    length, cx, cy = best
    if length == 0:
        sys.exit("ERROR: could not find the head's interior")
    # The widest row is the head's horizontal diameter, so its own y is the centre.
    return cx, cy, length // 2


def pack(grid):
    """1bpp, MSB-first, bit 0 = draw (the convention freeink::Icon uses)."""
    h, w = len(grid), len(grid[0])
    rowBytes = (w + 7) // 8
    out = []
    for y in range(h):
        for b in range(rowBytes):
            byte = 0
            for bit in range(8):
                x = b * 8 + bit
                drawn = x < w and grid[y][x]
                if not drawn:
                    byte |= 1 << (7 - bit)
            out.append(byte)
    return out


def parse_manifest(path):
    entries = []
    for line in open(path):
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        alias, lucide = (p.strip() for p in line.split("=", 1)) if "=" in line else (line, line)
        entries.append((alias, lucide))
    return entries


def carray(name, data):
    body = ", ".join(f"0x{b:02X}" for b in data)
    return f"static const uint8_t {name}[] = {{{body}}};"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--height", type=int, default=48, help="pin height in device pixels")
    ap.add_argument("--glyph", type=int, default=0, help="glyph size; 0 = as big as the head allows")
    ap.add_argument(
        "--glyph-dy",
        type=int,
        default=4,
        help="pixels to push the glyph down inside the head; the head's measured centre reads high",
    )
    ap.add_argument(
        "--halo",
        type=int,
        default=1,
        help="white pixels painted outside the shape, so its outline never touches a map line",
    )
    args = ap.parse_args()

    # Rasterise square and crop: the shape does not fill its own viewBox, and the
    # height that matters is the shape's, not the canvas's.
    square = args.height * 3
    ink = crop_to_ink(ink_grid(rasterize(SHAPE_SVG, square, square)))
    scale = args.height / len(ink)
    ink = crop_to_ink(
        ink_grid(rasterize(SHAPE_SVG, max(1, round(square * scale)), max(1, round(square * scale))))
    )
    # Room for the halo on every side, so the dilated mask is not clipped.
    pad = max(0, args.halo)
    if pad:
        blank = [False] * (len(ink[0]) + pad * 2)
        ink = (
            [blank[:] for _ in range(pad)]
            + [[False] * pad + row + [False] * pad for row in ink]
            + [blank[:] for _ in range(pad)]
        )
    height, width = len(ink), len(ink[0])
    # The halo is what separates the pin from the map: the shape's own outline is
    # black and lands straight on top of road lines otherwise. Same job the position
    # marker's white halo does (MapActivity::drawPositionMarker()).
    mask = dilate(silhouette(ink), pad)
    cx, cy, clear = head_circle(ink, mask)

    glyph = args.glyph
    if glyph == 0:
        # The largest even size whose diagonal still fits the clear circle, so a
        # corner of the glyph cannot touch the outline.
        glyph = int((clear * 2) / (2 ** 0.5)) & ~1
    print(f"pin {width}x{height}, head centre ({cx},{cy}), clear radius {clear}, glyph {glyph}px")

    lines = [
        "#pragma once",
        "",
        "#include <cstdint>",
        "",
        "// Generated by scripts/gen_pin_icons.py from src/components/icons/pin-shape.svg",
        "// plus the Lucide glyphs in src/components/icons/pins.icons.txt. Do not edit.",
        "//",
        "// Two arrays per pin, both 1bpp MSB-first with bit 0 = draw:",
        "//   kPinShapeMaskBits -- the silhouette, painted white first so the map does",
        "//                        not show through the head",
        "//   kPinShape<Alias>Bits -- the outline with the type glyph inside it",
        "//",
        "// The anchor is the bottom centre: the tip of the tail is the coordinate.",
        "",
        f"inline constexpr int kPinShapeWidth = {width};",
        f"inline constexpr int kPinShapeHeight = {height};",
        "",
        carray("kPinShapeMaskBits", pack(mask)),
        "",
    ]

    for alias, lucide in parse_manifest(MANIFEST):
        if lucide.startswith("digit:") or lucide.startswith("text:"):
            gi = text_grid(lucide.split(":", 1)[1], glyph)
        else:
            svg = os.path.join(LUCIDE_DIR, lucide + ".svg")
            if not os.path.exists(svg):
                sys.exit(f"ERROR: missing svg for '{alias}': {svg}")
            gi = ink_grid(rasterize(svg, glyph, glyph))
        composed = [row[:] for row in ink]
        # The head's geometric centre sits high to the eye: the tail draws the eye
        # down, so a glyph centred on the measured centre reads as if it were
        # floating at the top. 4 px down, judged on the panel twice (2026-08-17) --
        # the default is what the committed header was generated with.
        ox, oy = cx - glyph // 2, cy - glyph // 2 + args.glyph_dy
        for y in range(glyph):
            for x in range(glyph):
                if gi[y][x] and 0 <= oy + y < height and 0 <= ox + x < width:
                    composed[oy + y][ox + x] = True
        ident = "".join(part.capitalize() for part in re.split(r"[^A-Za-z0-9]", alias) if part)
        lines.append(f"// {alias}  (lucide: {lucide})")
        lines.append(carray(f"kPinShape{ident}Bits", pack(composed)))
        lines.append("")

    open(OUT, "w").write("\n".join(lines))
    print(f"wrote {OUT}")


if __name__ == "__main__":
    main()
