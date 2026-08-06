# Missing tile tracking

`MissingTilesStore` (`src/MissingTilesStore.h`, `src/MissingTilesStore.cpp`)
records which map tiles the device tried to open and could not, across
restarts, with a hit count per tile. The device never fetches a tile itself --
this is a record of what a real ride actually needed, so whoever builds tiles
can prioritise (`docs/roadmap.md`, "Public tile CDN"). The list is readable
over the map console (`missing`, below), which is what lets a phone ask the
device what it is short of and push those tiles back
(`docs/ble-map-transfer-protocol.md`).

> **Optimisation review, 2026-08-06.**
> [`optimization/04-tile-sync.md`](optimization/04-tile-sync.md) reviews this
> store and the `TileSyncActivity` that consumes it. Three open items matter
> before touching either: a sync that goes quiet never ends, completion counts
> landed *files* rather than settled rows, and arrivals on the map screen no
> longer clear the list at all (a regression from `412e0ed9`).

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

## Asking the phone to fill the gaps

**Sync map tiles** in the home menu, which opens `TileSyncActivity`
(`src/activities/map/TileSyncActivity.{h,cpp}`). What happens, in order:

1. The store is sorted into fetch priority and its size is the fetch's total.
2. The skip tally is cleared, so the failure count on screen belongs to this
   fetch (`MapConsoleState::clearSkips()`).
3. The screen waits for a phone to subscribe to the command characteristic, and
   says so. When one does, the device sends `NEED_TILES <count> fmt <version>`
   as an **unsolicited indication** -- the one place the device starts a
   conversation instead of answering one.
4. The phone pages the list with `missing` and pushes tiles back over the
   transfer channel (`docs/ble-map-transfer-protocol.md` in the parent repo).
5. The progress screen counts arrivals and skips until they add up to the
   total.

### Waiting for the phone is a state, not a failure

`sendCommandReply()` cannot tell you whether anybody is listening: NimBLE
accepts an indication into its one-slot queue with nobody subscribed, so its
return value is not evidence a phone heard anything. Measured on hardware
2026-08-06 -- the screen logged "asked for 29 tiles" into an empty room and then
sat at 0 of 29 with nothing to explain itself.

So the subscription is tracked directly (`BlePositionServer::isCommandSubscribed()`,
an `onSubscribe` callback because NimBLE-Arduino has no `getSubscribedCount()`),
and the screen has a real Waiting phase: it says it is waiting, says the channel
is Bluetooth, and says what would make it start. The ask goes out when a phone
subscribes, so opening the screen before the app is ready is the normal case
rather than a miss. A phone that walks off mid-sync puts it back to waiting
instead of leaving a bar that will never move again -- and "no phone has ever
turned up" and "the phone was here and left" get different words, because one is
a setup problem and the other is range.

The subscription is also cleared on disconnect. NimBLE does not fire
`onSubscribe(0)` when a link simply drops, so without that the screen would
believe a phone was there long after it walked away.

An empty list says "no missing tiles" and stops there: the rider picked the menu
item and is owed an answer.

### The format version is part of the ask

`fmt <version>` in `NEED_TILES` is `MapTileReader::kFormatVersion`
(`src/activities/map/MapTileReader.h`) -- the one `.tib` version this build
reads, checked for exact equality in `parseHeader()`
(`src/activities/map/MapTileReader.cpp:85`). `info` reports the same number as
`INFO tile_fmt=<version>`, so a builder can ask without starting a fetch.

Without it the feature quietly defeats itself. A tile built to another version
transfers fine, passes CRC32 and gets renamed into place; the transfer reports
`OK`, so `drainTransferredTiles()` drops the entry from the list. Then the next
render opens the file, the reader refuses it on the version, the map hatches the
same square, and `record()` puts it straight back on the list. The next fetch
asks for the same tile again. **The waste repeats every fetch, not once.**

The device does not verify the header of an arriving tile before clearing its
entry, and deliberately so: the receiver deals in bytes and paths, not tiles
(the same channel carries route and style pushes), and re-opening every arrival
to parse a header would put an SD read on the arrival path for a case the
`fmt` handshake already covers. The system is self-correcting either way -- an
unreadable tile is re-hatched and re-recorded on the next render, so the list
never keeps a lie for long. If `fmt` ever has to be *enforced* device-side
rather than trusted, validating the header in `drainTransferredTiles()` before
`forget()` is the place.

### `skip <z> <col> <row> [<reason>]`

The phone saying it cannot supply a tile
(`src/activities/map/MapCommandParser.cpp`, `parseSkip()`). Without it the
progress screen would wait for a file that is never coming.

A skip is **counted, not acted on**: the tile is still missing, so it stays on
the list. Only an arrival removes an entry. `<reason>` is one free-form word
for the log -- the screen shows a count.

### An arriving tile clears its entry

`MissingTilesStore::forget()`, driven from
`MapActivity::drainTransferredTiles()`. A landed file's path is the only place
the transfer knows which entry it answered, so `MapTransferReceiver` parses it
(`src/activities/map/MapTilePath.h`) and publishes the coordinate; a non-tile
push -- a route, a style -- parses false and clears nothing.

**The removal happens on the activity task, not where the file lands.** The
store's other writer is `record()` from `renderViewport()`, i.e. the activity
task, and `std::vector` with a second writer on the NimBLE host task is
corruption waiting for a coincidence. So the host task publishes
`Status::lastTile` plus a `tileSeq` counter and the activity task acts on the
change (`src/activities/map/MapTransferReceiver.h`). Two tiles landing between
two `loop()` iterations would collapse into one removal; a file takes seconds
and `loop()` runs continuously, and the cost would be one stale entry the next
fetch asks for again.

Unlike a reorder, a removal **does** mark the store dirty. A list that still
asks for tiles already on the card would have the phone send them all again
after a restart.

### Its own screen, off the home menu -- not a map-menu item

It started as a third item in the map screen's CONFIRM menu, because the map
screen owns the BLE peripheral (`onEnter()`: `begin()` then `attach()`;
`onExit()`: `detach()` then `end()`) and that was the only place with a live
link to build on. That was the wrong reason to put it there. Filling coverage
gaps is preparation -- it happens at home, over a phone that has the tiles on
it, before a ride. Nobody stops mid-trail to sync map data.

So `TileSyncActivity` starts its own BLE, attaches its own
`MapTransferReceiver`, and carries its own `MapConsoleState` plus a BLE console
over it (the phone answers in the same ASCII: `missing` to read the list, `skip`
to give up on a tile). The map screen went back to being a map. Recording still
happens there -- `renderViewport()` calls `record()` on every hatched tile --
so it is record on the trail, fetch at home, and the two are never on screen
together.

The console state is deliberately **not** shared with the map's. Two screens are
never up at once, and sharing would put this screen's skip tally and the map's
zoom in one object for no reason.

### One bar per tile

A single bar for the batch hides what matters: which tile is moving, which is
stuck, which ones the phone refused. So the screen is a list, one row per tile,
each with its own progress bar, in the fetch-priority order the device sent. It
is also the shape parallel transfers would want -- more than one row simply
shows movement at once.

**Row state is derived, not accounted for.** A second ledger kept in step with
the store is how the two drift apart, so each row asks:

- the receiver's `Status::activeTile` says this row is on the wire, and
  `received` over `total` is its bar
- the skip observer (`IMapSkipObserver`, called synchronously from
  `MapConsoleState::execute()`) says the phone gave up on it
- otherwise, gone from `MissingTilesStore` means it arrived -- `forget()` is the
  only thing that removes an entry, and only an arrival calls it
- still in the store means still waiting

The one thing that has to be remembered is the **order**, because the store
shrinks as tiles land. `rows_` is a snapshot of the priority order taken when
the sync starts, so a row never moves under the rider's eyes. Heap-allocated
(~2.4 KB at the 200-entry cap) and freed in `onExit()`; this screen allocates no
`MapTileSource`, so it is still the cheaper of the two map-side screens.

`skip` needs a per-tile callback rather than the `MapSkipTally` snapshot: two
skips between two polls would leave a row stuck on "waiting" forever.

### Refresh cadence

A tile settling -- landed or skipped -- repaints the whole frame: the summary
line, one row's state and possibly the window all change.

The bytes of the transfer in flight climb continuously, and every repaint is a
real waveform pass. So the moving bar is rate-capped at
`kActiveRowRefreshMs` (2 s) and repaints **only its own row's rectangle**
through `renderer.displayBufferWindow()`, the same mechanism as the map's busy
badge. Per chunk would spend the transfer refreshing instead of receiving.

BACK cancels. The device cannot stop the phone from its end -- the transfer
protocol's abort opcode (`0x03`) is a frame the *central* writes -- so the
cancel is `FETCH_CANCEL` on the command channel.

## Getting the file itself off the device

**Not reachable via WebDAV / the WiFi file manager.** `WebDAVHandler::isProtectedPath()`
rejects any path with a segment starting with `.`, `/.crosspoint/...`
included (`src/network/WebDAVHandler.cpp:774-782`) -- verified by reading
the guard, not tested against a live server. So the raw
`missing_tiles.json` still needs an SD card pull, the same channel already
used for a full base-map preload (`docs/roadmap.md`, "three channels"). The
`missing` command above is the over-the-air path and does not need the file.

## Verified vs assumed

- **Verified on hardware, over USB serial (2026-08-06, build
  `develop-41e1a6e3`).** Gaps were made on purpose by sending `pos` to empty
  country at three zoom steps, then reading the list back:
  - `INFO tile_fmt=2` in `info`.
  - Recording works across viewport resets and across LODs: 7 entries after
    three resets, 29 after thirteen.
  - **Priority order is what it claims.** With 7 entries the reply was 4x z12
    (regional) first, then z11 (overview), then 2x z13 (detail) -- and inside
    z12, all at count 1, ascending by col then row, which is the total-order
    tiebreak doing its job.
  - **Count-descending inside a tier**: after a tile was hatched twice,
    `missing_13_4496_2826=2` sorted ahead of every count-1 z13 entry.
  - **Paging**: at 29 entries, page 0 printed exactly 20 and answered
    `missing_next=20`; `missing 20` printed the remaining 9 and
    `missing_next=done`. Order held across the page boundary.
  - **An offset past the end** answers `missing_total=29`,
    `missing_offset=20`, `missing_next=done`, `OK` -- an empty page, not an
    error, which is what makes a paging loop's last request harmless.
- **Verified on the host**: the `unavailable` case, page-0-only ordering, the
  `skip` verb, its tally and its per-tile observer, the priority policy in isolation and the tile-path
  parse (`test/map_command_parser/MapCommandParserTest.cpp`,
  `test/missing_tile_priority/MissingTilePriorityTest.cpp`,
  `test/map_tile_path/MapTilePathTest.cpp`; 247 tests green across the whole
  suite, 2026-08-06).
- **Verified**: compiles clean (`pio run`, default env, 2026-08-05); RAM cost
  is 200 x `sizeof(MissingTileHit)` (~16 bytes with padding) plus
  `std::vector` overhead, well inside headroom (build reported 17.6% DRAM
  used overall). Read off the code, not measured on hardware: the
  `isProtectedPath` block above, and the throttle's actual behaviour on a
  real ride.
- **Not verified.** Everything above went over USB serial. The BLE half and the
  whole fetch flow have not run, in the order the open questions would bite:
  - **20 indications back to back on BLE**, each waiting for its ATT confirm,
    has not been timed. Serial proves the arithmetic, not the channel: on
    serial a page is one cheap burst, on BLE it is 20 round trips inside the
    map activity's `loop()`. If a page stalls it long enough to matter,
    `kMissingPageSize` is the knob. `tools/blereplay.py` in the parent repo
    against a card with this 29-entry list settles it.
  - **The sync screen reaches the panel and waits correctly** (2026-08-06,
    build `develop-70da0b86`): it enters from the home menu, starts BLE, draws
    29 rows in 45 ms, logs `29 tiles to ask for, waiting for a phone` instead of
    the false `asked for 29 tiles` the previous build logged into an empty room,
    and BACK leaves cleanly with the heap returned. **What no phone has yet
    exercised**: the ask itself, a row's bar moving, the windowed refresh of one
    row (does its rectangle clip a descender), and whether the row count that
    fits is the one the geometry predicts.
  - **`NEED_TILES` reaching a real central.** The mechanism is the one
    `sendCommandReply()` already uses on hardware, but nothing has subscribed
    to the command characteristic from a phone yet.
  - **`forget()` on a real arrival**, and with it the whole clear-on-OK path.
    No tile has been pushed to this build.
  - **The SD write.** `flushIfDirty()` is on a 10-minute timer or the screen's
    exit, and the test above never left the map screen, so no
    `missing_tiles.json` has been read back off a card yet.
