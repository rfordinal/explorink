# Tile freshness: how the device learns a tile it already has went stale

**Status 2026-08-09: built end to end on this branch, never run on hardware.**
The signal, the setting, the BLE exchange, the stale list and both triggers are
in and covered by host tests; nothing has been flashed. What is described below
as "how the check works" is read off the code, not measured.

Laptop side lives in the parent repo: `docs/tile-index-spec.md` (the index
format), `mapbuilder/tile_index.py`, `mapbuilder/tools/build_index.py`.

## The problem

The firmware only ever fetches a tile it is **missing**. `MapTileSource` records
a tile as missing when the reader fails to open it or it holds no geometry
(`src/activities/map/MapTileSource.cpp:134-145`, `docs/missing-tiles.md`). A
tile that opens fine and has geometry is treated as good forever, no matter how
many times the map is rebuilt.

So the railway/tram fix -- a classification bug that drew a tram line as a
mainline railway -- was fixed, the area was rebuilt and republished, and every
device that had already synced kept the wrong tile. Permanently.

## The signal: `contentId()`

```
content_id = crc32( 6 x u32 LE per-layer crc32, in layer id order 1..6 )
```

`MapTileReader::contentId()`, `src/activities/map/MapTileReader.cpp`. Verified
bit-for-bit against mapbuilder's `content_id_from_layer_crcs()` over real
fixture tiles (`test/map_tile_reader/MapTileReaderGoldenTest.cpp`,
`ContentIdMatchesMapbuilder`).

**It costs nothing.** `parseHeader()` already reads the whole layer directory
and keeps every layer's crc32 in `layers_[i].crc32`
(`src/activities/map/MapTileReader.h`). `contentId()` is arithmetic over values
that are already in RAM -- no seek, no read, no extra flash.

**It needs no format change.** That matters more than it sounds: a tile whose
`format_version` does not match is refused at open
(`src/activities/map/MapTileReader.cpp`, the version check in `parseHeader()`),
which hatches it and records it missing. So bumping the format to carry a new
field would make **every tile on every card** look missing at once and force a
full re-download over BLE at ~7.4 kB/s. The per-layer crc32s are already there.

## Why not the timestamps in the header

Both were tried on the laptop side first and both were wrong. Measured, not
argued:

| signal | what it does | why it fails |
|---|---|---|
| `osmEpoch()` | time of the OSM extract | **Does not move when only the build rules change.** A rules rebuild runs from the same cached extract. Measured over Karlova Ves: a genuine rule change left `osm_epoch` identical on all 10 tiles while the geometry changed on all 10. This is the exact case the feature exists for. |
| `buildEpoch()` | time of the build | Moves on **every** build, including one that produces identical geometry. Comparing it re-downloads a continent to change nothing. |

The per-layer crc32s cover geometry and nothing else -- no epoch, no coordinates
-- so they move if and only if what the tile draws moved, whichever cause did
it. One signal, both reasons.

`buildEpoch()` is still worth keeping for display ("this tile last changed in
March"). It is not the comparison.

### Consequence: no "never downgrade" rule

The laptop-side spec used to say a device holding a newer tile must never be
downgraded. That existed only because time was the signal. With content
identity the CDN is simply the source of truth: content differs, take the CDN's
copy. A deliberate rollback is then followed correctly instead of ignored.

## The setting: `mapTileFreshnessMode`

`CrossPointSettings::MAP_TILE_FRESHNESS_MODE`, category Map, **default `Off`**.
Same rule as `mapAutoSyncTiles`: it spends the phone's mobile data, so the rider
chooses it deliberately or it does not happen.

| mode | when the device asks |
|---|---|
| `Off` | never. No `CHECK_TILES` is ever sent, and the code path costs one compare per `loop()`. |
| `SyncScreen` | once when the tile sync screen opens (`TileSyncActivity::askAboutFreshness()`). Preparation at home, which is where spending data belongs. |
| `Live` | that, plus from the map screen on a cooldown (`MapActivity::maybeCheckTileFreshness()`, `kFreshnessIntervalMs`, 10 minutes). |

Ten minutes rather than the autosync's one: the answer changes when somebody
rebuilds an area -- hours or weeks apart -- not when the rider moves.

## The sync screen has no viewport, so the map leaves it one

`content_id` is free only where a tile is already open, and only the map screen
opens tiles. The sync screen is where a rider deliberately spends data, and it
draws no map at all.

So `MapActivity` writes its last viewport snapshot to `g_lastHeldTiles`
(`src/activities/map/LastHeldTiles.h`) after every reset, and `TileSyncActivity`
answers `have` from it. In memory, one snapshot, never persisted -- it changes
every frame, and writing it would be an SD write per frame for a value whose
whole worth is being current.

Empty until the map has drawn once, which reads correctly: there is nothing to
check. The sync screen says so and sends no `CHECK_TILES`.

## How the check works

The device does **not** read the index. That was decided rather than defaulted:
an on-device reader means seek/offset arithmetic in firmware and a
world-sized index mirrored onto one SD card, for a check the phone can already
do. Same division of labour the missing-tile fetch already uses -- the device
asks, the phone fetches from the CDN.

1. Device sends `CHECK_TILES <count>`. The phone reads the list with `have`, and
   the device answers `(z, col, row, content_id)` per tile from the snapshot the
   last render left behind. The content ids were collected where
   `MapTileSource` already opens each tile (`contentIdAt()`) -- no extra I/O,
   the same trick `MissingTilesStore` plays for absent tiles.
2. Phone byte-range reads the matching slots out of the CDN's `.idx` and answers
   `stale <z> <col> <row>` per differing tile, then `checked <n>`. One range
   request per zoom plane, not one per tile: the slots are contiguous in the
   row-major plane.
3. **The phone then pushes those tiles unasked.** It found them, so it holds
   their expected content ids; relaying the list back through the device would
   only lose that. It requests each with `?crc=<content_id>`, which makes every
   version its own CDN cache key -- the tile path does not change when a tile is
   rebuilt, and the edge caches for 7 days with no purge.

The device's own record of them (`StaleTilesList`) is bookkeeping, not a fetch
queue: it flags the tile in a `tiles` reply and it arms the loop guard below.

`checked unknown` means the phone could not read the index. **Not zero.** It is
logged and nothing is marked -- see the wire contract in
`../../docs/ble-map-transfer-protocol.md`.

### Two things the implementation must not get wrong -- and how it does not

Both are handled in `src/activities/map/StaleTilesList.h`, covered by
`test/map_command_parser/MapCommandParserTest.cpp`.

**Stale tiles do not belong in `MissingTilesStore`.** Its records are persisted
(`z/col/row/count`, `src/MissingTilesStore.cpp`) even though the in-memory
`refused` flag is not. A stale entry written there would survive a reboot and
the device would then ask the phone for a tile it already has. Its eviction is
also by `count`, so fresh stale entries are dropped first -- and in a live
check, a stream of them would push out genuinely missing tiles. Keep a separate
in-memory list.

**A stale fetch that does not fix the tile must stop asking.** If the arriving
tile still does not match the index, mark it and do not re-check it. Without
that, a cache serving an old copy turns a live check into an endless fetch loop
with real battery behind it.

The device cannot compare against the index itself, so the guard is built from
what it does see: `StaleTilesList::onArrived()` remembers that a tile was
fetched *because* it was stale, and a second `stale` report for the same tile
becomes `giveUp()` instead of another entry. A `skip` for a stale tile does the
same. Both survive `clear()`, so the next check does not rediscover the tile and
start over.

## Not verified

Everything above is read off the code. Nothing has been on the glass. The
hardware run that would settle it, once the flash is agreed:

- Put a deliberately older tile on the card against what the CDN publishes.
- `Off` produces no `CHECK_TILES` at all -- confirm on the serial log.
- `SyncScreen` catches it when that screen opens; `Live` catches it mid-ride
  with the sync screen never opened.
- **An already-current tile never triggers a fetch in any mode.** This is the
  one that matters: a false positive costs a rider their viewport over a
  ~7.4 kB/s link.
- The phone offline answers `checked unknown` and nothing is fetched.
