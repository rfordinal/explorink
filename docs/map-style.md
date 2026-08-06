# Map style — how mapstyle.json reaches the renderer

`data/mapstyle.json` is the map's style. The device never reads it. It is a
build-time input, compiled into the firmware by a generator, exactly like the
i18n string tables.

Verified by reading this tree on 2026-08-05 (branch `feat/mapstyle-to-renderer`,
merged to `develop` the same day).

> **Optimisation review, 2026-08-06.** The renderer this style feeds is the
> device's slowest path.
> [`optimization/01-render-pipeline.md`](optimization/01-render-pipeline.md) has
> the costs and the order to fix them in — fixed-point projection, screen-clamped
> area scans, one clip per road instead of one per stroke copy. A style change
> that adds an area layer or a wider road pays into all three, so read it before
> planning one.

**Confirmed on the panel, 2026-08-05**, with framebuffers pulled off the device
over `CMD:SCREENSHOT` (`docs/device-shots/malacky-landuse-z*.png` in the parent
repo, captured by `tools/device_map_shot.py`). Buildings with outlines and a
stipple interior, forest as a diagonal hatch, built-up areas as a tone, per-class
road widths, the marker and the compass — all drawn on the glass, at zoom steps
0, 1 and 2. Nothing about the tones or the widths needed changing after seeing
them.

**And the on-device timings, which the laptop could not answer**, read off the
map screen's own debug line:

| zoom step | LOD | m/px | bytes read | viewport reset |
|---|---|---|---|---|
| 0 | z13 | 1.0 | 484 KB | 3240 ms |
| 1 | z13 | 3.0 | 484 KB | 2506 ms |
| 2 | z12 | 6.0 | 198 KB | 1088 ms |

Two things fall out of that. Buildings at the detail LOD cost roughly 1.4 s more
per viewport reset than the regional view without them — real but affordable for
a screen that redraws on demand. And z0 and z1 read **the same bytes** yet differ
by 700 ms, so the extra time is drawing, not I/O: at 1 m/px the same geometry
covers far more pixels.

## The path

```
data/mapstyle.json
  -> scripts/gen_mapstyle.py            (pre: build step, platformio.ini)
  -> src/activities/map/MapStyleDefaults.h   (gitignored, constexpr MapStyle)
  -> MapRenderer::render(canvas, source, state, style)
```

`MapStyle` (`src/activities/map/MapStyle.h`) carries what the renderer draws
with:

| field | from | used at |
|---|---|---|
| `roadWidthPx[32]` | `layers.roads.rules[].width`, per `class_id` | `MapRenderer.cpp`, first road pass |
| `roadCasingPx[32]` | `layers.roads.rules[].casing_px` | `MapRenderer.cpp`, second road pass |
| `placeDotDiameterPx` | `layers.places.dot_radius_px`, doubled | `MapRenderer.cpp`, places pass |
| `markerXPx`, `markerYPx` | `device.marker_x_px` / `marker_y_px` | `MapViewport.h:51-52` |
| `puckRadiusPx`, `puckRingPx`, `puckArrowPx` | `layers.position` | `MapRenderer::drawMarker` |
| `buildingsEnabled`, `buildingOutlinePx`, `buildingHatch`, `buildingHatchSpacingPx` | `layers.buildings` | `MapRenderer.cpp`, buildings pass |
| `waterEnabled`, `waterLinePx`, `waterHatch`, `waterHatchSpacingPx` | `layers.water` | `MapRenderer.cpp`, water pass |

`arrow_px` is the arrow's tip-to-tail length. The tail sits a quarter of it
behind the anchor and the base is half of it wide, so the style's 28 px draws the
same triangle this renderer drew before the puck existed.

`MapActivity.cpp:419` passes `kDefaultMapStyle`. Nothing overrides it at
runtime; there is no style file on the card and no setting for any of this.

## Why a struct and not free constants

`MapRenderer` is also compiled into the laptop-side preview
(`test/map_preview/`, and `pio run -t map-preview`). Passing the style as an
argument is what lets that preview render with the same numbers the firmware
compiled, and lets the golden fixture pin its own frozen style so a style edit
cannot break it (`test/map_tile_reader/MapTileReaderGoldenTest.cpp`).

The laptop preview is the point of all this: editing the style in mapbuilder's
webapp and looking at the result takes about two seconds, against a firmware
build plus a flash. The mechanism is documented in the parent `xteink` repo,
`docs/device-preview.md`.

## Two road passes, and why the order is not a preference

A cased road is a black stroke at the full width with a white stroke
`2 * casing_px` narrower inside it: two black edges, road left white between
them. That is how a main road reads as bigger than a side street with no colour
to spend.

**Every black stroke is drawn before any white fill.** `MapRenderer::render`
walks the road layer twice for this (`MapRenderer::kRoadPasses`). Finishing one
road completely before starting the next would let the later road's white fill
punch a hole in the earlier road's black edge, leaving a broken casing at every
junction. `IMapSource::beginWays()` is rewindable precisely so this costs a
second seek over a ~20 KB layer instead of a buffer holding the whole layer
(`IMapSource.h:22-27`).

The second walk means `MapTileSource::waysEmitted()` reports the way count
`kRoadPasses` times over. That is the counter working as documented; a caller
that wants "ways in the picture" divides by it.

## GfxRenderer's thick line is not usable for a map

**Found 2026-08-05, read off the code, not yet confirmed on the panel.**
`GfxRenderer::drawLine(x1, y1, x2, y2, lineWidth, state)` draws `lineWidth`
copies of the line offset **downward in y only** (`GfxRenderer.cpp:713-717`).

For the UI's horizontal rules that is fine. For roads it is not: a north-south
road's copies land on top of each other, so it stays one pixel wide however wide
the style says it is, while an east-west road of the same class comes out full
width. Per-class widths would have been visible on half the compass and
invisible on the other half.

`MapStroke::stackFor` (`src/activities/map/MapStroke.h`) decomposes a thick map
line into one-pixel lines instead. Both `IMapCanvas` implementations use it,
`GfxRendererCanvas` on the device and `PpmCanvas` in the laptop preview, so a
wide road comes out the same width in the same place on both.

**It stacks along the dominant axis, not along the perpendicular, and that is
the whole trick.** Offsetting along the true perpendicular is the obvious
approach and it stripes: on a diagonal the perpendicular is diagonal too, so
consecutive copies land 1.41 px apart and the road draws as parallel hairlines
with white between them. Seen on a rendered map, 2026-08-05, before the fix.
Stacking along the axis the line moves fastest in cannot leave a gap, because
consecutive copies are exactly one pixel apart there and Bresenham fills every
step of the other axis. The copy count is scaled by `len / major` so the
perpendicular thickness still comes out at the style's width -- a 45-degree road
needs 12 copies to look as wide as a horizontal one does with 8.

A 1 px line is one Bresenham line at every angle, no scaling. "1 px" in a style
file means one pixel, and the golden fixture depends on it.

`test/map_stroke/MapStrokeTest.cpp` guards both failure modes directly: ink area
per bearing (catches the thin-road one) and longest interior white run per row
and column (catches the striping one). Neither is visible to a test that only
asserts something got drawn.

`GfxRenderer` itself is untouched. It is inherited code and the UI depends on its
current behaviour (this repo's CLAUDE.md, "Treat inherited code as upstream's").
Nothing else in the map path calls its thick-line overload.

Open: how a wide road's outer corner looks on the panel at a sharp polyline
bend. The preview shows no gap at the widths this style uses (up to 10 px), and
the joint is not mitred, so a very sharp bend may show a notch. The 2026-08-05
hardware run did not look for it specifically, so it stays open.

## Rounding and the two zeros

Style lengths are floats; `IMapCanvas::drawLine` takes an `int` width. The
generator rounds half up, and **floors a visible class at 1 px** — a width typo
must not silently delete a road class. `layers.roads.rules[].hidden: true`
compiles to width 0 instead, and `MapRenderer::strokeWay` skips those.

Two separate zeros, worth not confusing:

- **width 0** — this class is never drawn, in any travel mode. From `hidden`.
- **not in the mode mask** — dropped earlier, in `MapTileSource`, and only for
  the current travel mode. From `modes` and `MapModeMaskDefaults.h`.

## What is still not from the style

The missing-tile hatch spacing (`MapHatch.h`) is `constexpr`. It is a
diagnostic, not part of the map's look.

Labels, the route, junction dots, water and buildings are specified in the style
file and drawn by nothing. Labels are blocked on a canvas primitive: `IMapCanvas`
has three drawing calls and none of them is text, and the label layout rule needs
a measured text width rather than an estimate. The route and its junction dots
are blocked on data, since no route reaches the device yet. The parent repo's
`docs/mapstyle.json.md` marks every field implemented or not.

Three style zeros disable rather than shrink: road width 0 (`hidden`),
`placeDotDiameterPx` 0 (places layer off), `puckRadiusPx` 0 (arrow with no disc).
The last is the golden fixture's setting, not a shape the spec asks for.

## Buildings and water

Both layers live in the tile as **way records**, the same shape as roads
(`docs/map-data-spec.md`, "Layer ids": `1` water, `2` buildings). `IMapSource`
hands them out as `MapWayRef` through `beginBuildings()`/`nextBuilding()` and
`beginWater()`/`nextWater()`. Two differences from a road matter:

- **`classId` is always 0.** The tile format carries no building or water class.
  So `mapstyle.json`'s river/stream/canal rules cannot be told apart on the
  device, and `layers.water.default.width` is the one width every waterway gets.
  Widening rivers alone needs a tile format change, not a style change.
- **An area is a closed ring** -- first point repeated as last
  (`mapWayIsClosedRing`, `IMapSource.h`). Buildings are always areas; a water
  record that is not closed is a waterway line.

Neither layer sees the mode mask. A lake is not a road class.

### Tone is what a built-up area wants, not hatch

A building at map zoom is five to ten pixels across. That is far too small to
carry a line pattern: a cross hatch at 4 px leaves a couple of ticks per house
and a village reads as dirt on the screen. Judged on rendered output 2026-08-05,
against a reference map the maintainer supplied.

The answer on 1-bit is a **tone**: a screen-space pixel pattern with a period of
two or three, which reads as flat grey from arm's length. `MapAreaTone`
(`src/activities/map/MapAreaTone.h`) has four:

| tone | pattern | reads as |
|---|---|---|
| `Stipple` | 1 px in 9 (`x % 3 == 0 && y % 3 == 0`) | a fine dotted texture |
| `Light` | 1 px in 4 | light grey |
| `Dark` | 1 px in 2, checkerboard | mid grey |
| `Solid` | every pixel | black, for shapes too small to texture |

`Light` and `Dark` are **GfxRenderer's own dither patterns**, not new ones
(`GfxRenderer.cpp`, `drawPixelDither<Color::LightGray>` and `<Color::DarkGray>`).
That is deliberate twice over: the device paints those with `fillRectDither`, one
call per span instead of one per pixel, and the two canvases cannot disagree
about the pattern's phase. `MapTone::inkAt` mirrors the same formulas for the
laptop preview -- if those specialisations ever change, change it with them.

**The pattern is anchored to screen coordinates, never to the shape.** That is
what makes two buildings a metre apart share one texture instead of each starting
its own, which is the difference between a village reading as built-up and as
noise. It also means a fill split into different spans cannot shift.

Drawn through one new canvas primitive, `IMapCanvas::fillSpan(x1, x2, y, tone)` --
a span rather than a pixel so the device can hand a whole run to its dithered
fill, and a span rather than a rectangle because the shapes are arbitrary rings.
`MapAreaFill::toneRing` pairs scan-line crossings exactly as the hatch does, so a
courtyard stays white.

**Grey is coming.** A separate branch is adding the panel's real grey levels. When
it lands, a tone should map to one of those instead of to a dither pattern; the
swap belongs in the two `IMapCanvas` implementations, behind `MapAreaTone`, and
nothing above that line needs to know which it got.

### Hatch, kept for large areas

A solid black building on 1-bit swallows the roads around it and reads as a hole
in the map, so an area is drawn as an outline plus a tone or a hatch
(`docs/map-render-spec.md`). The hatch is still there because one big area -- a
lake, a forest -- is large enough for lines to read as a pattern rather than as
scratches. `fill` in the style picks which: `tone` or `hatch`.

`MapAreaFill` (`src/activities/map/MapAreaFill.{h,cpp}`) draws that hatch as
**hatch lines clipped to the ring**, using `IMapCanvas::drawLine` only. No new
canvas primitive: the device gets it without the scanline-fill callback
`GfxRenderer` does not have, and the laptop preview gets the same pixels because
both run this code.

The pattern comes from `mapstyle.json`'s matplotlib hatch string, first character
only: `/` diagonal, `\` antidiagonal, `-` horizontal, `|` vertical, `X`/`x`/`+`
cross. The repeat count in `"XXXX"` is a matplotlib density knob and means
nothing here -- density is `hatch_spacing_px`, in device pixels like every other
length. So that field, which used to be read by nothing, is now the one that
matters and the string's length is the one that does not.

Two implementation notes worth not undoing:

- Hatch lines are anchored to a multiple of the spacing **in screen space**, not
  to each ring's own bounding box. A row of houses then shares one hatch grid and
  reads as a block instead of as noise.
- Crossings are paired, so a concave (L-shaped) building leaves its notch white.
  `test/map_area_fill/` checks that no ink lands outside the ring for every
  pattern, against an independent even-odd point-in-polygon oracle. A leaked
  hatch line looks like a stray road, not like a fill bug, which is why that test
  measures ink position rather than ink existence.

### Turning buildings on costs SD reads, not pixels

Measured on the laptop preview at the reference view
(48.446967, 16.988511, W, zoom step 2, ride -- `docs/visual-refs.json` in the
parent repo), 2026-08-05:

| style | ways drawn | bytes read |
|---|---|---|
| buildings off | 2249 | 181 KB |
| buildings on | 6571 | 789 KB |
| buildings + water on | 6760 | 822 KB |

4.4x the bytes off the card for one viewport reset. That is why
`buildingsEnabled` and `waterEnabled` gate the **read**, not the draw: a
disabled layer is never opened. Render time barely moves (11-17 ms on the
laptop), so on the device this is an SD I/O decision, not a drawing one.

**Answered on hardware 2026-08-05**, see the timing table at the top of this
document: 484 KB with buildings at the detail LOD is a 2.5-3.2 s viewport reset
against 1.1 s for the 198 KB regional view. Buildings are also no longer written
at the coarse LODs at all (`mapbuilder/build_config.json`), so the expensive case
only arises at detail zoom, where the wait is expected anyway.

## Landuse: forest and built-up areas

Landed 2026-08-05, both sides. The tile layer is `layer_id 6`, way records like
buildings and water, and it carries **two classes in one layer**:
`MapLanduseClass::Forest = 1`, `BuiltUp = 2` (`MapAreaClass.h`, mirroring
mapbuilder's `landuse_class.py`).

**The renderer walks that layer twice, once per class**, because the two are
drawn at different depths and a single walk would emit them in whatever order
the tile stored them. A park inside a housing estate has to land on top of it.
Same shape as the two road passes, and the same rewindable `begin*()` makes it
cost a second seek rather than a buffer.

Full draw order, fixed (`docs/map-data-spec.md`, "A tile is a storage unit, not a
render unit"):

```
built-up area -> forest -> water -> buildings -> road casings -> road fills -> places
```

Landuse is background. Everything above it has to stay readable over it, which is
why a tone is a sparse pattern rather than a grey block.

`MapStyle` carries per-class outline, tone, hatch and hatch spacing indexed by
`MapLanduseClass`, so the two classes are independent. **Two adjacent layers must
not share a tone** -- their boundary disappears, and that boundary is often the
thing being navigated by. The committed style uses stipple for built-up and a
diagonal hatch for forest, which is large enough to carry lines.

A class whose rule draws nothing at all is a build error, not a silent skip:
an enabled layer is read off the card in full, so a rule with no fill and no
outline is pure I/O cost. Drop the rule or disable the layer.

## Water has classes now, so a river is not a ditch

Also 2026-08-05. Every water record used to carry `class_id 0`, which made
`mapstyle.json`'s river/stream/canal rules undeliverable -- documented here as a
dead end for exactly one day. The builder now writes `MapWaterClass`:
`Unknown = 0`, `River = 1`, `Stream = 2` (canal and drain too), `Lake = 3`.

`MapStyle::waterLinePx` is a table indexed by that class. `Unknown` is a real
value with a real width -- a ditch or a weir still gets drawn, undifferentiated,
the same way an unrecognised `highway` lands on the road enum's `unknown`.

Only `Lake` arrives as a closed ring, so it is the only class whose rule needs a
fill. The style's water rules key on **class names**, not OSM tags: the tile
carries the class, and matching tags in the style would be a second vocabulary
nothing can check (`docs/map-data-spec.md`, "One vocabulary, not two"). The
laptop sketch resolves through the same class table, so both panels style a
river the same way.

## Adding the next area layer

The path is now well worn. For each new layer:

1. A `MapTileReader::Layer` id, and **`kMaxLayers` bumped to match**. That one is
   easy to miss and fails invisibly: `open()` rejects the tile, the source counts
   it unavailable, and the screen fills with hatch that looks exactly like "no
   map data here". It happened with landuse.
2. A class enum in `MapAreaClass.h` if the layer has more than one kind of thing,
   mirroring the builder's module.
3. A pass pair on `IMapSource` and `MapTileSource` -- about ten lines, the shape
   of `beginBuildings()`/`nextBuilding()`.
4. A `MapStyle` block, per class where it matters, plus the `gen_mapstyle.py`
   mapping and a `mapstyle.json` block whose rules match on class names.
5. One call in `MapRenderer::render`, in draw order, guarded by the style's
   enable flag so a layer nobody draws is never read.

## Two marker paths, and they disagree

**Open, 2026-08-05.** The position marker is drawn twice over, by two different
pieces of code:

- **On the device:** `MapActivity::drawPositionMarker` — mode-aware (hike a dot,
  cycle a small arrow, ride a bigger one), drawn straight onto `GfxRenderer`,
  using `kMarker*` constants and none of the style.
- **In the laptop preview:** `MapRenderer::drawMarker` — the style's
  `layers.position` puck. `MapRenderer::render` draws no marker at all, so a
  caller picks one.

So the marker is the one thing on screen the preview cannot vouch for.

The reason `drawPositionMarker` bypasses `IMapCanvas` is gone: it needed a white
halo fill, and the canvas paints white now (`MapInk`). Moving it behind the canvas
would put the device's marker in the preview.

**Deliberately not being fixed right now** (maintainer, 2026-08-05: the marker in
the preview is not what he is looking at). Do not treat this as urgent work. Until
it is unified, the one rule that matters: do not read the preview's puck as the
marker the device draws.

Whenever it is picked up, it needs a style decision first. `mapstyle.json` has one
`layers.position` block and no per-mode marker sizes, so either the style grows
them (a `modes.<name>.marker` block) or one set of numbers is scaled per mode.

## Both generated headers are gitignored

`MapStyleDefaults.h` and `MapModeMaskDefaults.h` are build products, never
committed (`.gitignore`). PlatformIO regenerates them on every build; for the
host build, `test/CMakeLists.txt` runs the same two generators as CMake custom
commands. That second wiring is not cosmetic — before it existed a host build
reused whatever a previous firmware build had left in the source tree, so a
style edit did not reach the native preview at all.
