# Place labels

City, town, village and city-district names on the map. Added 2026-08-12.

Before this, the renderer drew a dot per place and no name at all
(`MapRenderer.cpp` place walk) because `IMapCanvas` had no text primitive. The
plan this executes is `docs/place-labels-plan.md` in the parent repo.

Read with `docs/map-render-spec.md` (parent repo) item 5, which is the contract.

## What is drawn

- A name beside its dot, in one of two tiers:
  - **major** -- place rank 0-1, city and town: bigger, bold.
  - **minor** -- rank 2 and up, village/suburb/hamlet/farm: smaller, regular.
  The rank comes from the tile (`MapPlaceRef::rank`,
  `mapbuilder/tilegen/build_config.json`'s `place_ranks`: city 0, town 1, village
  2, hamlet/suburb 3, farm 4). Rank cutoffs per zoom are a build-time decision
  (`place_max_rank` per LOD) -- there is no zoom-to-rank table in the firmware.
- Names in their own Title Case. Not all-caps: at a glance, mixed case is the
  faster read, and a name is not a heading.
- Legibility over the map comes from a **white halo** around the glyphs
  (`label_halo_px`, default 2). The older opaque white box is still there
  (`label_bg`), and is stronger, but it blanks a rectangle of map per label. The
  halo only knocks out the pixels the letters need, which is what makes a name
  compact enough to draw several of.
- The dot is always drawn. A place whose label was dropped still shows as a dot.

## Greedy, not a zoom table

`MapLabels::offer()` is called for every place in the frame during the walk that
draws the dots (`MapRenderer.cpp`), keeping the best twelve candidates by
(rank, then distance to the viewport anchor). Nothing is drawn during that walk:
placement has to know where the earlier labels went, and that is not known until
the layer has been walked to the end.

`MapLabels::draw()` then walks the candidates in that order and, for each, tries
eight positions around the dot -- right, left, below, above, then the four
diagonals -- taking the first that passes every test:

1. the label's box, inflated by the halo or the box padding, is fully inside the
   canvas's drawable rect (on the device that excludes the header band --
   `docs/map-header-status.md`);
2. inflated further by `min_label_gap_px`, it touches no label already placed and
   not the position puck;
3. no more than `max_route_overlap_pct` of it sits over the route.

A candidate that fails all eight is dropped, and its dot stands alone.
`max_labels` caps the count on top of that -- a render-cost backstop, not the
declutter mechanism.

Measured on the Zahorie route overview (`map_preview --route
build/trips/zahorie-male-karpaty-loop.tir --fit-route`): with only the four
cardinal positions, 2 of 6 on-screen names were placed and 4 dropped; adding the
diagonals took it to 4 placed, 2 dropped. That is why the diagonals exist.

## Why the route is the one thing labels avoid

Roads and buildings are *not* tracked. A name is meant to sit over the map --
that is what the halo is for -- and tracking road pixels would cost a second
pass over the roads layer off the SD card to make a decision that does not need
it.

The route is different: it is the line the rider is following, so a name across
it is a name in the way. It is tracked in a coarse occupancy grid
(`MapOccupancyGrid`, one bit per 8x8 px cell, 1,250 bytes), marked by the same
pass that *draws* the route -- because a second walk over the route means
re-reading its file (`IMapRouteSource.h`).

`max_route_overlap_pct` is 8 rather than 0 deliberately. Forbidding any overlap
loses names along the road being ridden, which is exactly where they are wanted;
a few per cent lets a label sit next to the line without crossing it.

## Layout is anchored, not marker-relative

Placement measures from `MapStyle::markerXPx/markerYPx` (the viewport anchor),
never from the live marker position. The marker slides between redraws while the
map underneath stays byte-identical (`docs/map-render-spec.md`), and layout off
the live position would mean a full redraw per GPS fix.

## The font

`IMapCanvas` gained `measureText`/`drawText`, and the two implementations differ:

- **Device** -- `GfxRendererCanvas`, forwarding to `GfxRenderer::drawText` with a
  built-in face.
- **Host preview** -- `test/map_preview/PreviewFont.cpp`: the same built-in font
  tables, measured with the firmware's own `EpdFont`, glyphs inflated with zlib
  instead of uzlib. So a label that fits in the preview fits on the panel.

The style asks for a **line height in device pixels** and the canvas picks the
largest face that does not exceed it. This matters because a built-in font's name
is its point size at 150 DPI, not its pixel height
(`lib/EpdFont/scripts/fontconvert.py`: `ppem = size * 150 / 72`), so
`notosans_12` is a 34 px line. Measured line heights of the faces in flash:

| face | line height | ascender | weights |
|---|---|---|---|
| `notosans_8` | 23 | 18 | regular only |
| `ubuntu_10` | 24 | 20 | regular, bold |
| `ubuntu_12` | 29 | 24 | regular, bold |
| `notosans_12` | 34 | 27 | regular, bold |
| `notosans_14` | 40 | 32 | regular, bold |
| `notosans_16` | 45 | 36 | regular, bold |
| `notosans_18` | 51 | 41 | regular, bold |

So today's labels come out as **Ubuntu UI bold 29 px** (major) and **Ubuntu UI
regular 24 px** (minor): sans-serif, proportional, both weights present, and the
only faces in flash small enough for a compact label. The smallest Noto Sans
built in is a 34 px line -- too tall to fit several names on a 480x800 screen.

**Open.** A purpose-built map face (Source Sans 3 or a small Noto Sans cut, at
real pixel sizes, Latin plus the Central European diacritics) would be the better
answer and needs no code change beyond adding it to the candidate list --
`GfxRendererCanvas::kLabelFontIds` and `PreviewFont`'s `kFaces`. Cost is flash
(each face is tens of KB) and a run of
`lib/EpdFont/scripts/convert-builtin-fonts.sh` plus `build-font-ids.sh` (which
needs `ruby`, absent on the current build host). Noto Sans TTFs are already in
`lib/EpdFont/builtinFonts/source/NotoSans`; Source Sans 3 is not.

## RAM

`MapLabelScratch` is ~3.2 KB: two occupancy grids (route, and what is already
taken) at 1,250 bytes each, plus twelve 40-byte candidates. Allocated once in
`MapActivity::onEnter()` and only when the compiled style draws labels; freed in
`onExit()`. On OOM the map keeps its dots and loses the names. `MapRenderer`
holds no state of its own, which is why the caller owns this
(`MapRenderer.h`).

## Cost

The halo is the same string re-drawn eight times per ring radius (so 16 extra
passes at `label_halo_px` 2) before the black pass. `MapRenderTiming::labelsMs`
was added for exactly this question, and the map's render log line now prints it.

**Open -- needs measurement on the panel.** Nothing here has been on hardware
yet: the placement rules were verified in the host preview and by
`test/map_labels` (18 tests), and the render is 0 errors / 293 host tests green,
but the halo's real cost at 160 MHz and how the 2 px halo resolves on the glass
are unmeasured. If it proves expensive, the cheap knobs in order are
`label_halo_px` 1, then `label_bg` true (one rectangle instead of 16 text
passes).

## Style fields

All in `data/mapstyle.json`, `layers.places`, compiled by
`scripts/gen_mapstyle.py`:

| field | default | meaning |
|---|---|---|
| `label_px` | 29 | major tier line height, device px; 0 hides that tier |
| `label_bold` | true | major tier weight |
| `label_minor_px` | 24 | minor tier line height |
| `label_minor_bold` | false | minor tier weight |
| `label_offset_px` | 6 | gap between dot edge and label box |
| `label_halo_px` | 2 | white outline width around the glyphs; 0 for none |
| `label_bg` | false | opaque white box instead of the halo |
| `label_bg_pad_px` | 3 | box padding |
| `label_bg_border_px` | 1 | box border |
| `max_labels` | 6 | hard cap per frame |
| `min_label_gap_px` | 4 | minimum space between two label boxes |
| `max_route_overlap_pct` | 8 | how much of a label may sit over the route |
| `label_max_width_px` | 170 | width cap; longer names are cut with U+2026 |

Edit them in mapbuilder's webapp, never by hand (parent `CLAUDE.md`).

## What is not built

- **Off-screen place chevrons** (`docs/map-render-spec.md` item 6) -- still not
  drawn. The style has had `offscreen_places` for a long time; this change did
  not touch it. A place whose dot is off screen is skipped here and is not
  counted as a dropped label.
- **Labels avoiding other places' dots.** A label box can cover a neighbouring
  dot. Rare at the current label counts, and cheap to add to the `taken` grid if
  it shows up.
