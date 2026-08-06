# Plan 09 — progressive render (two refreshes per reset)

Show the base map first, add the buildings second. The rider is oriented by the
first picture and does not wait for the second.

Proposed 2026-08-06 after a **14 second** viewport reset on a dense tile, reported
from the device. This plan says how it would be built, what it costs, and — the
important part — **the condition under which it is worth building at all**.

## Step 0 — a frame that says "loading" before the tiles are read

Raised by the maintainer 2026-08-06, and it is both cheaper than everything below
and independent of it. Build this one first, whatever happens to the rest.

**The problem, measured.** Entering the map screen with a persisted fix calls
`renderViewport()` straight from `onEnter()` (`MapActivity.cpp:460`), and nothing
reaches the panel until `displayBuffer()` at the very end of it
(`MapActivity.cpp:1118`). E-ink holds the last image, so the panel keeps showing
**the previous screen** — the home menu — for the whole read. Measured on
hardware: **10.9 s at rung 0, 15.3 s at rung 1** (see the baseline table in
[01-render-pipeline.md](01-render-pipeline.md)). No badge, no text, nothing: the
device looks broken for fifteen seconds.

`renderWaiting()` already draws a text frame and refreshes it
(`MapActivity.cpp:777-788`), but only on the path where there is no last fix
(`:461-463`). The path that takes 15 s is the one with no feedback.

**The fix.** Draw a status frame and refresh it *before* the tile work starts, on
entry. Cost: one 500 ms refresh, and the first pixel of feedback arrives in tens
of milliseconds instead of fifteen seconds.

It has none of the hazards the rest of this plan carries: no draw-order question,
no additive-ink requirement, no marker-patch problem — the tile render that
follows clears the screen and draws the whole frame exactly as it does today.

Two open choices, both cheap:

- **What it says.** `STR_MAP_WAITING_BLE` is the existing waiting string and is
  about BLE, not tiles, so this needs its own `tr()` key. Whether it also shows
  the coordinate and the rung is a judgement call — that information is what makes
  the wait legible rather than merely acknowledged.
- **When it fires.** Only on `onEnter()` (the 15 s case, and the only one with no
  feedback at all), or before every viewport reset. A zoom press already gets the
  busy badge through `showBusy()` (`MapActivity.cpp:279-300`), so entry is the
  gap. Start with entry only.

The second half of the maintainer's point — **not being dependent on tiles to draw
a map screen at all** — is a bigger idea and belongs with the phases below: the
furniture (compass, marker, hints, scale) costs no tile read, so a frame carrying
only that is available almost immediately. That is the natural phase A if this
plan ever grows a third tier, and it is strictly better than a text screen because
it is already the real screen, just without the map in it.

## What it does not do

It does not make a reset faster. The total work is the same; the panel just shows
a usable picture part-way through. Everything in plans
[01](01-render-pipeline.md) and [02](02-tile-io.md) makes a reset genuinely
shorter, and both are measurable, pixel-identical and smaller than this. **Do
them first.**

## The trigger condition

Build this only if, after plan 01 commits 1-5 and plan 02 steps 1-3 have landed,
**some zoom rung still takes more than about 5 seconds** on a dense tile.

The reason is arithmetic. Plan 01's measurement says 99.6 % of the buildings
walked at rung 0 never produce a pixel, and plan 02 says every drawn layer is
read off the card twice. If those two are fixed, the frame that costs 14 s today
is doing a small fraction of that work, and splitting a 2-second frame in two to
save a second of perceived latency is not worth a second visible state, a second
waveform pass and the marker-patch hazard below.

If a rung is still slow after that, this plan is the right next move, because at
that point the remaining cost is real work on geometry that is actually on the
screen, and no further optimisation makes the buildings free.

## The cost side

**measured 2026-08-05** (`MapActivity.cpp:833-838`, `docs/map-follow.md`): a
panel refresh takes **500 ms** whatever its area — the waveform is a fixed cost
and a window only narrows what it touches. 62 windowed marker moves and 26 full
frames in one replayed ride all took the same 500 ms.

So a second phase costs **+500 ms of panel time and one more waveform pass per
reset**, plus the battery for it. Against 14 s that is noise. Against 2 s it is
25 %.

## The draw-order constraint, which is the whole design

Draw order is fixed and is a correctness requirement, not a preference: built-up
area, green area, water, **buildings**, road casings, road fills, places
(`MapRenderer.cpp:72-166`, `docs/map-render-spec.md`). Buildings sit **under**
the roads. So "base first, buildings second" means drawing buildings **onto**
finished roads, and whether that is safe depends entirely on what ink the
building style writes.

### Buildings must be additive-black only

**read.** With today's style — `fill: hatch`, `hatch: "XXXX"`,
`outline_width: 0` (`data/mapstyle.json`, `layers.buildings.rule`) — a building
is drawn by `hatchRing` and `outlineRing`, both of which emit
`drawLine(..., MapInk::Black)` (`MapAreaFill.cpp:87-89`, `:179`). Black only.
Nothing is erased, so a late building pass leaves every road pixel alone.

**The hazard is `fill: tone`.** A tone goes through `fillSpan`
(`MapAreaFill.cpp:170`) to `fillRectDither`, and a dithered fill **writes both
inks**: `row[i] = (row[i] & ~mask) | (mask & whiteMask)`
(`GfxRenderer.cpp:1030-1052`, and the comment there states it outright). A
building drawn with a tone after the roads would punch white holes in them.

Therefore: **this plan requires the building rule to stay hatch-or-outline.** If
someone switches buildings to a tone, the second phase must go back to being
part of the first, or the roads must be redrawn after it. Put that condition in
[`../map-style.md`](../map-style.md) next to the building rule if this plan is
ever built. (Done 2026-08-06: it is there.)

### The one visual change it does make

Building hatch drawn after the roads lands inside the **white interior of a road**
wherever a building polygon and a road stroke overlap — which happens, because
road strokes are up to 6 device px wide and buildings sit right against them.
Today the road's white fill covers that hatch; reversed, it does not.

Small, but it is a real change to the picture. It is checkable with no flash: the
webapp's `firmware` panel runs the same `MapRenderer`
(`docs/device-preview.md` in the parent repo), so a before/after pair of that
panel is the evidence, plus one look on glass.

### The marker patch must be taken after the last phase

`renderViewport()` saves the marker's background patch immediately before drawing
the marker, and the comment says why: everything the marker can sit over must
already be in the framebuffer, or restoring the patch later erases the marker
onto a background that never existed (`MapActivity.cpp:1092-1102`).

A naive split breaks exactly that. If phase A saves the patch and refreshes, and
phase B then hatches buildings across the marker box, the saved patch no longer
matches the panel and every following marker move restores a stale background.

So the order is fixed:

```
phase A   clear, landuse, water, roads, places, hatch, compass, hints, marker
          refresh                                   <- rider is oriented here
phase B   buildings
          redraw marker (hatch is additive black and can dirty its white ring)
          save marker patch                         <- only now
          refresh
```

The marker is drawn twice and the patch is saved once, at the end. Follow mode
must stay off between the two refreshes — `viewportDrawn_`
(`MapActivity.cpp:1114`) is the existing flag for exactly that state, so set it
after phase B, not after phase A.

## What it needs from the source

Nothing new. `IMapSource` is rewindable per layer and the renderer already makes
seven `begin*()` passes per frame (`IMapSource.h:22-27`, plan 02's table). A
buildings pass that runs after the roads passes is one more pass in a design that
is already pass-outer, not a new kind of cost.

Shape: split `MapRenderer::render()` into `renderBase()` and
`renderBuildings()`, both taking the same arguments, with `render()` kept as
`renderBase(); renderBuildings();` so the golden test and the native preview keep
one entry point and one output.

## Gates

- `test/map_preview`'s golden render, through the unsplit `render()`, must be
  **byte-identical**. The split itself changes no pixel; only the device's
  two-phase call order does.
- A `firmware`-panel before/after pair showing the road-interior hatch overlap,
  reviewed as a style decision.
- Device: `%lums` on debug line 2 is the framebuffer time and will barely move —
  that is expected and is not the point. The number to record is **time to the
  first refresh**, which needs its own `millis()` readout.
- Both refreshes must be `FAST_REFRESH` (`MapActivity.cpp:1118`). Two full
  refreshes per reset is not this plan.

## Rejected: grey buildings instead of a second phase

Drawing buildings in a grey plane rather than a second pass was considered and
does not work. `GrayscaleFrame::render()` invokes its draw callback **13 times**
on this panel (`GrayscaleFrame.h:102-118`), and the map's callback is a streaming
pull off the SD card that holds no geometry (`IMapSource.h:9-27`) — so a grey map
frame is 13 full tile reads, not one extra waveform pass. Full reasoning in
[01-render-pipeline.md](01-render-pipeline.md), step 8. It needs a bounded
display list first, which is the thing `IMapSource` exists to avoid.
