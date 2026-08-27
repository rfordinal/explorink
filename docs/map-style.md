# Map style — how mapstyle.json reaches the renderer

`data/mapstyle.json` is the map's style. The device never reads it. It is a
build-time input, compiled into the firmware by a generator, exactly like the
i18n string tables.

**The style is per travel mode and per zoom rung since 2026-08-25.** Any block in
the file may carry a `when` list, and the generator resolves it into one compiled
`MapStyle` per (mode, rung). The device still does no parsing -- it indexes a
table. [`map-style-variants.md`](map-style-variants.md) is that mechanism: the
grammar, the flash cost, and why the class filter is intersected with it.

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

## Line types beyond solid and dashed

**Why more than a dash.** On 1-bit there is no colour, so rhythm and shape are the
whole vocabulary for telling one line class from another -- and two of those
distinctions matter on a hike map. 28.7 % of the pedestrian ways around Vratna are
on a waymarked route and the rest are farm and forest tracks (measured 2026-08-27),
drawn as the same 1 px hairline. And a `natural=cliff` at 2 px was the same line as
an index contour, where one says "a hundred metres of height" and the other says
"you fall here".

Three patterns, all per class, all off unless a style asks:

```json
{ "pattern": "dash_mark", "mark": "comb", "mark_px": 7,
  "dash_px": 10, "gap_px": 10, "width": 1 }

{ "pattern": "hachured", "tick_px": 3, "gap_px": 8, "width": 2 }

{ "pattern": "none" }
```

- **`dash_mark`** -- dash, gap, mark, gap, repeating. Needs `mark`; takes
  `mark_px` (odd; an even size has no centre pixel so every stamp would lean the
  same way, and the generator rounds up).
- **`hachured`** -- the line stays whole and short combs hang off its right-hand
  side. `tick_px` is the reach, `gap_px` the spacing. On a road class `dash_px`
  carries the reach instead, because a hachured line has no dash of its own.
- **`none`** -- no line at all; see "A toned watercourse".

The marks: `dot`, `square`, `circle`, `diamond`, `cross`, `u`, `comb`.

**Rasterised and counted, because the silhouettes do not separate where you would
guess.** At 3 px a circle and a square are the *same nine pixels* -- there is no
room for a corner to be missing -- and a diamond is a plus of 5. At 5 px they are
25 / 21 / 13; at 7 px, 49 / 37 / 25. So below 5 px only `cross`, `diamond`, `u` and
`comb` say anything at all.

**Two of them turn, the rest do not, and the split is not arbitrary.** A square
rotated is the same square, and rotating it would put its edges on diagonals where
1-bit has no grey to soften a staircase -- the maintainer's own reason, 2026-08-27,
for keeping it axis-aligned. But `u` and `comb` are *statements about a direction*:
their floor faces the drop. So those two are turned, and **snapped to four
directions** rather than turned freely, which keeps every edge on the pixel grid
and still says which way the ground goes.

Where the direction comes from is data, not taste: OSM puts a cliff's lower ground
on the right of the way's direction of travel, and the tile keeps its points in OSM
order precisely so the mark can face the right way without a refetch
(`docs/map-data-spec.md`, "The relief layer's second class"). Get the sign wrong
and every rock face on the map claims the drop is uphill.

**`comb` is the cartographic one** -- a bar along the line with three teeth on the
drop side, the escarpment hachure every topographic sheet uses. `u` is the same
idea with two, lighter at small sizes. Reference: the Prosiecka dolina sheet.

**A relief class refuses `dashed` and `ticked`.** A broken contour reads as a
footpath and a cliff wants combs rather than a railway's sleepers, so the generator
refuses both rather than drawing a wrong mark.

**None of this has been on a panel.** Which mark survives on glass, at what size,
is exactly the question a host render cannot answer.

## A toned watercourse

**A closed ring is the only thing that made water a surface, and a stream is
never one.** `MapRenderer`'s water pass branches on `mapWayIsClosedRing(way)`:
a ring gets `toneRing`, then the wave hatch knocked out of it in white, then its
border. An open way got a stroke and nothing else. So the Danube reads as an
obstacle because OSM maps it as a polygon, while a stream -- always a
`waterway=*` line -- could only ever be a black hairline, and `fill: tone` on a
stream rule drew **nothing at all**: `toneRing` is reached only in the ring
branch. The maintainer asked for a stream that reads as an obstacle, 2026-08-27,
and width alone could not say it.

Since then an open way can carry a surface, by the same three steps a cased road
takes and deliberately not by a second mechanism:

```json
{ "match": {"class": ["stream"]},
  "pattern": "dashed", "width": 1,
  "when": [{ "steps": [0, 1, 2], "modes": ["hike"],
             "pattern": "solid", "width": 7, "casing_px": 0,
             "fill": "tone", "tone": "dark" }] }
```

**`casing_px` decides whether the ribbon has edges, and 0 is a real answer.**

- **`casing_px: 0` -- edgeless.** The tone is the whole stroke, full width, no
  black rim. Maintainer's call 2026-08-27: *"I want the interior to eat the edge
  too, I do not want a border."* What it buys is that the mark reads as a
  **surface** rather than as a channel with banks: a 7 px ribbon with 2 px of
  black each side is mostly rim, and the tone becomes a detail inside a border
  instead of the thing itself.
- **`casing_px > 0` -- edged.** The same three steps a cased road takes, and
  deliberately the same ones rather than a second way of saying it: black at the
  full width, white at the interior, then the tone laid into the cleared middle.

`toneWayInterior` does the tone in both cases -- the same primitive a cased road's
tone uses; the only difference is whether it is handed the full width or the
interior (`MapRenderer.cpp`, the `tone != MapAreaTone::None` branch of the water
pass).

Three more things before tuning it.

- **`pattern: "none"` is how you say "no line, only the tone".** It exists because
  `solid` had come to mean two things on a toned water class -- a solid stroke
  without a tone, nothing drawn with one. `none` states it. It is water-only: the
  generator refuses it elsewhere, because a road that draws no line is
  `hidden: true`, and unlike `none` that also keeps the class out of the rung's
  tile read.
- **2 px is the hard floor, and it is nowhere near enough to read as a surface.**
  `toneWayInterior` refuses below 2 px because a 1 px dither reads as a dashed
  line and a dash already means water here. But the floor is not the answer:
  measured at rung 1 over Vratna, a 3 px band with `dark` -- the densest dither
  there is -- comes out as

  ```
  ......#.#....
  .....#.#.....
  ......#.#....
  .....#.#.....
  ```

  two thin dotted columns, alternating. It reads as a pair of hairlines, not as a
  band. A checkerboard is 1 pixel in 2, so in a 3 px channel there is no room for
  it to be a texture rather than a lattice. Give a dithered watercourse **5 px or
  more**, or use `tone: "solid"` if what is wanted is a filled band -- and then
  the tone is doing what a plain thick stroke does, which is worth knowing before
  reaching for it. Where the readable floor actually sits is a panel question.
- **The tone is per class now.** It was a single layer-wide `waterTone`, which
  meant a lake and a river had to agree about what water looks like and a toned
  stroke had no tone of its own. The wave **hatch** stays layer-wide: only rings
  carry it, and one water surface should not have two wave rhythms on a panel.
- **A dash still applies over the finished ribbon**, not instead of it, which is
  what an intermittent stream should look like.

**Which tone reads as an obstacle is a panel question.** Compared at rung 1 over
Vratna on a 3 px interior: `stipple` (period 3, 1 in 9) is about one dot per 3 px
of length and barely registers; `dense` (period 2) reads as a textured channel;
`dark` (checkerboard) reads as a grey ribbon and is the strongest. `dark` is also
what the lake and the area-mapped river use, so a stream set to it is the same
water surface in a narrow channel -- which is the literal form of the request.
Judged on host renders only. Nothing here has been on glass.

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
| `roadFlagRules[4]` | a `layers.roads.rules[]` entry whose `match` names `flag` / `roughness_min` | `MapRenderer.cpp`, `roadStrokeFor()`, both road passes |

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

## One ink, and the marks it has to spend

There is no colour, so a feature is told apart by the **shape of its mark**, not
its hue. The whole style is the job of keeping these apart, and every one of
them was settled by pushing renders to the panel and looking
(`../../docs/map-legibility.md`).

| mark | means |
|---|---|
| solid, wide | the route |
| hollow ribbon (black edges, white core) | a major road |
| thin hairline | a minor road, and in bulk the texture that says settlement |
| dark surface with white waves knocked out | water |
| diagonal hatch | forest |
| stipple | built-up area (a period-3 dot grid -- see "Tone" below) |
| cased line in alternating black and hollow blocks | a railway |

Two consequences worth stating, because both were learned by getting them
wrong:

**Solid black is the route's mark and nothing else may take it.** Making major
roads solid black reads beautifully in isolation and destroys the route, which
`drawRoute` draws as a plain solid line with nothing else to distinguish it.
That is why roads keep their casing and why `gen_mapstyle.py` refuses a route
no wider than the widest road.

**A cased class needs a visible white core.** Below about 3 px the core stops
reading and the road turns back into a solid stroke -- into the route's
territory. Rule: `width - 2 * casing >= 3`.

## Dashed and ticked are different marks, not two lengths of one

`MapLinePattern` (`MapStyle.h`) has three values, and the middle two are easy to
confuse:

- **`Dashed`** breaks the stroke itself, so the background shows through. A
  watercourse: it should read as discontinuous, because it is not something you
  drive along.
- **`Ticked`** leaves the stroke whole -- casing and all -- and lays blocks
  across it. A railway: it must still read as one continuous line, because it
  is continuous, and it is a barrier crossed only at a level crossing.

They also want opposite rhythms, which is why `dash_px` and `gap_px` are
separate numbers rather than one period. The railway's proportions were read
off a reference map and are deliberate: 4 px wide as 1 px outline + 2 px core +
1 px outline, with blocks three widths long, 12 px black against 12 px hollow.

**The ticks are drawn in the second (white) road pass, not a third walk.** They
have to land after their own way's white fill or that fill erases them, and a
third pass would re-read the whole roads layer off the SD card for a few blocks
-- `kRoadPasses` is load-bearing in `MapTileSource` and `MapTileReader`, which
size their work by it. The residue is that a *later* cased road crossing a
railway paints over its blocks. That is a level crossing, where the road is
meant to read as on top, so it looks right rather than broken.

## A clipped ring's outline draws the cut, not the shore

`MapAreaFill::outlineRing` traces every edge of the ring it is given. A ring
reaching the device has been **clipped to its tile** (`mapbuilder/tilegen/tiles.py`,
`clip_polygon_to_box`), so some of its edges are not shoreline at all -- they
are the cut along the tile boundary. Outlined, they draw as a hairline straight
across the water at every tile edge.

Found on the panel 2026-08-08: a 1 px line across a bend of the Morava, traced
to two 9,000-unit ring segments sitting exactly on `x=0` of tile
`12/2240/1417`. At heading 270 the frame is rotated, so a tile's vertical edge
becomes a horizontal line on screen -- which is what made it look like a
feature rather than an artefact.

**Water areas therefore have no outline** (`layers.water`, the `lake` rule is
`hidden`, so `waterLinePx[lake]` is 0). The `Dark` tone defines the shape on
its own; `toneRing` and `hatchRing` do not care about `waterLinePx`, so the
surface and its waves are untouched.

**Buildings still carry a 1 px outline and have the same flaw.** A building
clipped by a tile boundary gets a straight edge drawn along it. It is rare and
small -- a building spanning a tile edge is unusual, and the false edge is a
few pixels rather than the width of a river -- so it is left alone knowingly.
Anything that gives buildings a heavier outline, or draws another outlined area
layer, inherits this.

The honest fix for all of them is a per-vertex "this edge came from the clip"
flag in the tile format so `outlineRing` can skip those segments. That is a
format change for a cosmetic problem, which is why it has not been made.

**A trap this cost three wrong diagnoses.** `scripts/gen_mapstyle.py` clamps a
visible class's width with `max(..., 1)`, so setting `width: 0` in the style to
test "what if there were no outline" silently generates **1** and changes
nothing. Only `hidden: true` produces 0. When changing the style to test a
hypothesis, check the generated header, not the rendered picture.

## Not every `railway=*` is a railway

`tag_to_class.py` used to be `if tags.get("railway")`, so anything carrying the
key drew as a mainline track. Measured over one Bratislava viewport
2026-08-07: 1508 `rail`, and also **604 `tram`**, 64 `disused`, 54 `razed`, 18
`abandoned`, plus 21 `workshop` / `signal_box` / `turntable` / `roundhouse` /
`traverser` / `loading_ramp`.

136 of those are track that no longer physically exists. Drawn as a bold
barrier, the map was announcing a level crossing where there is only road. It
went unnoticed while every line was 1 px; the block symbol made it obvious.

`mapbuilder/tilegen/tag_to_class.py` now uses an **allowlist** (`_RAILWAY_DRAWN`), not a
denylist -- the key carries far more than tracks, and a denylist lets the next
unfamiliar value through as a mainline railway, which is exactly how this
happened. `tram` is excluded by decision rather than error: a tram runs in the
street and is crossed anywhere, so drawing it like a mainline railway misstates
what the rider will meet.

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
- **a flag rule's `hidden`** -- added 2026-08-27, and it is a third thing again:
  the class is drawn, this *way* is not. The bytes are still read off the card,
  because the filter is per way and the class mask is per class. See below.

## Matching a way's flag bits

**Added 2026-08-27. The mechanism is in; no style uses it yet, on purpose.**

Every `.tib` way record has carried two attribute bytes since the first tile was
written -- `roughness` and a 16-bit `flags` -- and until this change `MapRenderer`
read **neither**. `MapTileReader` parsed them (`MapTileReader.cpp:379-380`), `MapWayRef` already
declared both (`IMapSource.h:33-34`) and `MapTileSource` already filled them in
(`MapTileSource.cpp:408-409`) -- so the carry-through was never the missing half.
The renderer simply never read the fields. It does now, in one place:
`roadStrokeFor()`, `MapRenderer.cpp:119`. The concrete cost: a track tagged `access=no` was drawn as
the same hairline as an open path, so the map told a hiker a closed track was
open.

### How much data is actually there

Measured 2026-08-27 with a throwaway Python walk over the whole local mirror
(`mapbuilder/cdn/base` in the parent repo): **1,291 tiles, 474,178 road-layer way
records** -- 86 tiles / 36,604 ways at z11, 262 / 146,694 at z12, 943 / 290,880 at
z13. **Measured, not read off the code.**

One caveat, and it matters: every tile in that mirror is **format version 3**, and
this branch's reader accepts version 4 only
(`MapTileReader::kFormatVersion`). So the counts came from a standalone parser,
not through `MapTileReader`, and the local `map_preview` cannot render that mirror
at all on this branch.

| bit | flag | ways set, all zooms | share | share at z13 |
|---|---|---|---|---|
| 0 | `link` | 5,985 | 1.3 % | 0.6 % |
| 1 | `bridge` | 17,142 | 3.6 % | 2.6 % |
| 2 | `tunnel` | 2,120 | 0.4 % | 0.6 % |
| 3 | `oneway` | 41,115 | 8.7 % | 6.4 % |
| 4 | `unpaved` | 43,665 | 9.2 % | 8.1 % |
| 5 | `no_motor` | 24,000 | 5.1 % | 6.7 % |
| 6 | `no_bicycle` | 21,342 | 4.5 % | 6.0 % |
| 7 | `no_foot` | 20,264 | 4.3 % | 5.7 % |
| 8-15 | waymark id, `seasonal`, `permit` | **0** | 0 % | 0 % |

**28,920 ways carry at least one access restriction** (bits 5-7), 21,700 of them
at the detail LOD, which is where a walker actually reads the map. Per class, the
restriction is concentrated rather than spread: `service` is 21.3 % `no_motor`
(12,654 ways), `ferry` 25.8 %, `track` 5.3 %, `footway` 3.1 % `no_bicycle`. So
this is not a rounding error in the data; it is a fifth of the service roads in
the mirror.

`roughness` is real too. Its low three bits, across the same 474,178 ways:

| value | meaning | ways | share |
|---|---|---|---|
| 0 | unknown | 218,951 | 46.2 % |
| 1 | best | 123,685 | 26.1 % |
| 2 | | 40,865 | 8.6 % |
| 3 | | 15,071 | 3.2 % |
| 4 | | 35,770 | 7.5 % |
| 5 | | 26,207 | 5.5 % |
| 6 | | 13,504 | 2.8 % |
| 7 | worst | 125 | 0.0 % |

**53.8 % of road ways carry a non-zero roughness.** Note that 0 is *unknown*, not
*smooth*, so a rule with a floor of 1 restyles a bit over half the network and a
rule that swept 0 in would restyle all of it.

### The grammar

A rule in `layers.roads.rules[]` whose `match` names `flag` or `roughness_min`
instead of `class` is a **flag rule**. Same list, same rule shape, same `when`
blocks -- there is no second place to say things.

```json
{ "match": {"flag": "no_foot"},
  "width": 1, "pattern": "dotted" }

{ "match": {"flag": ["no_motor", "no_bicycle"], "roughness_min": 5},
  "width": 1, "pattern": "dotted",
  "when": [{"modes": ["ride"], "hidden": true}] }
```

**A rule any variant can draw needs its `width` in the file.** `when` patches
rather than replaces, so `hidden: true` plus a `when` that unhides resolves to a
visible rule with no width, and `gen_mapstyle.py` refuses it -- correctly, since
there is no class width for a flag rule to inherit. Write the width once at the
rule and let `when` hide it where it is not wanted, which is the direction above.
Both examples in this file compile; the two that used to be here did not.

- **`flag`** is a name or a list of names. A list is **any bit set**, not all of
  them -- the same reading `match.class` already has.
- **`roughness_min`** is 1-7 and means `roughness & 0x07 >= this`. Only the low
  three bits are looked at, so a future `sac_scale` in bits 3-5 cannot be
  mistaken for a worse surface.
- Both together is **and**.
- Names, and only these: `link`, `bridge`, `tunnel`, `oneway`, `unpaved`,
  `no_motor`, `no_bicycle`, `no_foot`, `seasonal`, `permit`. Bits 8-13 are one
  6-bit waymark *symbol id*, not six flags, so no single bit of it has a name --
  a rule on "bit 9" could not be right.
- A rule may **not** carry both `class` and `flag`. Split it, or narrow it with
  `when`.
- **It replaces the stroke, it does not patch it.** So the rule must state its
  own `width`, or say `hidden: true`. `casing_px`, `pattern`, `dash_px` and
  `gap_px` default to a solid uncased line. There is no per-field inherit,
  because a flag rule spans classes and there is no one class width to inherit
  from -- and the trap that follows is that a rule matching `bridge` flattens a
  motorway to the width it names.
- `fill`, `tone` and `major` are **refused** on a flag rule rather than ignored.
  A tone is validated against its class's casing and interior width, and a rule
  spanning classes has no one interior to check. Open -- add it when a panel pass
  says a shaded flag treatment is wanted.
- **At most four rules** per resolved style (`kMapRoadFlagRuleSlots`,
  `MapStyle.h`). A fifth fails the build with the reason.
- **First match wins**, in file order. Deliberately not the `when` list's
  last-wins rule: a `when` entry patches, a flag rule replaces, and replacing
  twice is not a merge.
- **A class the style hides stays hidden.** `gen_mode_masks.py` intersects a
  hidden class out of that rung's tile class mask, so its ways never reach the
  renderer -- a flag rule that appeared to un-hide it would draw nothing and read
  as a bug in the rule.

### The warning: bits 8-15 exist and carry nothing

`docs/map-data-spec.md` allocates the whole remaining flag budget -- waymark id
(8-13), `seasonal` (14), `permit` (15) -- and `roughness` bits 3-5 (`sac_scale`)
and 6-7 (`trail_visibility`). **The builder writes zero into all of them today.**
Measured, not assumed: across 474,178 ways in the mirror, not one has a `flags`
bit above 7 or a `roughness` bit above 2 set.

So `{"match": {"flag": "seasonal"}}` compiles, validates, and **can never
match**. `gen_mapstyle.py` prints a warning naming the flag when a rule uses one
of those two names, rather than letting it look like a working rule that happens
to find nothing. The waymark bits have no names at all, which is the stronger
version of the same protection.

### What it costs

- **Flash: +826 bytes**, measured on `pio run -e default` before and after
  (3,990,165 → 3,990,991 bytes). Of that, 600 bytes is the table itself:
  `sizeof(MapRoadFlagRule)` is 10, four slots is 40, and `data/mapstyle.json`
  compiles to 14 distinct variants plus the base. The rest is `roadStrokeFor()`.
- **RAM: 0 bytes.** Unchanged at 59,012 both sides. The table is `constexpr` in
  the generated header, so it lives in flash; `RoadStroke` is a 24-byte local,
  well inside the 256-byte stack rule.
- **Per way, per pass:** up to four compares against an all-zero slot, on values
  that are in flash beside the widths the pass already reads. No allocation, no
  per-way state. **Read off the code -- not measured on hardware.**

### What a panel pass has to check

**Nothing here has been on a device.** There is no X4 attached
(`docs/PROGRESS.md`, the hardware gap), and the shipped style carries no flag
rule, so the only thing a hardware run can confirm today is that the render is
*unchanged* -- which is what `test/map_tile_reader`'s golden PPM already asserts
bit for bit on the host.

When the maintainer does turn a rule on, the panel questions are:

- Does a dotted or thinned hairline still read as a path at all on the glass? A
  1 px line under any break is close to invisible in daylight
  (`../../docs/map-legibility.md`).
- Does hiding restricted ways leave a hole a rider reads as "no data" rather than
  "no route"? That is the same failure the missing-tile hatch exists to prevent.
- At the coarse rungs, does a 5.7 % share of restyled ways read as a distinction
  or as noise?

Judge it through `tools/style_watch.py` first (`docs/device-preview.md`), then on
the panel. Never off a laptop PNG -- that rule is in the parent `CLAUDE.md` and it
is what this whole default-off arrangement is built around.

### Where the tests are

Two halves, because the grammar is evaluated on the laptop and only the resolved
structs reach the device:

- `scripts/test_mapstyle_flag_rules.py` -- the parsing: masks, the bit names, the
  refusals, and an assertion that the shipped `data/mapstyle.json` still carries
  no flag rule. Runs under `ctest` as `MapstyleFlagRules`.
- `test/map_flag_rules/MapFlagRulesTest.cpp` -- the drawing: a rendered way,
  checked for thickness and ink. Includes the default-unchanged pair (a way with
  every data-carrying flag set draws identically to an open one under
  `kDefaultMapStyle`), which is the test that should go red the day the knob is
  turned on.

`test/map_tile_reader`'s golden PPM is the whole-frame version of the same claim
and is unchanged by this work.

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

The answer on 1-bit is a **tone**: a screen-space pixel pattern that reads as
flat grey from arm's length. `MapAreaTone` (`src/activities/map/MapAreaTone.h`)
has three fixed patterns and a family of dot grids:

| tone | pattern | reads as |
|---|---|---|
| `Light` | 1 px in 4 | light grey |
| `Dark` | 1 px in 2, checkerboard | mid grey |
| `Solid` | every pixel | black, for shapes too small to texture |
| a dot grid | 1 px per period x period cell | a dotted texture, lighter the longer the period |

**A dot grid's period is packed into the tone value**, since 2026-08-26, so a
style can name a density rather than a name: `tone: dots, tone_period_px: 5`.
Shorthands exist for the three in use -- `dense` (period 2, 1 in 4), `stipple`
(period 3, 1 in 9) and `micro` (period 4, 1 in 16) -- and each has a `_stagger`
twin that offsets alternate dot rows by half the period.

Packed rather than a second field beside the tone, because a tone travels alone
through `IMapCanvas::fillSpan`, `MapAreaFill` and six fields of `MapStyle`. A
parameter next to it would have to be threaded through every one of those, and
the two could then disagree. `MapTone::dots(period, stagger)` builds one,
`dotPeriod()` and `isStaggered()` read it back, and host tests round-trip every
period from 2 to 15.

**`micro` exists because a wash under contour lines wants less ink than
stipple.** At 1 in 9 the dots compete with the lines; at 1 in 16 they read as a
surface and the lines stay the loudest thing.

**Why a staggered twin.** A single-dot-per-cell grid is perfectly regular, which
is clean and is also the thing that can beat against the panel's own pixel
structure, against a hatch, or against a neighbouring area's dots. The stagger
breaks the vertical alignment at identical density, so the two can be compared
with nothing else moving. **Which is better is a panel question and is open** --
the host render cannot answer it, which is the whole reason both exist
(maintainer's call, 2026-08-26).

**A dot grid is painted in strides, not pixel by pixel.** `fillSpan` knows the
period, so a row carrying no dots is skipped whole and a row carrying them is
walked `period` at a time: at 1 in 16 that is one `drawPixel` per sixteen columns
rather than sixteen tests. The lightest tone is therefore the cheapest to paint,
not the dearest -- the opposite of what it was when `Stipple` was the only
non-native pattern.

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

### Borders: what each layer actually has

Four layers draw a boundary, and each has its own field for it. There is no
shared "border" concept, which is worth knowing before looking for one.

| layer | field | what it draws |
|---|---|---|
| buildings | `layers.buildings.rule.outline_width` | 1 px ring around each building, over its own fill |
| landuse | `layers.landuse.rules[].outline_width` | per class, over the tone and the hatch |
| water | `layers.water.rules[].outline_width` | the ring of a water **area** -- a lake |
| roads | `layers.roads.rules[].casing_px` | not an outline: a black stroke with a narrower white or dithered one inside it |

**Water's outline was the same number as its line width until 2026-08-26.** A
lake's ring and a river's stroke shared `waterLinePx`, so widening the river
widened every lake edge with it. `waterOutlinePx` splits them, and defaults to
the line width when a style says nothing, so an existing style draws exactly
what it drew. Today's style leaves the lake rule `hidden`, so both are 0 for
lakes and the `Dark` tone defines the shape on its own -- see "Water" above.

**A road's casing is not an outline and should not be read as one.** An outline
is drawn around a shape that already has a fill; a casing is the fill: the class
is stroked black at `width`, then a narrower stroke of white (or a tone) is laid
inside it, so the "border" is whatever black is left at the edges. That is why
`casing_px` is refused when `2 * casing >= width` -- there would be no inside
left, and the road would be a solid black line drawn the slow way, in two passes.

**Three things a border cannot do, all of them deliberate stops rather than
oversights:**

- **It is always black.** `MapRenderer` passes `MapInk::Black` at all three call
  sites. A white outline is what would separate two adjacent dark areas, and it
  would need the draw order to say which of the two owns the boundary -- so it is
  a real feature and not a parameter.
- **It is always drawn last** -- see below. (A dashed one arrived 2026-08-26 and
  has its own section.)
- **It is always drawn last.** Tone, then hatch, then outline, in every area
  pass. An outline under a fill would let a hatch break the edge, which is
  sometimes what a cartographer wants and is never what this draws.

### A dashed area boundary, and why not on forest

`layers.landuse.rules[].outline_dash_px` and `outline_gap_px`, both required
together, turn that class's boundary into dashes. Both at 0 is solid, which is
what "no dash" means; one without the other is refused at generation, because a
dash with no gap is a solid line drawn the slow way and a gap with no dash draws
nothing.

**The dash phase runs along the whole ring, not per segment.** After
simplification a ring has a vertex every pixel or two, so a per-segment reset
would put a dash at every vertex -- a solid line with a stutter rather than a
dashed boundary. `MapAreaFill::outlineRingDashed` carries the travelled distance
across vertices, measured in Bresenham steps because that is the rhythm a pixel
grid actually draws in.

**Do not use it on forest in hike mode.** Judged at 1:1 on a Mala Fatra rung-1
frame, 2026-08-26, against a solid 1 px edge on the same tiles: the dashed
boundary is indistinguishable from the other dashed lines in the same frame.
That is not a matter of taste.

**Corrected 2026-08-26, and the correction makes the case stronger rather than
weaker.** This paragraph first said a dash means a *path* in this style, and that
is wrong. Resolved from the committed file, hike at rung 0: dashed is
**water** (`unknown`, `river`, `stream`), **ferry** and **aerialway**; railway is
`Ticked`; and every pedestrian class -- `footway`, `path`, `pedestrian`,
`cycleway`, `bridleway` -- is `pattern: solid, width: 1`, with `steps` and
`track` hidden. The lines the dashed forest edge could not be told from were
**streams**.

Which is worse than colliding with a path. A dashed hairline running along a
hillside and crossing contours reads as a watercourse, and one Tatra bbox holds
2,298 `waterway=stream` ways. A wood edge misread as a path is a wrong route; a
wood edge misread as a stream is a wrong water source, on the layer a walker
plans resupply from. It is also the argument for leaving dashed to water and
never giving it to a trail.

What the same renders did settle is that the edge needs *something*: with no
outline the dot tone simply stops, and nothing says whether the wood ends there
or the mapping does. A solid 1 px outline was the only variant where the boundary
could be told from the trails at a glance.

So the mechanism is here for the boundaries that do not collide -- `built_up`,
and an administrative area if one ever arrives -- and forest wants
`outline_width: 1` with no dash.

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

### What the hatch fields cost

Both of these are render cost, decided in the style file, with no code involved.
Measured counts are in
[`optimization/01-render-pipeline.md`](optimization/01-render-pipeline.md); the
short version:

- **`hatch_spacing_px`.** Lines per axis go as `1/spacing`, so 8 is half the lines
  and half the pixels of 4. At 1 m/px a building is 13 hatch lines and 416 pixels
  at spacing 4.
- **The pattern's axis count.** `X` is cross, which runs the axis fill twice. One
  axis is half the work, and in portrait the **vertical** one is the cheaper of
  the two, because a logical vertical span is contiguous in physical framebuffer
  memory.

Both change the picture, so they are style decisions. Judge them in the webapp's
`firmware` panel (`docs/device-preview.md` in the parent repo), not by arithmetic.

**One constraint if `fill` ever changes to `tone`.** A tone is a dithered fill,
and a dithered fill writes **both** inks (`GfxRenderer.cpp:1030-1052`), while
hatch and outline write black only. Anything that relies on buildings being
additive — notably the two-phase render in
[`optimization/09-progressive-render.md`](optimization/09-progressive-render.md),
which draws buildings after the roads — breaks silently the moment buildings get
a tone.

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

**Do not read those 11-17 ms as device milliseconds.** The laptop has an FPU and
the ESP32-C3 does not, and the whole geometry path — projection, edge crossings,
segment clipping — is written in `double`
([`optimization/01-render-pipeline.md`](optimization/01-render-pipeline.md)). The
same work is a software routine per operation on device. The host number is a
count-of-work proxy, not a timing.

**Answered on hardware 2026-08-05**, see the timing table at the top of this
document: 484 KB with buildings at the detail LOD is a 2.5-3.2 s viewport reset
against 1.1 s for the 198 KB regional view. Buildings are also no longer written
at the coarse LODs at all (`mapbuilder/tilegen/build_config.json`), so the expensive case
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
