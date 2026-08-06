# Plan 01 — render pipeline

What one viewport reset spends its milliseconds on, and how to cut it.

A reset is the device's slowest operation. The debug readout already times it
(`MapActivity.cpp:1027`, `%lums` on line 2), so every item here has a gate that
costs no new instrumentation.

## The fact that drives this whole plan

**ESP32-C3 has no floating-point unit.** **read** — the target is
`esp32-c3-devkitm-1` (`platformio.ini`, `[base] board`), RISC-V `rv32imc`. No
`F` extension, no `D` extension. Every `float` and every `double` operation is
a libgcc software routine: a call, a stack frame, a loop.

The whole map geometry path is written in `double`.

- `MapProjection::projectMerc` — 2 subtractions, 4 multiplies, 2 divides, 2
  `lround`, all `double` (`MapProjection.cpp:27-34`).
- `MapProjection::projectTileLocal` calls it once **per point of every way**
  (`MapProjection.cpp:36-41`, called from `MapTileSource.cpp:152-154`).
- `MapAreaFill::collectCrossings` does one `double` divide and one `double`
  multiply **per ring edge per scan line** (`MapAreaFill.cpp:30-31`).
- `GfxRendererCanvas::clipToRect` runs Cohen-Sutherland in `double`, up to 4
  divides per segment, and it runs once **per stack copy** of every road
  segment (`GfxRendererCanvas.h:136-182`, called at `:44`).
- `MapStroke::stackFor` calls `std::sqrt` and `std::ceil` per segment
  (`MapStroke.h:57-61`).

None of it needs `double`. Screen coordinates are `int16_t`. Mercator metres
fit `int32_t`. The rotation is two constants per frame.

**open**: how many milliseconds this actually is. Nothing has profiled it. The
measurement is step 1 below and it is cheap.

## The other fact: almost none of that work draws a pixel

**measured 2026-08-06**, from real `.tib` tiles in
`mapbuilder/test-data/trailink-sd`, by `mapbuilder/tools/tile_cost.py` (parent
repo). Counts are exact; no device needed, and no device timing is implied.

A tile is much larger than the screen at the rung that reads it. The detail LOD
is the extreme case:

| rung | m/px | LOD | tile span | loaded area (2x2) | screen area | ratio |
|---|---|---|---|---|---|---|
| 0 | 1 | z13 | 4,892 m | 95.7 km² | 0.38 km² | **249x** |
| 1 | 3 | z13 | 4,892 m | 95.7 km² | 3.46 km² | 28x |
| 2 | 6 | z12 | 9,784 m | 382.9 km² | 13.8 km² | 28x |
| 3 | 12 | z11 | 19,568 m | 1,532 km² | 55.3 km² | 28x |
| 4 | 20 | z11 | 19,568 m | 3,446 km² (3x3) | 153.6 km² | 22x |

A dense z13 tile in that set holds **9,000 to 11,300 buildings and about
100,000 points in 640-710 KB** — two ranges because `mapbuilder/test-data` is
gitignored and gets regenerated, and both builds measured on 2026-08-06 landed in
that band. At rung 0 a 2x2 range therefore reads **36,000 to 45,000 buildings to
draw 150 to 180** — 0.4 % of the buildings walked produce a pixel. At rung 1 it
is 3.6 %.

The **ratio** is the durable number here, not the tile: it comes from the tile
grid against the screen, so it does not move with which area was built. Re-derive
any of this with the tool; do not trust a tile path in this document to still
exist.

Every one of those tens of thousands is read off the card, **projected point by point in
software `double`** (`MapTileSource.cpp:169-171`), and **hatch-scanned in full**
(`MapAreaFill::hatchRing`, which takes its scan range from the ring's own bbox
and never asks where the screen is, `MapAreaFill.cpp:126-153`). Only the last
step — the pixel write — is skipped, by `clipToRect` rejecting the line
(`GfxRendererCanvas.h:44`).

For scale on the other side of that ratio: an on-screen building at 1 m/px is
13-15 hatch lines and 400-660 hatch pixels (cross hatch, 4 px,
`data/mapstyle.json`). 150 of them is under 100,000 pixels — nothing. **The
frame's cost is almost entirely work on geometry that is not on the screen.**

This changes the order of this plan. Step 3 was written as a maybe with a "skip
it if under ~15 %" gate; the number is 99.6 % at rung 0, so **step 3 is the
first thing to do**, and the fixed-point work in step 2 matters mostly for the
points that survive it.

## The baseline, measured on hardware

**measured 2026-08-06** on the X4, firmware `4079a4c0` (this plan's step 1
instrumentation), dense Bratislava — `pos 48.151428 17.116699 heading 3`, mode
hike, whole zoom ladder, tiles already on the card. `bytes` is
`MapTileSource::bytesRead()`, `points` is `pointsProjected()`.

| rung | LOD | tiles | total ms | landuse | **buildings** | water | **roads** | places | points | bytes |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | z13 | 1 | 10,861 | 130 | **7,058** | 58 | 3,605 | 10 | 170,922 | 1.8 MB |
| 1 | z13 | 2 | **15,324** | 159 | **8,641** | 65 | 6,441 | 18 | 290,914 | 3.1 MB |
| 2 | z12 | 4 | 6,636 | 1,439 | 25 | 376 | **4,761** | 35 | 116,400 | 1.4 MB |
| 3 | z11 | 2 | 5,038 | 1,280 | 12 | 535 | **3,191** | 20 | 81,081 | 0.9 MB |
| 4 | z11 | 9 | 12,731 | 3,554 | 56 | 1,828 | **7,205** | 88 | 229,280 | 2.5 MB |

Five things in that table, and three of them were not what this plan assumed:

1. **Buildings are the detail-LOD cost, as predicted.** 65 % of rung 0, 56 % of
   rung 1. The count-based prediction holds.
2. **Rung 1 is the worst rung, not rung 0** — 15.3 s. Two tiles instead of one,
   so it reads twice the geometry for a coarser picture. The area ratio said
   rung 0 was the outlier; wall time says the tile *count* matters more than the
   ratio.
3. **Roads are the second cost everywhere and the first cost at every coarse
   rung.** 7.2 s of rung 4's 12.7 s, 4.8 s of rung 2's 6.6 s. This plan
   under-weighted them: a road is drawn twice (`kRoadPasses`), each pass
   decomposes every thick segment into `MapStroke` copies, and **each copy gets
   its own `clipToRect` in `double`** (`GfxRendererCanvas.h:39-46`). So step 6(2)
   and step 10 are not tail items — they are the coarse-rung fix.
4. **Landuse is 3.5 s at rung 4**, from unclamped scan lines over rings far
   bigger than the panel. Step 4 confirmed.
5. **Up to 3.1 MB read off the card for one frame.** That is
   [plan 02](02-tile-io.md)'s territory and it is the largest single number here.

Revised order, on the measurement rather than on the counts:

1. Step 3, off-screen reject — the only item that cuts every layer at once.
2. Step 10 (trivial-accept clip) and step 6(2) (clip a stack once) — the road
   path, which the counts had ranked too low.
3. Step 4 and 5 — landuse at the coarse rungs.
4. Step 2, fixed point — 170k to 291k points per frame go through it.
5. Step 9 — the hairline fill, which mostly helps hatch and so mostly helps
   rung 0-1.

## Step 1 — measure before changing anything

Add a temporary counter pair to `renderViewport()`: points projected, and
`millis()` around the `MapRenderer::render()` call only, separate from the tile
reads. `MapTileSource::waysEmitted()` already gives way counts
(`MapTileSource.h:90`); points are what is missing.

**Time each layer, not just the frame.** Four `millis()` deltas around the four
blocks of `MapRenderer::render()` — landuse, buildings, water, roads
(`MapRenderer.cpp:82`, `:90`, `:102`, `:134`) — answer "is it buildings?" in one
flash, and that is the question every item below turns on. Log them as one
`LOG_DBG` line; they do not need to reach the panel.

Run the same coordinate at every zoom rung, ride and hike mode, over serial
(`mapcmd.py pos ...` then `zoom`). Record ms and point count per rung.

Gate for every later step in this plan: **same framebuffer, fewer ms.** The
framebuffer is checkable byte-for-byte with `CMD:SCREENSHOT` before and after
(firmware `CLAUDE.md`, the screenshot channel section). A render optimisation
that changes one pixel is a render change, not an optimisation, and has to be
argued as one.

## Step 2 — fixed-point projection

Replace the `double` projection with Q16 fixed point.

Per reset, in `MapProjection::reset()`, precompute:

```
kx = lround(cosTheta / mppMerc * 65536)   // int32
ky = lround(sinTheta / mppMerc * 65536)   // int32
anchorMercXi = lround(anchorMercX)        // int32, metres
anchorMercYi = lround(anchorMercY)        // int32, metres
```

Per point:

```
int32 east  = mercX - anchorMercXi;       // metres, |east| < ~25000
int32 north = mercY - anchorMercYi;
int32 sx = anchorScreenX + (int32)(((int64)east * kx - (int64)north * ky) >> 16);
int32 sy = anchorScreenY - (int32)(((int64)east * ky + (int64)north * kx) >> 16);
```

Notes that matter:

- **Use an `int64` intermediate.** Not because `int32` overflows, but because
  its margin is thin and latitude-dependent. Worked through, per rung — tile
  span in Mercator metres from `MapTileGrid::kWorldSizeM / 2^z`, `mppMerc` from
  `MapViewport::mppMercFor` (`MapViewport.cpp:14-17`), max `|east|` about 1.5
  tile spans across a 3×3 range:

  | rung | mpp | LOD | tile span | max \|east\| | `kx` at lat 48.5 | product |
  |---|---|---|---|---|---|---|
  | 0 | 1 | z13 | 4,892 m | ~7,300 m | 43,300 | 3.2e8 |
  | 2 | 6 | z12 | 9,784 m | ~14,700 m | 7,200 | 1.1e8 |
  | 4 | 20 | z11 | 19,567 m | ~29,400 m | 2,200 | 6.4e7 |

  Small `mpp` means a small tile extent, so the two terms trade off and the
  product never approaches `int32`'s 2.1e9. The worst case is the equator, where
  `cos(lat) = 1` doubles `kx` at rung 0: 7,300 × 65,536 ≈ 4.8e8, and the two
  terms summed reach ~9.6e8. That fits, with about 2× headroom.

  2× is not enough headroom to bet a silent wrap on, and a 64-bit multiply on
  rv32 is a handful of instructions — still far cheaper than a software `double`
  multiply plus a software divide. Take the `int64`. If profiling later says the
  64-bit multiply is the cost, Q8 in `int32` has room to spare (rung 0 equator:
  7,300 × 256 = 1.9e6).
- The divide disappears entirely. `1/mppMerc` is folded into `kx`/`ky` once per
  reset.
- Rounding: `>> 16` truncates toward negative infinity. Add `(1<<15)` before
  the shift to round to nearest, so the output matches `lround`'s behaviour on
  the golden test.
- `MapProjection::screenToMerc` is called 4 times per reset
  (`MapViewport.cpp:25`) and `lonLatToMerc` twice. Leave both in `double`.
  Four calls per frame is not worth the precision argument.

Gate: `test/map_preview`'s golden render is byte-identical, or the diff is
explained pixel by pixel. That test already exists and links the same
projection (`MapViewport.h:13-16`).

## Step 3 — reject off-screen ways before projecting them

**Do this one first.** It is the largest single item in this plan: at rung 0 it
removes the work for 99.6 % of the buildings walked (measured, see above).

The tile range is deliberately wider than the screen — `kMarginPx` is 64 px on
top of a rotated bbox (`MapViewport.h:37`, `MapViewport.cpp:37-41`) — but that
margin is not why the range is wide. The range is wide because **a z13 tile is
4,892 m and the rung-0 screen is 480x800 m**: the smallest possible range is
already 250x the visible area. Right now every point of every way is projected,
then the segments are clipped one at a time (`GfxRendererCanvas.h:44`).

Cheaper: while reading a way's points (`MapTileSource::nextWayRecord`,
`MapTileSource.cpp:135`) track the local `min/max` x and y — free, integer,
the loop is already there. Then project the four bbox corners (4 projections,
not N), inflate by the style's widest stroke, and if the box misses the screen
entirely, skip the way: no per-point projection, no per-segment clip, no draw.

Cost: 4 projections and a rect test per way. Saving: N projections and N clips
per rejected way, where N reaches 256 (`MapTileReader::kMaxWayPoints`).

The record must still be **read** in full — reading is what advances the stream
(`MapTileSource.cpp:143-146` already says this about the class mask). Only the
projecting is skipped.

**Rejection has to happen in `MapTileSource`, not in `MapRenderer`.** A reject in
the renderer saves the hatch scan but not the projection, and the projection is
the bigger half. Two levels, both worth having:

1. **In `nextWayRecord`** (`MapTileSource.cpp:142-181`): track local `min/max`
   while reading the points, project the four bbox corners, reject before the
   per-point loop at `:169-171`. Saves projection **and** everything downstream.
2. **In `MapAreaFill`** (step 4 below): clamp what survives to the screen.

**open, and cheap to close**: the same fraction for roads. The measurement above
is buildings, whose count comes from a real tile; roads are 6,073 records and
18,596 points in the same tile, so the ratio is the same but the absolute saving
is 6x smaller. Add a counter next to `waysFiltered_` (`MapTileSource.h:90`) and
read it off `info` per layer.

## Step 4 — clamp area fills to the screen before scanning

`MapAreaFill::toneRing` scans **every row of the ring's own bounding box**
(`MapAreaFill.cpp:163`), and the bounding box comes from projected coordinates
with no screen clamp (`ringBounds`, `:59-68`). A landuse ring covering one z13
tile at the 1 m/px rung is roughly 3,200 px tall. The panel is 800.

So the loop can run 4× more rows than the panel has, and every row costs
`O(pointCount)` with a software `double` divide per crossing. `hatchAxis`
(`:71-93`) and `hatchDiagonal` (`:95-122`) have the same shape.

Fix, three lines each: intersect the scan range with `[0, screenHeight-1]` (or
the x range for the vertical family) before the loop. The canvas already drops
out-of-range output (`GfxRendererCanvas.h:61`, `:67-69`), so this changes no
pixel — it only stops computing pixels that are thrown away.

`hatchDiagonal`'s range is in the diagonal index `c`, not in x or y. Clamp its
`from`/`to` by intersecting the ring bbox with the screen rect first, then
recomputing `from`/`to` from the clamped box.

`MapAreaFill` needs the screen size to do this. It has no renderer reference by
design. Two options: pass the clip rect as a parameter through
`MapRenderer::render` (it already has the canvas), or add
`IMapCanvas::clipBounds()`. The second is smaller and both implementations —
`GfxRendererCanvas` and `test/map_preview`'s `PpmCanvas` — already know their
own size.

Gate: `test/map_area_fill` still passes, plus a new case with a ring far larger
than the canvas asserting the same pixels as today.

## Step 5 — integer crossings in `MapAreaFill`

After step 4 the scan count is bounded by the panel, so the per-crossing cost
is what is left. `collectCrossings` does:

```
const double t = (double)(value - a) / (b - a);
const int crossing = (int)(aOther + t * (bOther - aOther));
```

Integer form: `crossing = aOther + (int32)(((int64)(value - a) * (bOther - aOther)) / (b - a))`.
Coordinates are `int16_t`, so the product fits comfortably in `int64` and the
one integer divide replaces a software `double` divide plus a software
multiply.

The comment at `:27-29` says doubles were chosen because "the products overflow
32 bits". True for `int32`; not true for `int64`. Update the comment in the
same change — a comment that now states something false is part of the work,
not a follow-up (firmware `CLAUDE.md`).

Gate: `test/map_area_fill`, unchanged expectations. The half-open edge rule at
`:23` must not move; that rule is what keeps hatch inside a ring.

## Step 6 — stop drawing a thick road N times

`MapStroke::stackFor` decomposes a thick line into `count` one-pixel Bresenham
lines, where `count = ceil(lineWidth * len / major)` (`MapStroke.h:57-62`). A
6 px road at 45° is 9 separate Bresenham walks over the same segment, each
walking every pixel of it, each preceded by its own `clipToRect` call
(`GfxRendererCanvas.h:39-46`).

Every pixel of a thick road is therefore touched several times, and
`GfxRenderer::drawPixel` is not cheap per call: a non-inlined function, a
`rotateCoordinates` switch, a bounds check with a `LOG_ERR` branch, and a
read-modify-write on one byte (`GfxRenderer.cpp:490-520`).

Two candidate replacements, in order of preference:

1. **One quad per segment.** A segment of width `w` is a rectangle; hand it to
   `IMapCanvas::fillPolygon` as 4 points. Each pixel is written once.
   `GfxRenderer::fillPolygon` already exists and is used for the marker
   (`MapActivity.cpp:203`). Joins between segments leave a notch on sharp
   corners — for map roads at these widths that is probably invisible, but it
   is a **visual** change, so it needs the screenshot comparison and a look on
   glass, not just a timing number.
2. **Clip once, offset after.** Keep the stack, but clip the base segment once
   and derive the copies from the clipped endpoints, re-clipping only when the
   base segment is within `lineWidth` of an edge. Pure win, no visual change,
   smaller payoff than (1).

Do (2) first — it is safe and mechanical. Treat (1) as a separate branch with a
side-by-side device shot, because it is the only item in this plan that can
change what the map looks like.

## Step 7 — orientation-aware span direction (investigate, do not assume)

The map runs in portrait. **read**: the settings default is
`orientation = PORTRAIT` (`CrossPointSettings.h:212`) and nothing in
`src/activities/map/` overrides it.

In portrait, `rotateCoordinates` maps logical `(x, y)` to physical
`(y, panelHeight-1-x)` (`GfxRenderer.cpp:217-222`). So a **logical horizontal
run is a physical vertical column**: one bit in each of N different bytes,
N rows apart. A logical vertical run is a contiguous physical byte run.

`MapAreaFill::toneRing` fills logical horizontal spans (`:170`), which is the
expensive direction on this panel. `fillRectImpl`'s fast path — `memset` across
whole bytes — only triggers for runs that are contiguous in physical memory
(`GfxRenderer.cpp:933-961`).

So scanning tone fills by **column** instead of by row would turn each span
into a byte run. The machinery for it already exists: `collectCrossings` takes
a `horizontal` flag (`MapAreaFill.cpp:14`) and `hatchAxis` already uses both
directions.

**open**, and the reason this is step 7 and not step 4: the win is unmeasured,
and it couples the fill direction to the panel orientation, which is a coupling
this code has deliberately avoided. Measure first — fill one full-screen ring
both ways under `millis()` on the device. If the difference is under ~10 ms,
drop the idea and write that down; the coupling is not worth less than that.

## Step 8 — the grey question, answered before it is asked

The panel does four grey levels, and the map deliberately does not use them
(`README.md`'s status table, `docs/eink-grayscale.md`). This plan agrees, and
here is the render-cost reason to put next to the existing contrast reason:

`GrayscaleFrame::render()` calls its draw callback **once for the base pass and
once per band per plane — 13 times on a 480-row panel**
(`GrayscaleFrame.h:116-118`, `STRIP_ROWS = 80` at `:102`). The map's draw
callback is a streaming pull off the SD card that holds no geometry
(`IMapSource.h:9-27`). Thirteen invocations means **thirteen full tile reads**,
CRC included.

So a grey map frame is not "one more waveform pass". With today's source it is
13× the SD cost of a reset. Anyone wanting grey areas on the map needs a
replayable geometry cache first, and that cache is exactly what `IMapSource`
exists to prevent (`IMapSource.h:9-14`, 40.7 KB and 1218 allocations for one
dense z12 tile).

Verdict: grey stays out of the map until there is a bounded display list. Not a
task in this plan — a reason recorded so the question does not get re-opened
from scratch.

## Step 9 — an axis-aligned hairline is a rect, not a pixel loop

**read.** `GfxRenderer::drawLine` special-cases both axis-aligned directions and
walks them **one `drawPixel` call per pixel** (`GfxRenderer.cpp:673-686`).
`drawPixel` is a non-inlined function with a `rotateCoordinates` switch, a bounds
check carrying a `LOG_ERR` branch, and a read-modify-write on one byte
(`GfxRenderer.cpp:490-524`).

`GfxRenderer::fillRect` reaches the same pixels through `fillRectImpl`, which
rotates **two corners**, then writes whole bytes with masks and a `memset`
(`GfxRenderer.cpp:837-843`, `:892-963`). Same pixels, one call instead of N.

Every hatch line is exactly this shape: `hatchAxis` emits axis-aligned lines of
width 1 (`MapAreaFill.cpp:87-89`). So does every horizontal or vertical road
segment and every ring outline edge on those axes.

Change, in `GfxRendererCanvas::drawLine` (`GfxRendererCanvas.h:32-47`), not in
`GfxRenderer` — the fork rule keeps inherited code alone (`CLAUDE.md`, "Treat
inherited code as upstream's"):

```cpp
// after clipToRect, per stack copy
if (cy1 == cy2)      renderer_.fillRect(min(cx1,cx2), cy1, |cx2-cx1|+1, 1, black);
else if (cx1 == cx2) renderer_.fillRect(cx1, min(cy1,cy2), 1, |cy2-cy1|+1, black);
else                 renderer_.drawLine(...);   // unchanged
```

Pixel-identical by construction, so the screenshot gate should show a byte-equal
framebuffer. Note the portrait asymmetry from step 7: a **logical vertical** run
is contiguous in physical memory and gets the `memset` path, while a logical
horizontal run of N pixels is N one-byte masked writes. Both beat N `drawPixel`
calls; only one of them beats it by a lot. Mirror the change in
`test/map_preview`'s `PpmCanvas` only if it measures there too — it does not have
to, the two canvases already differ in their line implementations
(`docs/device-preview.md` in the parent repo, "Close, not provably
byte-identical").

## Step 10 — clipping an already-visible line should not touch a `double`

**read.** `clipToRect` converts all four coordinates to `double` before it even
computes the two outcodes, and it does that for every line
(`GfxRendererCanvas.h:136-140`). The common case after step 3 is a line **fully
inside** the screen, where the whole function is a no-op that rounds its own
inputs back to where they started.

Change: an integer trivial-accept in front of it.

```cpp
if (x1 >= 0 && x1 <= maxX && y1 >= 0 && y1 <= maxY &&
    x2 >= 0 && x2 <= maxX && y2 >= 0 && y2 <= maxY) return true;   // no doubles
```

Four compares against eight, no `double`, no `lround`. Pixel-identical: for an
inside segment the existing path already returns the same integers.

Cheapest item in this plan by effort. It is listed last because its size depends
on step 3 — before step 3, most lines are *not* trivially inside, and the fast
path misses.

## The style levers, for when code is not wanted

Two knobs cut hatch work with no code at all, both in `data/mapstyle.json` under
`layers.buildings.rule`, both testable in the webapp's `firmware` panel in about
two seconds (`docs/device-preview.md` in the parent repo):

- **`hatch_spacing_px: 4` → `6` or `8`.** Hatch lines per axis go as
  `1/spacing`, so 8 is half the lines and half the pixels of 4.
- **`hatch: "XXXX"` → `"||||"`.** `X` is `Cross`, which runs `hatchAxis` twice
  (`MapAreaFill.cpp:140-143`). One axis is half the work. Prefer the **vertical**
  one: in portrait a logical vertical span is a contiguous physical byte run
  (step 7), so it is also the cheaper axis to fill.

Both change what the map looks like, so they are style decisions, not
optimisations, and they go through the preview panel and a look on glass. They
are listed here so the cost side of that decision is on record.

One concern per commit (`refactor-for-review` skill). Reordered 2026-08-06: the
off-screen reject moved to the front, because the measurement above says it is
where the frame goes.

1. `perf(map): time each layer of the render pass` — instrumentation only,
   step 1.
2. `perf(map): skip projecting fully off-screen ways` — step 3. Biggest item.
3. `perf(map): clamp area scan lines to the screen` — step 4.
4. `perf(map): fill an axis-aligned hairline as a rect` — step 9.
5. `perf(map): accept an on-screen segment without clipping it` — step 10.
6. `perf(map): project way points in fixed point` — step 2.
7. `perf(map): integer edge crossings in area fill` — step 5.
8. `perf(map): clip a thick road once, not once per copy` — step 6(2).
9. Revert the instrumentation from commit 1, or keep it behind the existing
   `LOG_DBG` level if the numbers stay useful.

Commits 2 to 5 are all pixel-identical and all small; take the framebuffer
comparison after each. Commits 6 and 7 change arithmetic, so they need the golden
test as well.

Steps 6(1) and 7 are their own branches with their own device evidence.

## What this plan cannot fix

Step 3 stops the device **projecting and scanning** geometry it will not draw. It
cannot stop it **reading** it: the bytes are what advances the stream
(`MapTileSource.cpp:160-163`). At rung 0 that leaves tens of thousands of
buildings' worth of card reads for ~150 drawn buildings, whatever this plan
does.

Two places that can be fixed, neither of them here:

- **The read itself** — plan [02-tile-io.md](02-tile-io.md), which halves it by
  not reading every layer twice.
- **The tile contents** — the detail LOD's tile is 6x the screen's height at
  rung 0, so the smallest legal range is already 250x the visible area. That is a
  tile-format and mapbuilder question: `docs/tile-simplification-plan.md` in the
  parent repo.
