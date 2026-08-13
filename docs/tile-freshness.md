# Tile freshness: how the device learns a tile it already has went stale

**Status 2026-08-09: built, flashed, signal verified, and all four
device-initiated triggers run against the real Android app.** `Off` is
silent, `Live` fires and fetches correctly, a live control pair proved an
already-current tile is never re-fetched -- `SyncScreen` was found not to fire
at all, a real firmware bug -- and that bug is now fixed and reverified on
hardware. See "Device-initiated triggers, verified against the real Android
app, 2026-08-09" near the end for the detail.

Laptop side lives in the parent repo: `docs/tile-index-spec.md` (the index
format), `mapbuilder/tilegen/tile_index.py`, `mapbuilder/tilegen/tools/build_index.py`.

## The problem

The firmware only ever fetches a tile it is **missing**. `MapTileSource` records
a tile as missing when the reader fails to open it or it holds no geometry
(`src/activities/map/MapTileSource.cpp:134-145`, `docs/missing-tiles.md`). A
tile that opens fine and has geometry is treated as good forever, no matter how
many times the map is rebuilt.

So the railway/tram fix -- a classification bug that drew a tram line as a
mainline railway -- was fixed, the area was rebuilt and republished, and every
device that had already synced kept the wrong tile. Permanently.

Seen again on hardware 2026-08-12, and that time the mode was **`Live`**, not
`Off`: trams drawn along a city street at zoom rungs 3-6 from a card nine days
old, while the check ran every ten minutes and answered "nothing stale" each
time. The cause was on the reply channel -- see "The reply channel dropped
lines" below.

**How to tell a stale card in one number:** ask the map console `have`. It lists
every tile of the current viewport with the `content_id` the device computed for
it, which is the same value the CDN's index publishes per slot, so one command
compares the card against what is published. The way counts in the header
(`4t 4151w`, `src/activities/map/MapActivity.cpp:2818`) are **not** usable for
this: they are raw `waysEmitted()`, the host preview reports that divided by
`MapRenderer::kRoadPasses`, and the two still did not reconcile after allowing
for it (4151 against 2086 on 2026-08-13) -- open, and no reason to wait for it
now that `have` answers the question directly.

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

## What was measured, 2026-08-09

Flashed to the X4 and driven over the serial map console. The device booted
clean, drew the map and reported 125 KB free heap.

**The signal is right, and it caught a real stale tile on the first try.** The
card held two tiles for the viewport at 48.15674/17.04819:

```
have  ->  have_13_4483_2842=23f4190c        the card
          have_13_4484_2842=c2e2b307

CDN index slot, byte-range read:  4483/2842 = 0x50f97736
                                  4484/2842 = 0xc198f90b
```

Both disagreed. That is not a defect in `contentId()` -- it is the thing this
feature exists to find, and it was proven by fixing one of them:
`tools/blepush.py` pushed the CDN's current `4483/2842` (143,798 B, fetched
with `?crc=50f97736`) onto the card, and after a `redraw`:

```
have  ->  have_13_4483_2842=50f97736        now exactly what the index says
          have_13_4484_2842=c2e2b307        untouched, still stale
```

One tile replaced, one left alone as the control. So `MapTileReader::contentId()`
on real hardware agrees bit for bit with `mapbuilder` and with the published
index, and the earlier disagreement was a genuinely out-of-date card.

The rest of the wire grammar, on the device:

| sent | device answered |
|---|---|
| `have` with a viewport whose only tile is missing | `INFO have_total=0` -- a missing tile has no content to vouch for, so it is left out rather than reported as content 0 |
| `stale 13 4484 2842` | `freshness: z13 4484/2842 is out of date`, `OK` |
| `tiles` | `tile_13_4483_2842=ok`, `tile_13_4484_2842=stale` -- the three states, on real data |
| `checked 1` | `freshness: 1 tile(s) out of date` |
| `checked unknown` | `freshness: phone could not check (no index)` -- distinct from zero, as it must be |
| `stale 13 4484` | `ERR bad_arity` |
| `checked maybe` | `ERR bad_number` |

Binary archived as `docs/firmware-builds/feat-tile-freshness-007950dd-good.bin`
in the parent repo.

## Device-initiated triggers, verified against the real Android app, 2026-08-09

Firmware `0.1.0-dev-feat/tile-freshness-007950dd`, Android app
`0.2.0-g2b1c312` (`feat/tile-freshness-index`), Galaxy S24 over real BLE.
Serial log is the evidence for every timestamp below.

**`Off` sends no `CHECK_TILES`.** 90 s window, BLE connected and subscribed the
whole time, 7 forced redraws (`redraw` on the map console). Zero `freshness:`
or `CHECK_TILES` lines. This is the positive-absence proof the silent path
needs — the redraws prove the code path actually ran each time, not that
nothing happened at all.

**`Live` fires automatically and correctly.** Entered the Map activity with
`Live` set: BLE connect at `+62 ms` from `begin()` (`638280`→`638342`),
subscribed at `+731 ms` (`639011`), `onEnter` done at `+2821 ms` (`641101`,
includes the ~1.6 s render), and `freshness: asked about 2 tile(s) on screen`
fired 61 ms after that (`641162`) — i.e. as soon as the activity had something to
ask about, not on any fixed delay. No cooldown wait needed: a freshly
constructed `MapActivity` has `freshnessNextAskMs_ == 0`, which the gate at
`MapActivity.cpp:651` treats as "not blocked," so leaving and re-entering Map
is a working substitute for waiting out the real 10-minute
`kFreshnessIntervalMs`.

**`SyncScreen` did not fire, and could not, as originally written — fixed and
reverified same day.** This was a firmware bug, not a missing feature.
`TileSyncActivity::onEnter()` checks `phoneListening()` synchronously, in the
same call that starts the BLE peripheral (`TileSyncActivity.cpp:97-120`) —
both branches (`rowCount_ == 0` and the missing-tiles branch) gated on that
same already-evaluated value. `trackPhone()`, which does correctly notice the
phone subscribing *later*, called `askForTiles()` and nothing else — it never
called `askAboutFreshness()`. So the only way `askAboutFreshness()` could fire
from this screen was if a phone was already subscribed at the exact instant
`onEnter()` ran, and that instant is microseconds after
`BlePositionServer::begin()`, which just started advertising. Measured
end-to-end connect time on real hardware: `begin()` at `489254`, `connected` at
`490407` (**+1153 ms**), `subscribed` at `491069` (**+1815 ms**). A phone
cannot connect in zero milliseconds, so `phoneListening()` was false on every
real entry, not just most of them. This is exactly the kind of bug a unit test
cannot catch: the logic was correct, the *timing* was not, and nothing about a
synchronous test exercises a 1.8-second BLE handshake. Confirmed broken by
entering `TileSync` twice in a row (leave, come straight back): second entry
logged `phone subscribed, asking` (the missing-tile ask, from `trackPhone()`)
with **no** `freshness:` line anywhere near it, on either visit.

Fix, `TileSyncActivity.cpp:97-108` and `:191-212`: both `onEnter()` branches
now defer to `trackPhone()` when no phone is listening yet instead of settling
into a final state early, and `trackPhone()` calls `askAboutFreshness()`
before `askForTiles()` on the same subscribe edge that already drove the
missing-tile ask. No new double-ask risk: `askAboutFreshness()` already
guarded itself with `freshnessAsked_` (set once per `onEnter()`, checked first
thing inside the function), so a second subscribe within the same screen visit
was always going to be a no-op — the fix only needed to give the guarded call
a second, later place to be reached from.

Reverified on hardware after the fix, same firmware rebuild, real Android app:

- Entered `TileSync` before ever opening the Map screen this boot (so
  `g_lastHeldTiles` was empty): `phone subscribed, asking` at `65554`, and in
  the same call, `freshness: no tiles drawn yet, nothing to check` — the honest
  no-op, not silence and not a crash. `asked for 17 tiles` (the missing-tile
  ask) followed immediately after, proving the ordering and the fact that a
  freshness no-op does not block the rest of the visit.
- Visited Map (populating `g_lastHeldTiles` with 2 tiles), then entered
  `TileSync` again: `phone subscribed, asking` at `178636`, `freshness: asked
  about 2 tile(s)` 9 ms later at `178645`, `asked for 17 tiles` at `178660` —
  freshness first, missing-tiles ask second, exactly the fixed order. Both
  tiles came back `stale`, both were pushed and replaced
  (`MAPXFER ... /base/13/4483/2841.tib`, `.../4484/2841.tib`), `checked 2` /
  `phone is pushing them` logged correctly.
- Double-ask guard, same visit: force-stopped and relaunched the Android app
  mid-run to force a real disconnect/reconnect (`phone gone, back to waiting`
  at `315550`, reconnected and `phone subscribed, asking` again at `318653`).
  No second `freshness:` line of any kind followed — `askAboutFreshness()`'s
  existing `freshnessAsked_` guard silently absorbed it, `asked for 17 tiles`
  fired again as expected (the missing-tile ask has no such guard and
  legitimately re-asks the phone every reconnect).

**An already-current tile never triggers a fetch, and this was proven with a
live control pair, not a synthetic one.** The `Live` run above hit a viewport
whose two tiles disagreed with the CDN by chance (`z13 4483/2842` = current,
`z13 4484/2842` = stale, the same pair as the "What was measured" session
above — the earlier fix had not stuck across the intervening reflashes). The
device asked, the phone answered `stale 13 4484 2842`, and only
`4484/2842` was pushed (`MAPXFER begin ... 396014 bytes`, control-console `grep
4483` over the whole session log turns up zero `MAPXFER` lines for it — only
the four map-viewport `reset` summaries, which always name both tiles as part
of the redraw regardless of fetch). Post-transfer `have` read back
`have_13_4483_2842=50f97736` (unchanged) and `have_13_4484_2842=c198f90b`
(now matching the CDN index, same value the "What was measured" session
recorded for that slot) — proof the current tile was never touched and the
stale one now matches exactly.

**Throughput: 7.9 kB/s for the app's real transfer** (396014 bytes in 50.1 s,
`begin` at `641780`, `done` at `691892`), against `docs/ble-map-transfer-protocol.md`'s
2.6 kB/s for `blepush.py` at the same negotiated MTU (256, 248-byte payload).
Not the same measurement: the Android stack renegotiated the connection
interval down to 12 units (15 ms) during the transfer (log: `conn params:
interval 12 units (15 ms)`), where it sat at 24 units (30 ms) idle.
`blepush.py` has no code that requests a connection parameter change at all
(`grep -n "interval\|priority" tools/blepush.py` — nothing) and BlueZ does not
expose a simple central-side connection-interval request over its D-Bus API
the way Android's `requestConnectionPriority()` does. So the two throughput
numbers measure different links, not different protocol efficiency — see
`docs/ble-map-transfer-protocol.md` for the correction.

**Phone-offline path (`checked unknown`) was not exercised this pass** — every
run here had the phone connected throughout. Console-driven `checked unknown`
handling is already covered in "What was measured" above; only the
device-initiated trigger for it (CDN unreachable while the phone answers a
live `CHECK_TILES`) remains open.

## `CHECK_TILES` never stated its own format version -- found and fixed 2026-08-10

**Found on hardware, chasing a report that a rebuilt, republished area still
showed old content on a real device with `Live` on and a phone connected.**
Direct proof it was not a rebuild problem: `have` over the console read back
the card's real content ids for its four held tiles
(`67a13267`, `a5d8d099`, `857b4056`, `3d04647a`), and every one of them
disagreed with what the freshly rebuilt CDN mirror actually contains for
those same tiles (`913ef02b`, `6ee97058`, `8b2b73a0`, `47d3cfec` --
`mapbuilder/tile_reader.read_tile_identity()` against `mapbuilder/cdn/base/12/...`).
Genuinely stale card, and the phone had just answered `checked 0` for it
minutes earlier.

Cause: `CHECK_TILES <count>` (`MapActivity.cpp`, `maybeCheckTileFreshness()`)
carried only a count, never a format version, unlike `NEED_TILES <count> fmt
<version>` (`TileSyncActivity.cpp:182`, `MapActivity.cpp:519`). On the
Android side, `FreshnessChecker.formatVersion` (`FreshnessChecker.kt:114`,
before the fix) was written only from a parsed `NEED_TILES` line
(`FreshnessChecker.kt:127`) and read by every index range read
(`FreshnessChecker.kt:227`). A device with nothing missing never sends
`NEED_TILES` at all, so `formatVersion` stayed `null` for the whole
connection and every index read fell back to
`CdnTileSource.DEFAULT_FORMAT_VERSION = 2` (`TileSource.kt:85`) -- one
version behind the device and the CDN, both on 3
(`mapbuilder/tilegen/tiles.py:53`'s `FORMAT_VERSION`, confirmed live by the device's
own `info` reply, `tile_fmt=3`). The phone was comparing the card against the
CDN's abandoned `/v2/` index tree, where an old, pre-fix content id still
happened to sit in the same slot -- a false "not stale" on every single
check, for exactly the devices with nothing missing that this feature most
needs to reach.

Fixed by giving `CHECK_TILES` the same `fmt <version>` word `NEED_TILES`
already carries (`MapActivity.cpp`, `MapTileReader::kFormatVersion`), and
teaching the phone to read it directly (`MissingList.CheckTiles`,
`MissingList.kt`; `FreshnessChecker.kt`'s `onCommandLine` sets
`formatVersion` from it before calling `start()`), no longer solely
dependent on a `NEED_TILES` having arrived first. `NEED_TILES` still updates
the same field when it does arrive, so a still-missing tile does not regress.

**Verified: unit-tested, not yet re-measured live end to end.** Both apps
build clean. New coverage: `MissingListTest`'s two `check tiles carries...`
tests and `FreshnessCheckerTest`'s `CHECK_TILES states its own format
version, no NEED_TILES required` -- the latter fails without the fix (asserts
the index read happens against format 3 from a `CHECK_TILES 1 fmt 3` alone,
no preceding `NEED_TILES`) and passes with it. Flashed to real hardware and
confirmed the ask still fires (`freshness: asked about N tile(s) on screen`).
**Open:** a live BLE capture of the actual `CHECK_TILES ... fmt 3` bytes
in flight was attempted this pass and blocked on a flaky `bleak` connect in
this environment, not retried — the wire format is unchanged from
`NEED_TILES`'s already-proven `fmt <version>` grammar, so this is a real gap
in *this specific verification*, not in confidence about the fix's shape.

## The reply channel dropped lines -- found and fixed 2026-08-13

The whole check ran correctly and still answered about a fraction of the
screen. Measured on hardware, one map viewport holding four z11 tiles:

```
X4:      freshness: asked about 4 tile(s) on screen
X4:      rx: have
phone:   device wants 4 tile(s) checked, format 3
phone:   checking 1 tile(s) in 1 range read(s)      <-- one, not four
phone:   check finished: 0 stale of 1
X4:      rx: checked 0  ->  "0 tile(s) out of date"
```

Two of those four tiles were out of date on the card -- a build from 2026-08-04,
still drawing trams as mainline railway, still carrying the `track` class the
overview LOD dropped on 2026-08-08. The device had been asking every ten
minutes for days and being told everything was current.

**The `have` reply was fine; the link lost it.** The same command over USB
serial returns all five lines every time, and so does a laptop BLE client
(`tools/mapcmd.py --ble have`, 3 of 3 runs, all five lines). Only the phone saw
one.

**Cause: one indication per line, and a confirm wait that gave up too early.**
`sendCommandReply()` waited for `CommandCharCallbacks::onStatus` before
returning, which is the right idea -- NimBLE's `indicate()` only means the line
reached a one-slot queue, and the next call overwrites whatever is still in it.
But the wait was 500 ms and **returned `true` on timeout**, so a slow peer put
the clobbering straight back. Confirms from this Android build measure
**688-1503 ms** (`BLEPOS: reply confirm took N ms`, added in the same pass),
i.e. always over the old budget. Six lines took 3.9 s and four of them died in
the queue.

Fixed in two places, both needed:

- **Batching.** `MapBleConsole` packs whole lines into one indication up to the
  ATT payload (253 bytes at MTU 256) and flushes at the end of `poll()`;
  `BlePositionServer::sendCommandBlock()` sends the block. A four-tile `have`
  is 83 bytes -- one indication, one confirm. Compatible with every existing
  reader by construction: both the app (`BleLink.handleIndication`) and
  `tools/mapcmd.py` already reassemble by newline rather than assuming one
  indication is one line.
- **A missing confirm is a failure.** The wait is now 3 s and returns `false`,
  logging `reply unconfirmed after N ms`. Not retried: the peer may still take
  the first copy, and a duplicate line in a listing whose point is a count that
  adds up is worse than a short one.

The phone stopped trusting a short listing in the same pass: `HaveReader`
compares the tile lines it got against `have_total`, and `FreshnessChecker`
answers `checked unknown` instead of a verdict when they disagree
(`FreshnessCheckerTest`, "a have listing that lost lines is answered unknown,
never checked"). Belt and braces on purpose -- the batching removes today's
loss, the count check makes tomorrow's loud.

**Verified end to end on hardware 2026-08-13**, same viewport that produced the
failure above:

```
freshness: asked about 2 tile(s) on screen
BLEPOS: reply confirm took 1487 ms for 83 bytes    <-- the whole listing, one indication
rx: stale 11 1120 710   ->  z11 1120/710 is out of date
rx: stale 11 1121 710   ->  z11 1121/710 is out of date
rx: checked 2           ->  2 tile(s) out of date
MAPXFER begin /trailink/base/11/1120/710.tib, 48864 bytes, crc 6c938098
MAPXFER done  /trailink/base/11/1120/710.tib, 48864 bytes, crc 6c938098
freshness: z11 1120/710 replaced
```

The card's `content_id` for that tile went `ec483e47` -> `c0f79ee0`, which is
what the CDN's index publishes for it. First time the check has replaced
anything.

**Open: the push is slow.** 48864 bytes took 119 s, i.e. ~410 B/s, against the
2.6 kB/s `docs/optimization/03-ble-link.md` measured for a fetch. Both tiles
still land, and the app's own reply timeout (`TileFetcher.REPLY_TIMEOUT_MS`,
15 s) is per frame rather than per file, so this costs minutes rather than
correctness. Not chased in this pass -- needs a per-chunk timestamp on one
transfer to say whether the wait is the SD write, the connection interval or
the app's queue.

## The status channel loses that slot -- fixed 2026-08-13

**Read off the code, not measured.** A consequence of the fix above, found in
the BLE review (`../../docs/ble-review-2026-08.md`, "Stability -- firmware"
item 1) and fixed in the same week.

**There is one indication slot per connection, not one per characteristic.**
The command channel (`...0003`) and the transfer status channel (`...0005`)
both indicate on the same link, so only one of them can have an indication
unconfirmed at a time. NimBLE's `indicate()` returns false for the other one.

The two now run at the same time by design. The map screen sends a `have`
listing and pushes stale tiles in the same session (this doc, "How the check
works"), and one listing can hold the slot for the whole confirm budget --
`kConfirmTimeoutMs = 3000` (`lib/BlePositionServer/src/BlePositionServer.cpp:509`),
against measured confirms of 688-1503 ms.

`sendTransferStatus()` retried 8x25 ms = 200 ms and then dropped the line. So
the `RDY` that opens a transfer, or the `OK`/`ERR` that closes it, was lost
whenever a listing was in flight -- and the phone, which waits for those lines,
timed the tile out and asked for it again later.

**A longer retry cannot fix it.** `sendTransferStatus()` runs inside the
transfer write callback, i.e. on the NimBLE host task
(`src/activities/map/MapTransferReceiver.cpp:297`, `:432`), and that is the same
task that processes the peer's confirm and frees the slot. Waiting there blocks
the event being waited for: every attempt fails by construction. Exactly the
shape of the advertising-restart bug (`ble-advertising.md`, "A failed restart is
the activity task's problem").

So it is a queue, not a bigger loop:

- **One attempt, then park.** `sendTransferStatus()` calls `indicate()` once. On
  busy it copies the line into a 2 x 64 byte pending buffer under the existing
  `g_mux` critical section and returns false
  (`lib/BlePositionServer/src/BlePositionServer.cpp:671-704`). No wait, no
  `vTaskDelay`, nothing on the host task. Two slots because the realistic worst
  case is two lines: one `RDY`, one verdict. Fixed size, no heap -- an
  allocation on the host task is the last thing wanted.
- **The activity task drains it.** `flushTransferStatus()`
  (`:707`) copies the front line out under the lock, `indicate()`s it outside
  the lock, and advances the tail only on success. One attempt per call; both
  screens call it once per tick (`src/activities/map/MapActivity.cpp:1698`,
  `src/activities/map/TileSyncActivity.cpp:325`), next to
  `serviceAdvertising()` and for the same task-ownership reason.
- **Overflow drops the oldest and logs.** A third line means the first is two
  verdicts stale (`:683`). Never blocks, never refuses the new line.
- **A parked line dies with its connection.** `onCentralDisconnect()`,
  `begin()` and `end()` clear the buffer -- the peer that would read it is gone,
  and a verdict about a transfer the *next* connection never made is worse than
  silence.

`false` from `sendTransferStatus()` therefore means "not on the link yet", not
"lost". No caller retries on it (`MapTransferReceiver.cpp:297`, `:432`, `:469`,
`:477` all ignore the return), which is why the meaning could change without
touching them.

**Open -- needs hardware.** The scenario to watch: start a freshness listing,
push a tile while it is unconfirmed, and expect
`BLEPOS: indication slot busy, parked transfer status: RDY ...` followed within
a tick or two by `parked transfer status sent: RDY ...`, with the phone
proceeding to chunks. Nothing has been on the glass yet.
