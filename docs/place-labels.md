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
draws the dots (`MapRenderer.cpp`), keeping the best 32 candidates by
(rank, then distance to the viewport anchor). Nothing is drawn during that walk:
placement has to know where the earlier labels went, and that is not known until
the layer has been walked to the end.

`MapLabels::draw()` then walks the candidates in that order and, for each, tries
eight positions around the dot -- right, left, below, above, then the four
diagonals -- taking the first that passes every test:

1. the label's box, inflated by the halo or the box padding, is fully inside the
   canvas's drawable rect. On the device that excludes three bands of screen
   furniture drawn *after* the map: the header (`docs/map-header-status.md`), the
   button-hint row along the bottom and the side-hint column on the right.
   Geometry may run under those and be painted over -- a road that continues off
   the panel still reads as a road. A name with half its letters painted over
   reads as a different name, so the placer stays out
   (`GfxRendererCanvas`'s `bottomReservedPx` / `rightReservedPx`, which shrink
   `drawableRect()` and nothing else);
2. inflated further by `min_label_gap_px`, it touches no label already placed and
   not the position puck;
3. no more than `max_route_overlap_pct` of it sits over the route.

A candidate that fails all eight is dropped, and its dot stands alone.
`max_labels` caps the count on top of that -- a render-cost backstop, not the
declutter mechanism.

Two caps, not one. `max_labels` in the style is the affordability ceiling; the
zoom rung carries its own (`MapViewport::ZoomStep::maxLabels`: 3, 4, 6, 8, 10,
12, 14 from rung 0 out) and the smaller wins. A rung knows how much ground is on
the panel, and that is what decides how many names it deserves -- twelve names
for the 480 x 800 m rung 0 shows is twelve names for one village, while twelve at
rung 6's 24 x 40 km is a map of the region. `docs/zoom-rungs.md` has the numbers
next to the other per-rung ones.

**The candidate cap is not a cosmetic number.** It was 12, and the first panel
run showed why that is wrong: the set is kept nearest-first, so at rung 6 with 88
places in range the twelve nearest all sat around the marker and the top half of
the screen was never considered for a name at all -- the label count looked
sensible while half the map went unnamed. It is 32 now
(`MapLabelScratch::kMaxCandidates`), which is past the number of places that can
share a screen rather than past the number of labels that fit, and `max_labels`
is 14. Same scene then names 13 places spread over the whole frame.

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

### The candidate list is mostly not in the build

`OMIT_FONTS` (`platformio.ini`) drops the four Noto Sans families from the
default build -- the map is the only screen left that draws text, and it draws
with UI_10/UI_12/SMALL. Four of the seven ids in
`GfxRendererCanvas::kLabelFontIds` are therefore never registered.

That was harmless for the picture and not harmless for the log.
`GfxRenderer::getLineHeight()` answers an unknown id with
`LOG_ERR("GFX", "Font %d not found")` and a 0, the picker walked the whole list
on every call, and a haloed label is 17 `drawText` calls -- so each label emitted
dozens of error lines over USB. **Measured on hardware 2026-08-13**, reported as
"desiatky riadkov".

Fixed by asking `GfxRenderer::getFontMap()` whether a face is registered before
asking its height (silent), and by memoising the last size-to-id answer, since
those 17 calls all ask for the same size. **No pixel changes**: an unregistered
face already scored 0 and was already skipped, so the face chosen was the same
before and after -- only the noise and 7 map lookups per call are gone.

So today's labels come out as **Ubuntu UI bold 29 px** (major) and **Ubuntu UI
regular 24 px** (minor): sans-serif, proportional, both weights present, and the
only faces in flash small enough for a compact label. The smallest Noto Sans
built in is a 34 px line -- too tall to fit several names on a 480x800 screen.

### Why 29/24 with a 2 px halo

Judged in the host preview on two scenes (rung 6 over Modra/Pezinok, 108 places
in range; rung 2 over Pezinok town), 2026-08-12:

| variant | verdict |
|---|---|
| **29 bold / 24 regular, halo 2** | chosen. Hierarchy reads at a glance, halo separates the name from roads and the built-up stipple |
| 29 / 24, halo 1 | halo too thin -- glyph stems touch the road lines under them |
| 24 bold / 24 regular, halo 2 | fits one name more (6 of 7 vs 5), but hierarchy is weight-only and much weaker |
| 29 / 29, halo 2 | both tiers big; crowds the screen for no gain |

One trap in that table: a minor tier below 24 px drops to `notosans_8`, so the
two tiers would come from **different type families** on the same map. Keep both
tiers on faces of one family -- 24 and 29 are both Ubuntu UI.

**Judged good enough on the panel 2026-08-12** -- the maintainer's call after
looking at the twelve-name rung 6 frame. A purpose-built map face (Source Sans 3
or a small Noto Sans cut) is still the better answer on shape grounds and is
written up as a plan rather than queued work: parent repo's
`docs/place-labels-plan.md`, "Follow-up plan: a map-specific type face". It needs
no code change beyond one row each in `GfxRendererCanvas::kLabelFontIds` and
`PreviewFont`'s `kFaces` -- the constraint to carry over is that both tiers must
come from one family.

## RAM

`MapLabelScratch` is ~3.8 KB: two occupancy grids (route, and what is already
taken) at 1,250 bytes each, plus 32 candidates of 40 bytes. Allocated once in
`MapActivity::onEnter()` and only when the compiled style draws labels; freed in
`onExit()`. On OOM the map keeps its dots and loses the names. `MapRenderer`
holds no state of its own, which is why the caller owns this
(`MapRenderer.h`).

## Cost -- measured on the panel

The halo is the same string re-drawn eight times per ring radius (16 extra passes
at `label_halo_px` 2) before the black pass, so the question was whether that is
affordable at 160 MHz. `MapRenderTiming::labelsMs` answers it and is in the map's
render log line.

**Measured on hardware 2026-08-12** (X4, ride mode, Modra/Pezinok):

| rung | places in range | labels drawn | labelsMs | frame |
|---|---|---|---|---|
| 2 (12 m/px) | 5 | 2 | **15 ms** | 508 ms |
| 6 (45 m/px), caps of 12 | 88 | 5 | **94 ms** | 3,793 ms |
| 6 (45 m/px), caps of 32/14 | 88 | 12 | **163 ms** | 3,888 ms |

4.2 % of the frame at the widest rung with a full dozen names on it, and the
frame itself grew 95 ms -- the cost scales with labels drawn, as the halo says it
should. The halo is not the expensive thing on
this screen -- landuse (1,826 ms) and roads (1,440 ms) are. If it ever needs to
go, the knobs in order are `label_halo_px` 1, then `label_bg` true (one rectangle
instead of 16 text passes).

Also confirmed on the glass: the 2 px halo separates a name from road casings,
the forest hatch and the built-up stipple, the 29/24 tier split reads as a
hierarchy at arm's length, and with the raised caps rung 6 carries twelve names
spread over the panel with none of them under the button row or the side hints
(parent repo's `docs/device-shots/place-labels-rung6-modra-20260812.png`).

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
| `max_labels` | 14 | affordability cap per frame; the zoom rung's own ceiling caps it again |
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
