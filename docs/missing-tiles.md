# Missing tile tracking

`MissingTilesStore` (`src/MissingTilesStore.h`, `src/MissingTilesStore.cpp`)
records which map tiles the device tried to open and could not, across
restarts, with a hit count per tile. Nothing on the device fetches a tile
over the network -- this is purely a record on the SD card of what a real
ride actually needed, so a laptop tool can later prioritise which tiles to
build (`docs/roadmap.md`, "Public tile CDN").

## Where a hit comes from

`MapTileSource::unavailableMask()` already knows which tiles in the current
viewport are absent, truncated or crc32-mismatched
(`src/activities/map/MapTileSource.h:69-84`), but it is cleared on every
`begin()` call -- nothing accumulates it across resets or a power cycle.
`MapActivity::renderViewport()`'s existing hatch loop, which already walks
that mask tile-by-tile to draw the hatch pattern, is the one place that knows
each missing tile's real `(z, col, row)`. It calls
`MissingTilesStore::record()` for each one (`src/activities/map/MapActivity.cpp:774`).

## Storage format

`/.crosspoint/missing_tiles.json` on the SD card:

```json
{
  "tiles": [
    {"z": 14, "col": 8721, "row": 5632, "count": 3}
  ]
}
```

Capped at 200 entries (`MissingTilesStore::kMaxEntries`,
`src/MissingTilesStore.h:41`). Past the cap, a new tile evicts whichever
entry has the lowest count so far (`src/MissingTilesStore.cpp`, `record()`)
-- a tile seen once and never again is the least useful entry to keep.

## Write policy: rate-capped, and only on a real list change

Bumping an already-known tile's count does **not** by itself schedule an SD
write. Only a tile's *first* appearance (or an eviction swap) marks the store
dirty (`MissingTilesStore::dirty_`, `src/MissingTilesStore.h`). This matters
because a rider parked at the edge of coverage re-renders the same missing
tile on every viewport reset -- without this distinction every one of those
resets would look like a reason to write.

`MapActivity` arms its own timer the moment `isDirty()` first goes true after
a flush, and does not push it out further while more tiles keep arriving
(`src/activities/map/MapActivity.cpp:785-787`):

```cpp
if (MISSING_TILES.isDirty() && missingTilesSaveDueMs_ == 0) {
  missingTilesSaveDueMs_ = millis() + kMissingTilesSaveIntervalMs;
}
```

`kMissingTilesSaveIntervalMs` is 10 minutes (`src/activities/map/MapActivity.cpp:44`).
So: at most one SD write per 10 minutes of active list changes, not a settle
delay that resets on every new tile (that would mean a long coverage gap
never actually saves). `loop()` flushes when the deadline passes
(`src/activities/map/MapActivity.cpp:498-501`); `onExit()` always flushes
once, unconditionally, as a leave-the-screen checkpoint
(`src/activities/map/MapActivity.cpp:392`) -- `flushIfDirty()` is a no-op
when nothing changed, so this costs nothing on a session with no misses.

This is the same idea as `saveLaddersIfChanged()`'s debounce for ladder
settings, just on its own, much longer schedule and a different trigger
(list membership, not "some time since the last edit").

## Getting the list off the device

**Not reachable via WebDAV / the WiFi file manager.** `WebDAVHandler::isProtectedPath()`
rejects any path with a segment starting with `.`, `/.crosspoint/...`
included (`src/network/WebDAVHandler.cpp:774-782`) -- verified by reading
the guard, not tested against a live server. Today the only way to read
`missing_tiles.json` is pulling the SD card, the same channel already used
for a full base-map preload (`docs/roadmap.md`, "three channels"). A BLE
console command or a WebDAV carve-out would be needed for an over-the-air
pull; neither exists yet.

## Verified vs assumed

- **Verified**: compiles clean (`pio run`, default env, 2026-08-05); RAM cost
  is 200 x `sizeof(MissingTileHit)` (~16 bytes with padding) plus
  `std::vector` overhead, well inside headroom (build reported 17.6% DRAM
  used overall). Read off the code, not measured on hardware: the
  `isProtectedPath` block above, and the throttle's actual behaviour on a
  real ride.
- **Not verified**: never run on the device. No SD card has an actual
  `missing_tiles.json` to inspect yet.
