# Street names on the map

What the device does with tile layer 8. The data half, the measurements behind
the numbers and the build order are in the parent repo
(`docs/street-labels-plan.md`, `docs/map-data-spec.md`, `docs/tasks/T-558.md`).

Read with [`place-labels.md`](place-labels.md): a settlement name and a street
name are two different mechanisms, and the difference is the whole design.

## A street name lies on its street, a place name sits beside its dot

A place name is placed: eight candidate positions around a dot, the first that
does not collide wins. A street name is not placed at all. It goes where the
tile says, at the angle the tile says, and the only question is whether it fits.

That is the same thing the contour height numbers already do
(`MapRenderer.cpp`, `drawContourLabels`), and it is why street names reuse that
path rather than `MapLabels::offer`'s:

- `IMapCanvas::drawTextRotated` takes an orthonormal basis in 1/1024ths. An
  angle would mean a float and the ESP32-C3 has no FPU.
- The 1 px white outline comes off the same rotated glyph mask, so it follows
  the turn. Drawing it as eight offset copies would not.
- A knockout box is refused here even harder than for a height: the box would
  wipe the street the name is about.

**`MapLabelCandidate` is not touched.** No third tier, no third budget inside
`MapLabels::draw`, no second candidate array. Street names have their own eight
slots in `MapLabelScratch` and their own pass.

## Order is the declutter rule

```
MapLabels::draw          place names, landmark names
MapLabels::drawRoadLabels   street names        <- here
drawContourLabels        height numbers
```

Each pass claims its ground in `MapLabelScratch::taken` and the next one yields
to it. A settlement name beats a street name; a street name beats a height. A
street is countable from the next one along, a village's name is not.

## Two rules refuse a name before any cap does

**A name wider than its own straight street.** Each record carries `run_q`, the
straight run of street through its anchor in ground metres, measured at build
time. The renderer converts it to this rung's pixels and refuses a name that
does not fit inside it. Without it a long name is drawn along a short stub and
has to bend, which is what no paper map does -- and measured on Bratislava, the
bottom tenth of streets have 30 device px or less at rung 0, shorter than any
name. Those are exactly the little grid streets a reference map also leaves
blank.

**One name per street per panel.** The builder spaces anchors along a street so
that a panel anywhere on it catches one; a street running the length of the
screen catches two. The first real render put `Liscie udolie` on one panel
twice. The first offer wins rather than the best one: both are on the same panel
and differ only in which end of the same street they sit on.

A name whose turned box crosses the panel edge is refused too. A place name
clipped by the edge still reads, because the dot it belongs to is visible; half
a street name lying along a street that leaves the screen reads as a smudge.

## The rank is the road class, and that is enough

Records arrive most-important-first from the tile, and the offer merges the two
to four tiles a close rung reads into one order: road class, then the longer
straight run. No distance to the anchor, unlike a place -- a street name is not
pointing at something the rider is near, it is saying what the line under it is
called.

Measured against a published reference map on one panel of real ground, that
order picks the same streets the reference names and leaves the same residential
grid blank. There is no scoring function and none is wanted.

## The text mask was 80 px wide, and that silently cost five names in six

`MapTextMask` was sized for a five-digit contour height: 80 x 32. Five digits at
12 px are about 40 px. **"Karloveska" at 23 px is 83**, so the first real render
drew exactly one of six offered names -- the only one under 80 px -- and the
other five were refused inside the canvas, far from where they were offered and
with nothing in the render to say so.

It is 160 x 32 now, which covers every name the style's own ellipsis cap lets
through (`label_max_width_px` 150, plus the outline). **The cost is DRAM, not
stack**: `GfxRendererCanvas::drawTextRotated` holds one static pair for the life
of the firmware, deliberately. +640 bytes resident, read off the array
dimensions and **not** yet confirmed by two builds.

## What it costs to read

Measured 2026-11-01 on the host preview, one Bratislava z13 tile, the same frame
with the style's `max_labels` at 3 and at 0:

| | bytes read for the frame |
|---|---|
| names on | 292,509 |
| names off | 286,654 |

**5,855 bytes**, which is the 5,650-byte layer plus its directory and crc. With
the style off it is **zero**: `MapRenderer` does not call `beginRoadLabels()`
unless `roadLabelPx` and `roadLabelMax` are both non-zero, the same gate the
relief layer has. So hike, ride, and every rung past 1 pay nothing at all.

## Style

`layers.road_labels` in `data/mapstyle.json`. Off everywhere by default, on for
**tourist at rungs 0 and 1** only -- at 6 m/px a name is longer than the street
it names, and a rider is not reading street fabric.

`max_labels` is the real declutter mechanism here, unlike the places where it is
a cost cap: a city-centre z13 tile offers a measured median of thirteen names on
one rung-0 panel and the cap is three.

`label_px` is 23 and **a smaller size is not available**: the registered faces
have line heights 12, 23, 24 and 29, so the only step down is the 12 px
contour-number cut, whose letter spacing falls apart on a name. `label_halo_px`
is 2 rather than the contour numbers' 1, because a street name lies along a
cased road and 1 px of white let the casing run through the letters.

## Where this shape comes from

Line placement is one of cartography's solved problems, and our rules land on
the same shape independently. That is evidence rather than coincidence, and it
is worth saying out loud because nothing here was copied (label: **read**, from
each project's own documentation).

MapLibre GL's `symbol-placement: line` places labels along the geometry at a
`symbol-spacing` interval and drops a candidate whose `text-max-angle` is
exceeded. Mapnik's line placement does the same with its own spacing and
maximum-angle knobs. **Our anchor spacing and `max_bend_deg` are those two
knobs under other names**, and the refusal rule
(`fitted.width_() > slot.runPx`) is their "no room, no label".

**Where we diverge, and the constraint that explains it:** both of them place
at render time from the full geometry. We precompute anchors in tilegen and
ship them in layer 8, because the device has no FPU and cannot afford a
geometry pass per frame. So the run length a name must fit into is known before
the tile is written, and the device only compares two numbers.

No constraint explains a second difference, so there is not one to report: the
restraint (three names at rung 0, two at rung 1) is a style decision about a
480x800 e-ink panel, not a departure from how anybody else does it.

## Unjudged

Every choice above was made on the host preview at 1:1. **Nothing has run on a
panel.** Whether a 23 px name with a 2 px halo over a dense residential grid
reads in the hand is the question the hardware pass exists to answer, and it is
the one question the preview cannot.
