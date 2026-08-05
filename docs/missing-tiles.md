# Missing tile tracking

`MissingTilesStore` (`src/MissingTilesStore.h`, `src/MissingTilesStore.cpp`)
records which map tiles the device tried to open and could not, across
restarts, with a hit count per tile. The device never fetches a tile itself --
this is a record of what a real ride actually needed, so whoever builds tiles
can prioritise (`docs/roadmap.md`, "Public tile CDN"). The list is readable
over the map console (`missing`, below), which is what lets a phone ask the
device what it is short of and push those tiles back
(`docs/ble-map-transfer-protocol.md`).

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

## Getting the list off the device: the `missing` command

`missing [<offset>]` on the map console prints the persisted list
(`src/activities/map/MapCommandParser.cpp`, `parseMissing()`;
`src/activities/map/MapCommandConsole.cpp`, `MapConsoleState::writeMissing()`).
Same command over both channels the console already has -- USB serial and the
BLE command characteristic -- because both feed the one shared
`MapConsoleState` (`src/activities/map/MapCommandConsole.h`).

`tiles` and `missing` are different things. `tiles` reports the current
viewport, at most 9 entries. `missing` reports every tile the device has ever
hatched, up to 200.

```
> missing
INFO missing_total=25
INFO missing_offset=0
INFO missing_12_2199_1416=7
...
INFO missing_next=20
OK
> missing 20
INFO missing_total=25
INFO missing_offset=20
...
INFO missing_next=done
OK
```

One entry per line, `missing_<z>_<col>_<row>=<count>`. `missing_next` is where
the next command should start, or `done`.

**Paged at 20 entries** (`MapConsoleState::kMissingPageSize`). Every reply
line is one BLE indication and each one waits for the peer's ATT confirm
before the next goes out (`BlePositionServer::sendCommandReply()`), so 200
lines from a single command would hold the map activity's `loop()` for
minutes. An offset past the end is an empty page, not an error -- that is what
makes a paging loop's last request harmless.

`INFO missing=unavailable` means no store was wired to the console, which is
deliberately distinct from `missing_total=0`: a reader must not mistake a
build that never connected the two for a device that needs no tiles.

The console half never includes `MissingTilesStore.h` (ArduinoJson plus the
SD-backed `PersistableStore`, neither of which the native tests link).
`MapActivity` implements a small `IMissingTilesSource` adapter over the store
instead (`src/activities/map/MapActivity.cpp`), and the console pulls entries
through it by index rather than being handed a copy -- a copy of up to 200
entries would sit in DRAM and go stale the moment another tile hatched.

### Fetch priority: which tiles a reader gets first

Page 0 of a listing sorts the list into fetch priority
(`MissingTilesStore::sortByFetchPriority()`, policy in
`src/MissingTilePriority.h`); later pages walk the order page 0 fixed. A
fetch can be cut short -- the rider rides off, the battery goes, the phone
walks out of range -- so whatever finished should already be the useful part.

Primary key is the LOD tier: **regional (z12) first, overview (z11) second,
detail (z13) last**. Ride mode is the primary use case and opens at zoom step
2, which is regional (`src/activities/map/MapRideMode.h:27`,
`src/activities/map/MapViewport.h:28-34`), so that tier is the view the rider
actually looks at by default. Secondary key is hit count, descending, inside a
tier. A tile hatched once at regional therefore still beats a tile hatched
fifty times at detail: the tier says "the rider's normal view is incomplete
here", the count only says "a close-up would have been nice".

col/row break the last tie, so the order is total and the same list pages the
same way twice (`std::sort` is not stable).

Tuning knob, not a law: if a detail tile hatched constantly should ever
outrank a regional tile hatched once, the two keys collapse into one weighted
score instead of this strict lexicographic pair.

The sort does not mark the store dirty. Entry order in the JSON carries no
meaning -- `fromJson()` reads it back positionally -- so a reorder is not new
information and does not earn an SD write.

## Getting the file itself off the device

**Not reachable via WebDAV / the WiFi file manager.** `WebDAVHandler::isProtectedPath()`
rejects any path with a segment starting with `.`, `/.crosspoint/...`
included (`src/network/WebDAVHandler.cpp:774-782`) -- verified by reading
the guard, not tested against a live server. So the raw
`missing_tiles.json` still needs an SD card pull, the same channel already
used for a full base-map preload (`docs/roadmap.md`, "three channels"). The
`missing` command above is the over-the-air path and does not need the file.

## Verified vs assumed

- **Verified**: the `missing` command's grammar, paging, `unavailable` case and
  page-0-only ordering, plus the priority policy itself, are covered by native
  tests (`test/map_command_parser/MapCommandParserTest.cpp`,
  `test/missing_tile_priority/MissingTilePriorityTest.cpp`, 52 tests green
  2026-08-05). Those run on the host, so they prove the logic, not the
  channel.
- **Verified**: compiles clean (`pio run`, default env, 2026-08-05); RAM cost
  is 200 x `sizeof(MissingTileHit)` (~16 bytes with padding) plus
  `std::vector` overhead, well inside headroom (build reported 17.6% DRAM
  used overall). Read off the code, not measured on hardware: the
  `isProtectedPath` block above, and the throttle's actual behaviour on a
  real ride.
- **Not verified**: never run on the device. No SD card has an actual
  `missing_tiles.json` to inspect yet, and no `missing` command has been sent
  over either real channel. The open question is the BLE one: 20 indications
  back to back, each waiting for its ATT confirm, has not been timed -- if a
  page takes long enough to stall the map's `loop()`, `kMissingPageSize` is the
  knob. Sending `missing` over USB serial and over BLE (`tools/blereplay.py`
  in the parent repo) against a card with a real list would settle both.
