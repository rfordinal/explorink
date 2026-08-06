# The route layer

How the device loads a planned route and draws it. The file format is
`docs/route-file-spec.md` in the parent xteink repo; the feature around it is
`docs/route-layer-plan.md` there. This file is the device side: what the code
does, what it costs, and what has not been measured yet.

Written 2026-08-06, with the branch `feat/route-layer`.

**Verified on real hardware 2026-08-06.** Flashed to the X4, a route pushed over
BLE, picked from the picker, and the overview photographed off the panel:
`docs/device-shots/route-overview-first-frame.png` in the parent xteink repo.
The measured numbers are in "What it costs" below. Known-good build archived as
`docs/firmware-builds/feat-route-layer-a434acbf-good.bin` there.

## What happens, in order

1. The home menu's Map row opens the picker, not the map
   (`src/activities/home/HomeActivity.cpp`, `onMapOpen()`).
   `ActivityManager::goToRouteSelect()` (`ActivityManager.cpp:211`) checks
   `MapRouteStore::anyRoutes()` first and goes straight to the map when the card
   has no routes.
2. `RouteSelectActivity` lists `/trailink/trips/*.tir` plus a **Skip** row. Each
   row's name and point count come out of that file's header
   (`MapRouteStore.cpp`, `list()`).
3. Picking a route calls `ActivityManager::goToMap(path)`; Skip calls
   `goToMap()` with no path (`RouteSelectActivity.cpp`, `chooseSelected()`).
4. `MapActivity::onEnter()` loads it (`MapActivity.cpp:468`) and draws the route
   overview as the first frame (`MapActivity.cpp:496`).
5. Any button that asks for the ordinary map ends the overview. The route stays
   drawn as a layer in every frame after that
   (`MapActivity.cpp:1119`, `MapRenderer.cpp:235`).
6. The map menu's **Whole route** row draws the overview again at any time
   (`MapActivity.cpp:726`).

`CMD:GOTO_MAP` over serial still goes straight to the map with no route
(`goToMap()`'s default argument), so the parent repo's screenshot and preview
tooling is unaffected.

## The overview fit

`MapRouteFit` answers "which zoom rung and which heading show the most of this
route". Unit-tested on the host (`test/map_route/MapRouteTest.cpp`) and
**confirmed on the panel**: a 71-point route running north-east came out at
heading 2 and zoom step 3, whole route on screen, with the compass rotated to
match (device log, 2026-08-06).

One streaming pass over the points, 16 sets of accumulators, one per heading on
`MapHeading`'s grid. It measures the **point set**, not the bounding box: a route
running north-east has a bbox far larger than the strip it fills, and the screen
is 480x800 rather than square, so the heading changes what fits
(`MapRouteFit.h`, the header comment).

Two decisions worth knowing before changing it:

- **The 16 cos/sin pairs are computed once, in `begin()`.** No trig per point,
  the same rule `MapProjection` follows.
- **The accumulators are `float`, and that is only safe because points are
  accumulated relative to a centre.** Absolute Mercator metres reach 2e7, past
  float's 24-bit mantissa. Relative to the route's own bbox centre they stay under
  1e6 m, where a float still resolves 0.06 m (`MapRouteFit.cpp`, `addPoint()`).

### The tie-break is the direction of travel, and that is not cosmetic

Among headings that land on the same zoom rung, the fit picks the one nearest the
bearing from the route's start to its end (`MapRouteFit.cpp:153`).

This was found the hard way. Sorting on the smallest *required* metres-per-pixel
looks obviously right and is wrong: zoom is a five-rung ladder, so two headings on
the same rung render at **exactly the same scale**, and a smaller required value
buys nothing once it is rounded up. The first version did sort on it, and a plain
north-south route came out tilted 22.5 degrees — because a slight tilt trades
vertical extent the 744 usable pixels are short of for horizontal extent the 424
usable pixels have spare. Optimal by arithmetic, askew on the panel, for no gain
a rider can see. `MapRouteFit.TheTiltThatBuysNothingIsNotTaken` is the test that
pins it.

A closed loop has no direction of travel, and then the fit prefers north up —
the one orientation a rider can always read
(`MapRouteFit.cpp:82`, `MapRouteFit.AClosedLoopFallsBackToNorthUp`).

### When nothing fits

The coarsest rung is 20 m/px, which is 9.6 x 16 km of screen. A day's ride is
longer than that. Then `Result::fits` is false, the coarsest rung and the best
heading are used anyway, and the middle of the route is what lands on screen. The
frame says so out loud (`STR_MAP_ROUTE_PARTIAL`, `MapActivity.cpp`'s
`renderRouteOverview()`), because a frame showing the middle of a long route looks
exactly like one showing all of a short one.

The alternative would be a sixth ladder rung reading a z10 tile set that does not
exist — see `docs/map-data-spec.md` open question 1 in the parent repo.

### The overview is not a follow frame

- The anchor is the screen centre, not the marker-height ladder's rung. An
  overview has no rider position in it, so there is no look-ahead to reserve.
- **No marker is drawn.** A puck at the screen centre would claim the rider is
  standing in the middle of their own route.
- `viewportDrawn_` and `markerPatchValid_` stay false, so the first fix after the
  rider leaves the overview does a full reset rather than trying to move a marker
  that was never drawn (`MapActivity.cpp:1086` and the lines around it).
- **A fix arriving while the overview is up does not redraw.** It is recorded and
  held (`MapActivity.cpp:943`). The phone sends one every five seconds, so
  redrawing would snatch the overview away before it could be read, and pay a full
  refresh to do it.

## Drawing

`MapRenderer::drawRoute()` (`MapRenderer.cpp:80`) draws the polyline at
`style.routeWidthPx` and a filled arrowhead at the far end, oriented along the
last segment. It runs between the road fills and the place dots — the draw order
`docs/map-data-spec.md` fixes. Over the roads, because a route under a casing is
not a route; under the place dots, because a dot on the route is what render-spec
item 4 draws.

Nothing is buffered. Points stream from the file and each segment is drawn against
the previous point, so route length costs no RAM (`IMapRouteSource.h`). The
laptop preview measures this: **0 bytes of heap and 0 allocations during a render
with a 71-point route loaded** (`map_preview --route ... --fit-route`, measured
2026-08-06 on the host).

### Screen coordinates are `int32_t`, deliberately

`IMapRouteSource::nextRoutePoint()` hands out `int32_t`, and
`MapProjection::projectMercWide()` produces it. A route is one object that can be
200 km long, so in follow mode at 1 m/px its far end is 2e8 pixels off screen —
which wraps an `int16_t` into a line drawn straight across the middle of the map.
`GfxRendererCanvas::drawLine()` clips per segment, so an honest off-screen
coordinate costs nothing while a wrapped one is a wrong picture.

Tile geometry keeps `int16_t`: a tile range is only ever 3x3 around the anchor.

## What it costs

| item | bytes | where |
|---|---|---|
| `MapRouteSource`, including its reader's 1 KB stream buffer and the fit's accumulators | ~1.6 KB heap | allocated in `onEnter()` **only when a route was picked** |
| `RouteSelectActivity`'s row list, 24 entries | ~2.8 KB heap | freed in `onExit()` |
| route bytes read per viewport reset | 8 per point | one sequential pass, `MapRouteReader::beginPoints()` |

A 3,000-point route is 24 KB on the card and 24 KB read per viewport reset. Against
the 181 KB a four-tile reset already reads for geometry, that is around 13 %.
**Read off the code** — the number to check is `MapRouteSource::bytesRead()`
against `MapTileSource::bytesRead()` in the same frame.

**Measured on hardware 2026-08-06**, 71-point route, two z11 tiles:

- **the whole overview frame took 2,537 ms**, tile reads and the e-ink refresh
  included (`route overview: heading 2, zoom step 3, 2 tiles, 2 missing, 2537 ms,
  whole route`). That is the same order as any other viewport reset, which pays
  about 1,800 ms for the refresh alone — the route did not add a cost worth
  seeing.
- **heap delta 0 across the tile load** in the frames after it
  (`heap: 55432 before tile load, 55432 after, delta 0`). The streaming claim
  holds on the device, not only on the host.

`points_crc32` is checked **once**, when the route is loaded
(`MapRouteReader::verifyPoints()`, `MapRouteReader.h:98`), not per reset. The file
on the card does not change underneath a session, and re-reading it purely to
re-confirm a checksum would double what the route costs per frame. The header crc
*is* checked on every `open()`, because `point_count` comes out of those bytes.

Firmware size with this branch: **RAM 17.6 %, flash 58.5 %** (`pio run`,
2026-08-06). RAM is unchanged from `develop` — everything the route adds is
allocated on demand.

## Open, and what would settle it

- ~~**Not run on hardware yet.**~~ Done 2026-08-06: pushed over BLE, listed by the
  picker, picked, overview drawn and screenshotted. The route file landed on the
  card through the **existing** transfer path with no firmware change, which was
  the plan's one untested assumption.
- **The route is 8 px wide and the widest road is 10 px**
  (`scripts/gen_mapstyle.py` warns about it on every build). Width is the only
  thing telling them apart on 1-bit e-ink, so the style needs a pass in the
  webapp. Judge it on a `map_preview --route` render, not by arithmetic.
- ~~**How long a viewport reset takes with a route loaded** is unmeasured.~~
  Measured: 2,537 ms for the overview frame, see above. Still unmeasured for a
  *long* route — the test route is 71 points, and a 3,000-point one reads 40x the
  bytes.
- **A route that crosses a tile-coverage gap** draws over the hatch, because the
  route pass runs before the hatch (`drawMapLayers()`, `MapActivity.cpp:1098`).
  The verification frame happened to include a real gap — two of its four z11
  tiles were missing, hatched down the left of the panel — and the route stayed
  readable over it. Not yet seen with the route itself crossing into the hatched
  area, which is the case that matters.
- **No console command.** `route` / `overview` would let the fit be exercised over
  USB serial without touching the picker. The menu row covers it for now.
- **A ladder press before the first fix drops the overview to the waiting
  banner.** Any button asking for the ordinary map ends the overview
  (`renderViewport()` / `renderWaiting()`), and with no fix yet the ordinary map
  is the "waiting for Bluetooth" banner. The step is still stored and the menu's
  Whole route row brings the overview back, so nothing is lost -- but a rider who
  presses zoom out of curiosity before their phone connects gets a blank screen
  for it. Left as is on purpose: the alternative is a zoom button that visibly
  does nothing, since the overview picks its own rung. Worth revisiting if it
  annoys anyone in practice.
