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
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
)


def text_grid(text, size, fit=None):
    """Short text, rendered as large as fits a `fit` x `fit` box (default: `size`),
    centred in a size x size ink grid -- `fit` < `size` leaves a margin so the
    numeral reads lighter next to the Lucide glyphs it shares a box with."""
    fit = fit or size
    path = next((p for p in DIGIT_FONTS if os.path.exists(p)), None)
    if path is None:
        sys.exit("ERROR: no sans font for text glyphs; install dejavu or liberation")
    # Grow the point size until the drawn ink no longer fits, then step back.
    best = None
    for points in range(fit, fit * 3):
        font = ImageFont.truetype(path, points)
        img = Image.new("L", (size * 3, size * 3), 255)
        ImageDraw.Draw(img).text((size, size), text, font=font, fill=0)
        grid = ink_grid(img)
        ys = [y for y in range(len(grid)) if any(grid[y])]
        xs = [x for x in range(len(grid[0])) if any(row[x] for row in grid)]
        if not ys:
            continue
        w, h = xs[-1] - xs[0] + 1, ys[-1] - ys[0] + 1
        if w > fit or h > fit:
            break
        best = [row[xs[0]:xs[-1] + 1] for row in grid[ys[0]:ys[-1] + 1]]
    if best is None:
        sys.exit(f"ERROR: could not fit '{text}' into {fit}px")
    # Centre it in a size x size box, so it sits where a Lucide glyph would.
    out = [[False] * size for _ in range(size)]
    oy, ox = (size - len(best)) // 2, (size - len(best[0])) // 2
    for y, row in enumerate(best):
        for x, v in enumerate(row):
            if v:
                out[oy + y][ox + x] = True
    return out


def rotate_about(img, angle_deg, pivot):
    """Rotate a greyscale image about `pivot`, expanding the canvas to hold it.

    Returns (image, new_pivot). Done at the oversampled size and downscaled after,
    so the outline survives: a nearest-neighbour rotation of a 3 px stroke at 1:1
    breaks it into dashes.
    """
    w, h = img.size
    px, py = pivot
    # Pad so the rotation cannot clip, with the pivot at the centre of the padding.
    reach = int(max(w, h) * 1.5) + 4
    canvas = Image.new("L", (reach * 2, reach * 2), 255)
    canvas.paste(img, (reach - px, reach - py))
    rotated = canvas.rotate(-angle_deg, resample=Image.BICUBIC, center=(reach, reach), fillcolor=255)
    return rotated, (reach, reach)


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
    ap.add_argument(
        "--steps",
        type=int,
        default=16,
        help="baked rotations of the shape, clockwise from point-down; 16 matches MapHeading's steps",
    )
    ap.add_argument("--oversample", type=int, default=8, help="rasterise at this multiple, then rotate and shrink")
    args = ap.parse_args()

    # Rasterise oversampled, once. Every rotation comes off this one image, so the
    # outline stays smooth: rotating a 1bpp bitmap directly turns a 3 px stroke into
    # dashes, and rotating the vector per step would need a rasteriser call per step.
    big = args.height * args.oversample
    # The shape does not fill its own viewBox, so render square and crop to the ink.
    square = rasterize(SHAPE_SVG, big * 3 // 2, big * 3 // 2)
    ink_big = crop_to_ink(ink_grid(square))
    # Rebuild an image from the cropped grid: PIL is what rotates.
    hb, wb = len(ink_big), len(ink_big[0])
    shape_img = Image.new("L", (wb, hb), 255)
    for y in range(hb):
        for x in range(wb):
            if ink_big[y][x]:
                shape_img.putpixel((x, y), 0)
    # The point is the bottom of the shape, on its centre line.
    tip_big = (wb // 2, hb - 1)

    # The head's centre, measured once on the upright shape. Re-measuring it per
    # rotated frame (the previous approach: head_circle() on the rotated raster)
    # broke at odd angles -- the tail's own stroke sweeps into the "widest interior
    # run in the upper half" scan and drags the detected centre sideways, off the
    # glyph the head circle actually shows on the panel (reported 2026-08-23,
    # visible on the S8: the flag sat well left of the ring's true centre at a
    # rotated step). A circle stays a circle under rotation -- only its position
    # moves -- so instead of re-detecting it, a marker dot is carried through the
    # exact same rotate/resize/crop/pad pipeline as the shape image and its
    # resulting centroid is the frame's head point. That tracks the real
    # transform bake() applies to the artwork instead of re-guessing it from a
    # silhouette that the tail can contaminate.
    head_mask_big = silhouette(ink_big)
    head_big = head_circle(ink_big, head_mask_big)
    # `--glyph-dy` (the head's measured centre reads high) has to be applied
    # *before* rotation, in the same local frame as head_big: baking it into
    # where the marker ellipse is drawn means it rides through the identical
    # rotate/resize/crop pipeline as everything else and rotates with the pin.
    # Adding it after the fact, to the frame's own already-rotated final y,
    # was a flat screen-space nudge that only matched the pin's own "toward
    # the tip" direction at step 0 -- at every other step it pointed the wrong
    # way by an angle-dependent amount, up to the full offset at 180 degrees
    # (reported 2026-08-24: pin_meet's ring+dot glyph, the one glyph
    # symmetric enough to reveal it, visibly off-centre inside the pin head at
    # every non-cardinal rotation step).
    scale_to_big = hb / args.height
    head_marker_center = (head_big[0], head_big[1] + args.glyph_dy * scale_to_big)
    head_marker_img = Image.new("L", (wb, hb), 255)
    ImageDraw.Draw(head_marker_img).ellipse(
        [
            head_marker_center[0] - 24,
            head_marker_center[1] - 24,
            head_marker_center[0] + 24,
            head_marker_center[1] + 24,
        ],
        fill=0,
    )

    def bake(angle_deg):
        """One rotation: (mask grid, ink grid, tip xy, head xy)."""
        if angle_deg == 0:
            img, marker, pivot = shape_img, head_marker_img, tip_big
        else:
            img, pivot = rotate_about(shape_img, angle_deg, tip_big)
            marker, _ = rotate_about(head_marker_img, angle_deg, tip_big)
        scale = args.height / hb
        size = (max(1, round(img.width * scale)), max(1, round(img.height * scale)))
        small = img.resize(size, Image.LANCZOS)
        marker_small = marker.resize(size, Image.LANCZOS)
        pivot_small = (pivot[0] * scale, pivot[1] * scale)
        grid = ink_grid(small)
        marker_grid = ink_grid(marker_small)
        # Crop to the ink and keep the pivot and the head marker in the cropped
        # frame's coordinates -- the same window for both, since they share the
        # same canvas throughout.
        ys = [y for y in range(len(grid)) if any(grid[y])]
        xs = [x for x in range(len(grid[0])) if any(row[x] for row in grid)]
        grid = [row[xs[0] : xs[-1] + 1] for row in grid[ys[0] : ys[-1] + 1]]
        marker_grid = [row[xs[0] : xs[-1] + 1] for row in marker_grid[ys[0] : ys[-1] + 1]]
        tip = (pivot_small[0] - xs[0], pivot_small[1] - ys[0])
        marker_xs = [x for row in marker_grid for x, v in enumerate(row) if v]
        marker_ys = [y for y, row in enumerate(marker_grid) for v in row if v]
        head = (sum(marker_xs) / len(marker_xs), sum(marker_ys) / len(marker_ys))
        # Room for the halo on every side, so the dilated mask is not clipped.
        pad = max(0, args.halo)
        if pad:
            blank = [False] * (len(grid[0]) + pad * 2)
            grid = (
                [blank[:] for _ in range(pad)]
                + [[False] * pad + row + [False] * pad for row in grid]
                + [blank[:] for _ in range(pad)]
            )
            tip = (tip[0] + pad, tip[1] + pad)
            head = (head[0] + pad, head[1] + pad)
        # The halo is what separates the pin from the map: the shape's own outline is
        # black and lands straight on top of road lines otherwise. Same job the
        # position marker's white halo does (MapActivity::drawPositionMarker()).
        mask = dilate(silhouette(grid), pad)
        # Clear radius only, from the old per-frame scan -- only step 0's is ever
        # used (for the glyph size), and changing that number is not part of this
        # fix.
        _, _, clear = head_circle(grid, mask)
        return mask, grid, tip, head, clear

    frames = []
    clear0 = 0
    for step in range(args.steps):
        angle = 360.0 * step / args.steps
        mask, ink, tip, head, clear = bake(angle)
        if step == 0:
            clear0 = clear
        frames.append((mask, ink, tip, head))
        print(
            f"step {step:2d} ({angle:5.1f} deg): {len(ink[0])}x{len(ink)}, "
            f"tip ({tip[0]:.1f},{tip[1]:.1f}), head ({head[0]:.1f},{head[1]:.1f})"
        )

    glyph = args.glyph or (int((clear0 * 2) / (2 ** 0.5)) & ~1)
    print(f"glyph {glyph}px, head clear radius {clear0} upright")

    lines = [
        "#pragma once",
        "",
        "#include <cstdint>",
        "",
        "// Generated by scripts/gen_pin_icons.py from src/components/icons/pin-shape.svg",
        "// plus the glyphs in src/components/icons/pins.icons.txt. Do not edit.",
        "//",
        "// One frame per rotation of the *shape*, clockwise from point-down. The glyph is",
        "// NOT rotated with it -- a numeral or a P has to stay readable -- so each frame",
        "// carries where its head's centre ended up and the device draws the glyph there,",
        "// upright.",
        "//",
        "// Every array is 1bpp MSB-first with bit 0 = draw, stride (w + 7) / 8:",
        "//   mask -- the silhouette plus its halo, painted white first so neither the map",
        "//           nor a road line shows through the pin",
        "//   ink  -- the outline",
        "//",
        "// `tipX`/`tipY` locate the point inside the frame: draw at (x - tipX, y - tipY)",
        "// and the point lands exactly on the coordinate the pin means.",
        "",
        f"inline constexpr int kPinShapeSteps = {args.steps};",
        f"inline constexpr int kPinGlyphPx = {glyph};",
        "",
        "struct PinShapeFrame {",
        "  const uint8_t* mask;",
        "  const uint8_t* ink;",
        "  int16_t w;",
        "  int16_t h;",
        "  int16_t tipX;",
        "  int16_t tipY;",
        "  int16_t headX;",
        "  int16_t headY;",
        "};",
        "",
    ]

    for step, (mask, ink, tip, head) in enumerate(frames):
        lines.append(carray(f"kPinShapeMask{step}Bits", pack(mask)))
        lines.append(carray(f"kPinShapeInk{step}Bits", pack(ink)))
    lines.append("")
    lines.append("inline constexpr PinShapeFrame kPinShapeFrames[kPinShapeSteps] = {")
    for step, (mask, ink, tip, head) in enumerate(frames):
        lines.append(
            f"    {{kPinShapeMask{step}Bits, kPinShapeInk{step}Bits, {len(ink[0])}, {len(ink)}, "
            f"{round(tip[0])}, {round(tip[1])}, {round(head[0])}, {round(head[1])}}},"
        )
    lines.append("};")
    lines.append("")

    for alias, source in parse_manifest(MANIFEST):
        if source.startswith("digit:") or source.startswith("text:"):
            # 2px smaller than the Lucide glyphs share: at full size next to a
            # thin-stroke icon a numeral reads noticeably heavier/bolder even
            # in a regular weight (reported 2026-08-24).
            gi = text_grid(source.split(":", 1)[1], glyph, fit=glyph - 2)
        else:
            svg = os.path.join(LUCIDE_DIR, source + ".svg")
            if not os.path.exists(svg):
                sys.exit(f"ERROR: missing svg for '{alias}': {svg}")
            gi = ink_grid(rasterize(svg, glyph, glyph))
        ident = "".join(part.capitalize() for part in re.split(r"[^A-Za-z0-9]", alias) if part)
        lines.append(f"// {alias}  ({source})")
        lines.append(carray(f"kPinGlyph{ident}Bits", pack(gi)))
        lines.append("")

    open(OUT, "w").write("\n".join(lines))
    print(f"wrote {OUT}")


if __name__ == "__main__":
    main()
