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

## Step 1 — measure before changing anything

Add a temporary counter pair to `renderViewport()`: points projected, and
`millis()` around the `MapRenderer::render()` call only, separate from the tile
reads. `MapTileSource::waysEmitted()` already gives way counts
(`MapTileSource.h:90`); points are what is missing.

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

The tile range is deliberately wider than the screen — `kMarginPx` is 64 px on
top of a rotated bbox (`MapViewport.h:37`, `MapViewport.cpp:37-41`) — so a
large share of every tile's geometry is off-panel. Right now every point of
every way is projected, then the segments are clipped one at a time
(`GfxRendererCanvas.h:44`).

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

**open**: what fraction of ways in a real viewport are fully off-screen. Add a
counter next to `waysFiltered_` and read it off `info`. If it is under ~15%,
skip this step.

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

## Commit sequence

One concern per commit (`refactor-for-review` skill):

1. `perf(map): count points and time the render pass` — instrumentation only.
2. `perf(map): project way points in fixed point` — step 2.
3. `perf(map): clamp area scan lines to the screen` — step 4.
4. `perf(map): integer edge crossings in area fill` — step 5.
5. `perf(map): clip a thick road once, not once per copy` — step 6(2).
6. `perf(map): skip projecting fully off-screen ways` — step 3, if step 3's
   counter justifies it.
7. Revert the instrumentation from commit 1, or keep it behind the existing
   `LOG_DBG` level if the numbers stay useful.

Steps 6(1) and 7 are their own branches with their own device evidence.
