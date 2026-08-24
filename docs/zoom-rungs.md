# Zoom rungs

The zoom ladder, what each rung costs, and the three things that stop being
constants once the ladder gets long. Added 2026-08-12 with rungs 5 and 6.

The map spec in the parent repo (`docs/map-data-spec.md`) owns *why* zoom is a
ladder at all. This file owns what the rungs are on the device and what depends
on them.

## The ladder

`src/activities/map/MapViewport.h:117-126` is the table (line numbers drift as
the file grows — re-check with `grep -n kZoomLadder` before citing them again).
Ground metres per pixel, and the LOD each rung reads:

| rung | m/px | LOD | panel covers | marker scale | move floor |
|---|---|---|---|---|---|
| 0 | 1 | z13 | 0.5 x 0.8 km | 8/8 | 12 px |
| 1 | 3 | z13 | 1.4 x 2.4 km | 8/8 | 10 px |
| 2 | 6 | z12 | 2.9 x 4.8 km | 8/8 | 8 px |
| 3 | 12 | z11 | 5.8 x 9.6 km | 8/8 | 8 px |
| 4 | 20 | z11 | 9.6 x 16 km | 8/8 | 6 px |
| 5 | 32 | z11 | 15.4 x 25.6 km | 6/8 | 3 px |
| 6 | 45 | z11 | 24 x 40 km | 5/8 | 2 px |

Rungs 5 and 6 were added for the regional view a long ride wants -- the parent
repo's `docs/coarse-zoom-plan.md` has the case, the measurements and the
verdict. Short version: over the Malé Karpaty the panel at rung 6 holds the full
width of the range plus 40 km of its length, and the host renders showed the
picture stays readable on z11 data -- since confirmed on the panel, see
"Measured on hardware" below.

## Rung 4 was the top for a reason, and rungs 5-6 pay for going past it

A rung's m/px caps the LOD it can read. Worst case is a rotated viewport, so the
tile range must cover the screen diagonal plus label margin on both axes:
`933 + 2 x 64 = 1061 px` (`MapViewport.h:82`, `kMarginPx`). Fitting that in a
3x3 range needs the span inside two tile widths, so
`max m/px = 2 x tile_width_merc / 1061 x cos(lat)`:

| LOD | tile (Merc m) | max m/px at 48.4N |
|---|---|---|
| z11 | 19,568 | 24.5 |
| z10 | 39,136 | 49.0 |
| z9 | 78,272 | 98.0 |

**Computed, not measured.** Rung 4's 20 m/px is 82 % of what z11 can serve, and
rungs 5 and 6 are past it. They read z11 anyway, which costs tiles:

| rung | tiles loaded | bytes read | ways |
|---|---|---|---|
| 4 | 6 | 211,116 | 261 |
| 5 | 9 | 353,856 | 701 |
| 6 | 12 | 507,146 | 1,538 |

**Measured** 2026-08-12 on the host renderer (`test/map_preview`), Malé Karpaty
48.35N 17.30E, ride mode, tiles from the parent repo's `mapbuilder/cdn`. Same
run over Trnava, the dense case: 6 tiles / 290,541 B at rung 4, 9 / 420,824 B at
rung 6.

`kMaxTiles` went 9 -> 16 for this (`MapViewport.h:97`). It bounds tile
*metadata*, not tile data -- the renderer still streams one tile at a time
(`map-memory.md`) -- and the host RAM probe stayed at 6,736 B with zero heap
allocations at every rung. Device RAM after the change: 17.5 % (57,324 B),
unchanged in practice.

## Measured on hardware, 2026-08-12

Flashed and timed the same day, anchored on the Malé Karpaty (48.35N 17.30E),
ride mode, `tools/zoom_rung_timing.py` in the parent repo. The firmware's own
per-reset log lines are the instrument -- nothing was added to measure this.

| rung | m/px | tiles | bytes | ways | card | render | framebuffer ready |
|---|---|---|---|---|---|---|---|
| 4 | 20 | 6 | 374,530 | 549 | 843 ms | 1,303 ms | **1,310 ms** |
| 5 | 32 | 9 | -- | -- | -- | -- | **2,214 ms** |
| 6 | 45 | 12 | 786,748 | 7,478 | 1,758 ms | 3,635 ms | **3,646 ms** |

Two runs per rung, and the pairs agree to within 1 ms (1,311/1,309,
2,214/2,214, 3,646/3,647) -- this is a deterministic cost, not a noisy one.

**Verdict: rung 6 ships as it is.** 3.6 s is well inside what the ladder already
costs -- rung 0 is 7,463 ms, of which buildings alone are 4,122
(`MapViewport.h:29-36`). A rung that shows 24 x 40 km for half of rung 0's time
is a good trade, and a viewport reset is a rare event (`map-follow.md`).

Where the time goes at rung 6: landuse 1,598 ms, roads 1,488 ms, water 444 ms,
places 105 ms, with 1,758 ms of that inside the card. 63,018 points projected
against rung 4's 8,006. So it is geometry volume, not tile count -- which is
exactly what a z10 LOD would cut, and the reason to keep that option open
(`coord_shift`, parent repo's `docs/coarse-zoom-plan.md`). Expect roughly rung
4's cost at rung 6 if it is ever built; nothing else about the rungs changes.

**Open -- a discrepancy worth resolving.** The host preview reported 12 tiles
and 507,146 bytes for this viewport; the device read 786,748. Same rung, same
anchor, same tiles. Unexplained. Candidates: the wider places ring the device
opens for edge chevrons (`docs/map-data-spec.md`, "two tile ranges"), or the two
`bytesRead` accessors not counting the same thing. Until it is chased, quote
device bytes for device claims and preview bytes for preview claims -- do not
mix them in one table.

### The rung also decides how often the map is redrawn at all

Everything above is the cost of **one** reset. The rung also changes how many
resets a ride needs, and that turns out to matter more: replayed at step 2
(6 m/px, ride mode's own default) the same ride takes 30 re-anchors and 264
marker moves, against 14 and 113 at step 4 (20 m/px) -- 2.3x the panel time.
Measured off the device with `test/map_window`; the table, the confidence and
what would settle it are in
[`map-follow.md`](map-follow.md), "The default ride rung costs more than twice
what was measured".

## Raising the tile cap broke a bitmap, silently

`MapTileSource` memoises which `(tile, layer)` pairs have passed their crc32
this frame, and which are known corrupt. That was a bare `uint64_t`: 9 tiles x 7
layer slots = 63 bits, and every call site carried a `bit < 64` guard.

16 tiles is 112 bits. The guards made that *safe* and not *correct* -- past bit
63 the memo stopped recording, so:

- a layer would be crc-checked again on every `MapRenderer::kRoadPasses` pass,
  which is the exact work the memo exists to skip, and
- a layer already known corrupt would be streamed again instead of hatched.

Both land on the tiles a coarse rung adds, and neither shows on the panel. Now
an explicit 128-bit set, `src/activities/map/MapLayerBits.h`, with a
`static_assert` against `kMaxTiles` so the next cap raise fails the build
instead of quietly degrading.

## Four things that are per rung now

All three for the same reason, read in two directions: **a screen pixel is worth
more ground the further out the rung is** (1 m at rung 0, 45 m at rung 6), and a
fixed pixel object covers more ground the same way.

### 1. Marker size -- `ZoomStep::markerScale8`

`src/activities/map/MapMarkerMetrics.h` scales every marker length from the
full-size numbers. Full size is a 54 px ring, which covers 54 m at rung 0 and
2.4 km at rung 6 -- at that point the marker stops pointing at a place and
starts hiding one. Rungs 5 and 6 draw 6/8 and 5/8 (40 px and 33 px rings).

Strokes do not scale with the shape: the ring is 3 px at full size and 2 px
below it, because 2 px is the thinnest line worth having on this panel at arm's
length. `markerScaled()` clamps every length to at least 1 for the same reason.

What it does **not** buy is a cheaper refresh. **Measured** on the X4
2026-08-05: a windowed refresh costs the same ~500 ms whatever its area
(`map-follow.md`). A smaller marker shrinks the framebuffer read-back and
write-back either side of the move, which is memcpy, not waveform.

The patch buffer stays sized for the full marker (`kMarkerPatchBytes`, 720 B),
so one buffer serves every rung. `markerBoxDrawn_` records the box the marker on
the panel was actually painted with, and `markerRect()` erases with that rather
than with the live rung's -- a rung change re-anchors, so the two always agree,
and `moveMarker()` checks it instead of trusting the ordering.

**Not visible in `test/map_preview`.** The preview draws the *style's* puck
through `MapRenderer::drawMarker`; the device draws `MapActivity::
drawPositionMarker`, which is the mode-aware one this scaling lives in. That gap
predates this change (see the parent repo's `docs/device-preview.md`) and the
scaling does not close it -- judging the marker size needs the panel.

**Judged on the panel 2026-08-12** and it holds: parent repo's
`docs/device-shots/zoom-rung6-dubova-20260812.png` against
`zoom-rung4-dubova-20260812.png`, same fix, rungs 6 and 4. The 5/8 marker is
still unmistakably the marker at 45 m/px, and the full-size one at rung 4 shows
what it would have covered.

### 1b. Arrow shape -- generated from `marker-ride.svg`

Cycle and Ride's heading arrow (`MapActivity::drawPositionMarker`,
`MapActivity.cpp:516-533`) used to be a plain 3-point triangle: tip, and two
base corners at a separate `baseHalfW` constant per mode. Since 2026-08-24 it
is a 6-point polygon (tip, two shoulders, a concave tail notch, two base
corners) matching `src/components/icons/marker-ride.svg`.

`scripts/gen_marker_shape.py` reads that SVG's circle (the ring) and arrow
path, expresses every path vertex as a (right, forward) offset from the
circle's own centre in permille of the tip vertex's own reach, and writes
`MapMarkerMetrics.h`'s `MapMarkerShape.generated.h` (gitignored, a `pre:` step
in `platformio.ini` for both the firmware and simulator envs, same pattern as
`gen_mapstyle.py`). `MapActivity::drawPositionMarker` scales those permille
ratios by its runtime `tipLen` and rotates them with `kMarkerHeadingDir` --
same transform the old 3-point triangle used, just looped over more vertices.
Only `tipLen` is still a free per-mode knob (`kMarkerCycleTipLen`,
`kMarkerRideTipLen`); `baseHalfW` is gone, since the whole shape's proportions
now come from the SVG.

This keeps the arrow hand-drawn rather than a baked icon, per
`firmware/explorink/CLAUDE.md`'s icon rule: a glyph that rotates from a live
heading (nav direction arrow, compass needle) stays vector, because a static
bitmap cannot represent that without extra logic on top. `docs/icon-migration-plan.md`
does not cover this marker for the same reason.

`MapMarkerMetrics.h`'s `markerArrowFitsAtEveryRung()` static-asserts the
shape's farthest vertex (`kMarkerArrowMaxReachPermille`, also generated) stays
inside the marker's patch box at every rung -- same correctness bound
`markerHandFitsAtEveryRung()` already held for Hike's heading hand. A future
SVG edit that makes the arrow reach further than the ring's own halo now fails
the build instead of silently smearing the marker across the map on a move.

**Verified on the simulator, 2026-08-24** (no X4/X4 Pro/X3 on hand --
`project_hardware_gap_2026_08_22`): `pio run -e simulator`, seeded a fix, and
sent `tools/mapcmd.py --sim <host:port> pos <lat> <lon> heading <N>` (bare
`heading N` does *not* work -- `MapCommandParser`'s "position unchanged"
branch that updates `lastHeading_` only runs for a command that also carries
a position, `MapActivity.cpp:2296-2305`). The rendered shape is a pixel-exact
match to `marker-ride.svg`'s concave-notch chevron.

**Not verified: rotation on screen.** With no route loaded the map is
track-up, so `drawPositionMarker` is called with
`MapFollow::relativeHeadingStep(headingStep, anchorHeading_)`
(`MapActivity.cpp:4717`) and `anchorHeading_` re-anchors to match the incoming
heading every frame -- the relative step comes out ~0 regardless of the
heading sent, and the marker always draws pointing up *by design* in this
mode. Seeing the raw rotation on screen needs a loaded route (route-hold,
north-up mode), which has no console command to trigger in the simulator
today. The rotation *transform itself* is untouched from the old triangle
code (same `dir`/`perp` construction, more vertices), so this is a gap in
today's test coverage, not a known-open question about the math.

### 2. Move floor -- `ZoomStep::minMovePx`

`MapFollow::kMinMovePx` was 8 px at every rung: 8 m of ground at rung 0 and 360 m
at rung 6. The rider who could most use a steady trickle of updates got the
fewest of them.

Per rung it is 12, 10, 8, 8, 6, 3, 2 px, which holds the *ground* step roughly
level (12, 30, 48, 96, 120, 96, 90 m) instead of the pixel step. Bigger than the
old 8 at the near rungs on purpose -- at 1 m/px a refresh every 8 m is a lot of
waveforms for a marker crawling across the panel, and the maintainer prefers a
visibly bigger jump there.

**Unverified as a comfort call.** These are numbers to ride with and look at, not
numbers anything measured. The ground-step column is the invariant a test holds
(`test/map_tile_reader`, `CoarseRungsShrinkTheMarkerAndTightenTheMoveFloor`), not
the pixel values.

### 3. Place-name ceiling -- `ZoomStep::maxLabels`

Added 2026-08-12, with the labels themselves (`docs/place-labels.md`).

Same reasoning read a third time: how many names the panel deserves follows how
much *ground* is on it, not how many places happen to be in the tile range. Rung
0 shows 480 x 800 m -- one settlement, where a dozen names would be a dozen names
for one village. Rung 6 shows 24 x 40 km, where a dozen names is a map of the
region and the reason to be at that rung at all.

Per rung: 3, 4, 6, 8, 10, 12, 14. The style's own `max_labels` caps it again and
the smaller of the two wins -- the rung answers "how many does this much ground
deserve", the style answers "how many can the renderer afford"
(`MapLabels::draw`).

Maintainer's call 2026-08-12: "pri z0 urcite je zbytocne mat 12 labelov, pri z6
to uz moze mat zmysel". **Unverified as a comfort call**, like `minMovePx`. What
*is* measured is that the cap is not about time: five labels at rung 6 cost 94 ms
of a 3,793 ms frame.

### 4. Keep-in margin -- derived from the marker

`MapFollow::kKeepInMarginPx` is 80 px: one full-size 54 px ring plus 26 px of
slack, so the marker is always fully on screen and there is still map ahead of
the rider. With the marker no longer one size, `MapActivity` passes
`ring + kKeepInSlackPx` for the rung on the panel -- 80 px at rungs 0-4, 66 at
rung 5, 59 at rung 6.

Keeping 80 px at a rung whose marker is 33 px would fence off screen the rider
can see the marker on, and force a full-frame re-anchor for nothing.

`MapFollow` itself stays a pure integer decision with no dependency on the
ladder: both numbers arrive on `MapFollow::Request` (`minMovePx`,
`keepInMarginPx`), defaulting to the old constants for callers with no rung in
hand -- the same pattern `partialMoveBudget` already used, and what keeps
`test/map_follow` and `test/map_replay` free of viewport headers.

## What is still one number for every rung

- **The ghosting budget** (`kMaxPartialMoves`, 12). Ghosting is per waveform,
  not per pixel, so a rung with tiny moves has no claim to a bigger budget. At
  rung 6 that is a clean frame about every 1 km. Tuning it is on-device work
  (`docs/optimization/07-power-and-lifecycle.md`), unchanged by this.
- **The heading drift limit** (4 steps of 22.5 degrees). A turn is a turn at any
  scale.
- **The marker-height ladder**, five rungs (`kMapMarkerStepCount`). It was one
  constant with the zoom ladder until this change; the marker anchor is a screen
  position and five of them already span mid-screen to bottom edge, so it did
  not follow zoom to seven. Each is clamped against its own count in
  `CrossPointSettings.cpp:210-213` -- a stored zoom step of 6 survives a reload,
  a marker step of 6 does not.

## The console

`zoom 0..6` over BLE and USB (`MapCommandParser.cpp:13`, bounds taken off
`MapViewport::kZoomStepCount`, not repeated). `marker 0..4`, off the marker
count. `map_preview --zoom 0..6` likewise.

## The phone learns the rung too, as a distance

Since 2026-08-17, `MapActivity::sendViewportDiagonalIfChanged()`
(`MapActivity.cpp`, called from `renderViewport()` right after
`consoleState_.setZoomInfo()`) pushes one unsolicited line, `DIAG_M
<metres>`, over the BLE command channel whenever the rung changes and once
per reconnect. `<metres>` is `ladder[step].mpp * sqrt(screenWidthPx^2 +
screenHeightPx^2)` — the ground distance the screen's diagonal represents,
not the rung index and not a width, so a phone never needs this file's own
table, and it stays correct on a differently-sized or landscape panel
without a firmware or app change. Wire format and the reasoning for
diagonal-over-index:
`../../docs/ble-map-transfer-protocol.md`, "Viewport diagonal". What the
phone does with it: `../../docs/send-interval-analysis.md` (parent repo),
`android/README.md`, "Send policy".

Confirmed 2026-08-17: the `zoom N` console command triggers the same
`renderViewport()` call a button press does, so it is a valid way to test
this without touching the device.
