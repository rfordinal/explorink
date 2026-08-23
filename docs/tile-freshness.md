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
| `SyncScreen` | once per visit to the tile sync screen (`TileSyncActivity::askAboutFreshness()`), **after that visit's fetch has settled** -- see "Two asks on one channel". Preparation at home, which is where spending data belongs. |
| `Live` | that, plus from the map screen on a cooldown (`MapActivity::maybeCheckTileFreshness()`, `kFreshnessIntervalMs`, 10 minutes), which is what drains the held-tile store in the background. |

Ten minutes rather than the autosync's one: the answer changes when somebody
rebuilds an area -- hours or weeks apart -- not when the rider moves.

## The list accumulates as you ride, and drains as it is answered

`content_id` is free only where a tile is already open, and only the map screen
opens tiles. The sync screen is where a rider deliberately spends data, and it
draws no map at all. So the map fills a store and the sync screen spends data
emptying it: `HeldTilesStore`, `src/activities/map/HeldTilesStore.h`, one global
instance both screens wire into their console state.

**It used to be a single-viewport snapshot, and that was the wrong shape**
(`LastHeldTiles.h`, deleted 2026-08-13). That version held one render's worth of
tiles -- 16 at the 4x4 worst case -- and `MapActivity::renderViewport()` rebuilt
it from scratch on every viewport reset. So a rider could pan across a city and
still only ever have the last screenful checked, while `MissingTilesStore` was
accumulating the tiles that were *absent* the whole time. Riding is how a device
accumulates tiles worth checking, so the list has to accumulate the same way.

Four rules, all in `HeldTilesStore.cpp` and covered by `HeldTilesStore.*` tests
in `test/map_command_parser/MapCommandParserTest.cpp`:

- **`record()` on every render, per tile.** Same coordinates and the **same**
  `content_id` changes nothing -- every reset walks tiles the last one already
  had, and re-arming there would mean the list never drains. A **different**
  `content_id` re-arms the entry: the card holds something the phone has not
  seen. `content_id == 0` is not recorded at all; a tile that did not open has
  no content to vouch for and is already on the missing path.
- **`have` lists only what is pending, and stamps what it listed.** A tile
  recorded between the listing and the phone's answer is pending but not asked,
  so `checked` cannot settle a tile the phone was never told about.
- **`checked <n>` settles the whole listed batch**, stale or not. A stale tile is
  settled too: the phone is already pushing the replacement, and the new copy
  re-arms this entry through `record()` when it is next drawn. That closes the
  loop and cannot spin, because the second answer agrees.
- **`checked unknown` settles nothing.** The phone could not read the index and
  is claiming nothing.

**This is why `Live` mode looks like it works a backlog down** -- it does. The
map screen's ten-minute check empties the store in the background, and the sync
screen's check is the same queue emptied on demand rather than a second,
parallel mechanism. A device with nothing pending stops asking entirely
(`MapActivity::maybeCheckTileFreshness()`, the `pendingCount() == 0` gate)
instead of re-sending the same screenful every ten minutes.

### The store holds 64, one listing sends 12

Two different caps, and conflating them is a freeze.

`MapBleConsole` bounds itself at `kMaxBlocksPerPoll` (2) indications per
`poll()`, so a hung-but-subscribed peer cannot chain 3-second confirm waits on
the activity task. **That cap is checked before reading the next command byte,
not while one command is emitting replies** (`MapBleConsole::poll()`). So a
single `have` runs to however many blocks its listing needs, back to back, and
the cap does not see it. A 64-entry listing is ~10 blocks: about 11 s of dead
buttons on a healthy link and up to 30 s against a peer that has stopped
confirming -- exactly the freeze the block cap exists to prevent, through a
different door.

So `HeldTilesStore::kMaxPerListing = 12`: a ~32-byte line packs seven to a
253-byte indication, so twelve entries plus the total line stay inside two
blocks. Nothing is lost, because the store drains -- the sync screen asks again
while anything is still pending, so a visit still empties the store, in rounds.
It cannot spin: `markAskedChecked()` settles the round before the next ask, so
`pendingCount()` strictly shrinks. It re-asks only on a `known` answer; `checked
unknown` settles nothing, so re-asking on that would loop forever against a
phone that cannot read the index. The map screen does not re-ask -- its next
cooldown is the next round, which is what a background drain should look like.

**The round cap alone does not bound the freeze, and the first version of this
got that wrong.** Capping the listing bounds one `have`. It does nothing about
*where the next round is started from*, and the first version started it
straight out of `onCheckFinished()` -- inside `MapConsoleState::handle()`'s
dispatch of `checked`, on the activity task, before the terminating `OK` the
phone was still waiting for. That put an e-ink repaint (500-1700 ms) and a
`sendCommandReply()` confirm wait (up to 3 s) in front of that reply: ~6.4 s per
round, six rounds for a full store, and a plausible deadlock against a peer that
will not confirm a new indication while its own command is open.

Suspected cause of a solid hang on real hardware 2026-08-13 -- buttons dead,
`CMD:SCREENSHOT` returning zero bytes, the device recovering on a plain reset.
**Read off the code, not proven:** no serial log was captured from the hang, and
it is not known whether that visit had more than one round's worth pending, so
whether the re-ask fired at all is open. A monitor attached from boot through a
reproduction would settle it -- the question is whether a second `freshness:
asked about ...` line precedes the silence.

The ask now goes through `freshnessAskPending_`, consumed in `loop()`, which is
the pattern the console already had. See below.

### Seeding a grid to look at

The grid's two marks are a layout decision, and a layout decision on this device
can only be settled on the panel. Until 2026-08-13 there was no way to put a
*populated* one there deliberately: the squares need a card with holes in the
right places and the dots need a ride that happened to touch the right tiles. So
the grid shipped without anyone having looked at a full one, which is how the
frames-around-dots problem survived to the second flash.

Three pieces close that, all on the device so what comes back is device output
rather than a laptop render of what the layout ought to be:

- **`fake <missing> <held>`**, a map-console command (`MapCommandType::Fake`,
  `IMapFakeSink`). Seeds synthetic entries into `MISSING_TILES` and
  `g_heldTiles` around the persisted last fix -- the same origin the sync
  screen's fetch order uses, so they land where its grid window will look. On
  the map screen only: that is the half with the projection and the missing-tile
  store. Elsewhere it answers `INFO fake=unavailable`, which is not the same as
  seeding zero.
  - **Deterministic, not random.** Two runs with the same counts must give the
    same picture, or comparing one dot size against another compares two
    layouts as well.
  - **Spread over z11/z12/z13 in rotation.** Dot size derives from the tile's
    LOD, so a grid of one LOD says nothing about whether the three are still
    distinguishable.
  - The reply states what actually landed, not what was asked for -- the stores
    have their own caps.
  - **It clears both stores before seeding**, and that clear is what makes the
    determinism above true -- it was false until 2026-08-13. `fake` only added
    before, so runs piled up, and because `MissingTilesStore` persists to the
    card a stale seeding bug was still on screen two flashes later. The cost is
    the device's real missing-tile list; it rebuilds the next time the map
    hatches anything.

**It seeds a ride, not a block.** A rider collects a corridor along a road, and
`chooseWindow()` is placed and shrunk around whatever shape that is -- a
rectangle never exercised it, and the picture said nothing about what the screen
looks like after an actual trip. Sixteen hand-laid z11 waypoints that bend,
inside the 6x8 window cap, with the near stretch held and the far stretch
missing on a fixed split.

Two bugs in that placement were visible on the panel and invisible in the code,
which is the whole argument for this tool existing:

- Offsets were first computed in **each LOD's own tile units**, so `+1` at z11
  was a whole parent of ground and `+1` at z13 a sixteenth of one. The three
  LODs spread over wildly different areas and the grid came back scattered.
- The missing stretch's start was **derived from the held count**, which wrapped
  modulo straight back onto waypoint 0 as soon as the held count covered the
  road -- so `--held 48` put every frame on top of the first dots. It is a fixed
  split now, and the two stretches cannot reach each other at any counts.
- **`CMD:GOTO_TILESYNC`** (`main.cpp`), mirroring `CMD:GOTO_MAP`. The sync
  screen was the one screen a host could not reach, so every look at it cost a
  person standing at the device.
- **`tools/tile_grid_shot.py`** in the parent repo, which chains the lot: set
  the mode, go to the map, seed, go to the sync screen, grab the framebuffer.
  One BMP per run.

**Stale dots are not seedable this way.** A `stale` report has to reach the sync
screen, and that screen runs a BLE console only -- `TileSyncActivity` holds a
`MapBleConsole` and no serial one. Use `tools/mapcmd.py --ble stale <z> <col>
<row>` with the screen already up. The dots `fake` seeds are the other half of
the same set anyway (queued, not yet answered for), and both marks draw
identically, because both mean "not settled yet".

A debug command in shipped firmware is deliberate here rather than sloppy: the
map console **is** this fork's debug surface (`have`, `tiles`, `stats`,
`CMD:SHOWIMAGE`), so this follows the existing pattern instead of adding a new
one.

### Never work inside a console dispatch

`MapConsoleState::handle()` runs on the activity task, one command at a time,
and the phone is waiting for that command's terminating `OK`. Anything slow
started from an observer it calls -- an e-ink repaint, a `sendCommandReply()`
confirm wait, a fetch -- delays that `OK`, and can deadlock against a peer that
will not confirm a new indication while its own command is open.

The console already provides the way out, and it is worth copying rather than
inventing: `handle()` returns a redraw flag, `poll()` aggregates it, and the
activity acts once `poll()` has returned (`MapBleConsole::poll`). **Set a member
flag in the observer; do the work in `loop()`.** `TileSyncActivity` does this
with `freshnessAskPending_` and `freshnessRedrawPending_`.

A second thing fell out of the same fix: `askAboutFreshness()` used to repaint
before returning, and every one of its callers already repaints immediately
after it -- two e-ink refreshes per ask. It now sets the redraw flag like
everything else, so a round costs one.

`CHECK_TILES <count>` states the round, not the backlog, so the number matches
what `have` is about to list.

**Found by rebasing onto `7ca2ea36`, not on hardware.** The interaction is read
off the two code paths; the 11 s and 30 s figures are that arithmetic against
the measured confirm range below, not timings anybody took.

### Why the store holds 64

`HeldTilesStore::kMaxEntries = 64`, a fixed array -- 1 KB of static DRAM, no
heap, no fragmentation risk. The RAM is not the constraint; the reply channel
is. A `have` line is ~32 bytes, seven fit in one 253-byte indication at MTU 256,
and every indication waits for the peer's ATT confirm before the next goes out.
Confirms measure **688-1503 ms** on the current Android build (see "The reply
channel dropped lines" below), so 64 entries cost about ten confirms.

**Computed from those measured confirms, not measured at that scale:**

| pending tiles | indications | rough wait |
|---|---|---|
| 64 | 10 | ~11 s |
| 100 | 15 | ~16 s |
| 256 | 37 | ~41 s |

A bigger store would not finish sooner -- it would take more rounds to empty --
so the cap is where a rider still waits through one check. **Open:** none of
these has been timed end to end on hardware; a single run with a full store and
timestamps on the first and last confirm would settle the whole column.

Bounded by eviction rather than refusal: a settled entry goes first, because the
map re-records it the next time it draws that ground. With nothing settled the
oldest goes, because the tiles worth checking are the ones near where the rider
now is.

**In memory, never persisted**, unlike `MissingTilesStore` -- and the difference
is real rather than an omission. That store exists so somebody *else* can fill
the gaps; a laptop tool reads the file off the card (`missing-tiles.md`).
Nothing off-device reads this list, and a `content_id` is only trustworthy while
it matches the bytes on the card, which a reboot cannot promise. A reboot costs
the accumulation and the map rebuilds it by drawing.

Empty before the map has drawn once, which reads correctly: there is nothing to
check. `have` answers `have=none` and the sync screen sends no `CHECK_TILES`.

## The screen says which one it is doing

**Added 2026-08-13, after a rider could not tell a check from a download.** The
check ran on the sync screen, moved real tiles over BLE, and the only evidence
it had happened at all was the serial log: `onTileStale()` and
`onCheckFinished()` logged and returned without a repaint, and a replaced tile
does not touch the grid either -- `drainTransferredTiles()` calls
`MISSING_TILES.forget()`, which returns false for a tile that was never missing.
So the panel showed the fetch's own verdict throughout and the data spend read
as a download of missing tiles.

`TileSyncActivity::formatFreshness()` now draws one line under the fetch's
numbers, in five states: idle (nothing drawn), asking, all current, N of M out
of date, and could-not-check. The last one is deliberately not phrased as good
news -- `checked unknown` means the phone could not read the index, and the two
must never read alike.

### The check queue is dots, the fetch queue is outlines

One grid, two marks, and each says what the square is waiting on:

| mark | meaning | goes out when |
|---|---|---|
| outlined square, inside a framed parent | waiting on **bytes** -- missing, or stale and being replaced | the tile arrives |
| solid dot, no frame | on the card, waiting on an **answer** | the phone says it is current (it goes) or stale (it becomes a frame) |

**The mark says what the device is waiting on, not why.** A stale tile stops
waiting on an answer and starts waiting on bytes, which is exactly what a
missing tile is doing, so it changes mark rather than keeping its own. The first
version kept it a dot until the replacement landed, and that hid the one
transition the grid exists to show: dots turning into frames as the check finds
things, frames then vanishing as they download.

**The frames belong to what is being downloaded.** The z11 parent frame and the
z12 quadrant frame are scaffolding for a hatched square -- they say how deep it
sits. A dot already carries its depth in its size, so a parent holding nothing
but dots is drawn bare: framing it spends ink on nothing and buries the one
thing worth watching, which is the dots going out one at a time. A parent
holding both still gets its frame, from the missing-tile pass, because
something in it really is missing.

**A dot is drawn for every tile queued for a check, not only for the ones that
turn out to be stale.** That is the point of it: the grid starts full of dots
and empties as the check works through them, so a rider can see the queue
draining rather than reading a number change. A tile that comes back stale keeps
its dot -- it is still not settled -- until the replacement actually lands.

The dot set is therefore every unsettled entry in `HeldTilesStore` plus every
entry in `StaleTilesList`. The three groups cannot overlap: a missing tile has
no `content_id` so it never enters the held store, and a stale tile was settled
in the store by the `checked` that reported it, so it is no longer pending
(`TileSyncActivity::interestAt()`).

The dot scales with the tile's LOD so depth still reads -- `kDotDivisor` of the
cell, floored at `kMinDotPx` and capped at `kMaxDotPx` -- but it stays well
under the cell on purpose. A disc that fills its square stops reading as a mark
on a map and starts reading as a filled tile, which is the thing the outline
decision already rejected for being all ink and no information.

**Measured on the panel 2026-08-13**, at a ~85 px cell: a divisor of 4 gave
20/10/5 px for z11/z12/z13, and the 5 px ones read as dirt rather than marks,
especially where they clustered next to a frame. Compressed to roughly 16/12/8
(`kDotDivisor` 5, `kMinDotPx` 8, `kMaxDotPx` 16).

**Open, and worth stating rather than hiding:** at 16/12/8 the 12 and the 8 are
hard to tell apart unless they sit side by side, so the LOD is only reliably
readable at the extremes. Whether that matters depends on whether a rider needs
the LOD from this screen at all -- nobody has asked one, and the position
already carries most of the depth information.

The **"N / M" denominator is `transferTotal()`, not `rowCount_`**. A stale
tile's replacement lands in `transfer.completed` exactly like a missing tile's
arrival, but only the missing ones were ever in `rowCount_`, so a visit that
replaced 24 stale tiles against 12 missing printed `24 / 12` -- a ratio above
one, seen on the panel 2026-08-13. An earlier fix covered only the
`rowCount_ == 0` case (which printed `1 / 0`) and left this one.

**Unverified:** the hardware run that proved the check works found zero stale
tiles, so neither `transferTotal()`'s stale term nor `onTileStale()`'s repaint
has actually executed on a device. A sync against a genuinely republished area
would settle both; `tools/mapcmd.py --ble stale <z> <col> <row>` with the sync
screen up forces it without waiting for one.

No new renderer primitive: `fillRoundedRect()` with a corner radius of half the
side is a disc, and it already clamps the radius to half the smaller side
(`lib/GfxRenderer/GfxRenderer.cpp`).

Two things had to change around it:

- **The grid window is sized on every unsettled tile** (`interestCount()`,
  `interestAt()`). It used to be sized on the missing list alone in `armRun()`,
  so a visit with nothing missing had no window at all -- which is exactly the
  common freshness case. `chooseWindow()` therefore runs again from
  `onCheckFinished()`, the first moment every `stale` line has landed.
- **`staleTiles_.onArrived()` is now called on the sync screen.** It never was:
  `drainTransferredTiles()` only called `MISSING_TILES.forget()`, which returns
  false for a tile that was never missing. So a replaced tile kept its entry for
  the rest of the visit -- now visible as a dot that would never go out -- and
  the ping-pong guard was never armed on this screen, meaning a cache serving
  the old copy could be fetched repeatedly. The map screen had always done this
  (`MapActivity::drainTransferredTiles()`); the sync screen now matches.

**Not yet on hardware.** Builds clean and the geometry is read off the code; how
a 3-20 px dot resolves on the panel next to a 2 px outline is a tone question
and this project does not settle those from a laptop render
(`docs/eink-grayscale.md`, and the `CMD:SHOWIMAGE` path exists for exactly
this).

## How the check works

The device does **not** read the index. That was decided rather than defaulted:
an on-device reader means seek/offset arithmetic in firmware and a
world-sized index mirrored onto one SD card, for a check the phone can already
do. Same division of labour the missing-tile fetch already uses -- the device
asks, the phone fetches from the CDN.

1. Device sends `CHECK_TILES <count> fmt <version>`. The phone reads the list
   with `have`, and the device answers `(z, col, row, content_id)` for every
   tile in `HeldTilesStore` that no check has settled yet. The content ids were
   collected where `MapTileSource` already opens each tile (`contentIdAt()`) --
   no extra I/O, the same trick `MissingTilesStore` plays for absent tiles.
   The count is advisory: the phone trusts `have_total` and the lines it
   actually receives, so a render landing between the two cannot break it.
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

Reverified on hardware after the fix, same firmware rebuild, real Android app.

**Read the ask ordering below as history, not as current behaviour.** These runs
sent the freshness ask first and the missing-tile ask second, 9-15 ms apart.
That was reversed on 2026-08-11 -- two conversations on one command channel end
each other's listings -- so today the sync screen sends `NEED_TILES` alone and
holds `CHECK_TILES` until the fetch settles. See "Two asks on one channel" below.
Everything else in these runs still stands.

- Entered `TileSync` before ever opening the Map screen this boot (so the
  held-tile list was empty): `phone subscribed, asking` at `65554`, and in
  the same call, `freshness: no tiles drawn yet, nothing to check` — the honest
  no-op, not silence and not a crash. `asked for 17 tiles` (the missing-tile
  ask) followed immediately after, proving the ordering and the fact that a
  freshness no-op does not block the rest of the visit.
- Visited Map (populating the held-tile list with 2 tiles), then entered
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

## Two asks on one channel -- found and fixed 2026-08-11

**The sync screen sends `NEED_TILES` first and alone, and holds `CHECK_TILES`
until the fetch settles.** Not the other way round, and not both at once. This
reverses what the 2026-08-09 runs above describe.

Measured on hardware. The screen fired both 15 ms apart, the phone answered each
with a command on the same channel, and a reply listing on that channel ends
with a plain `OK` -- so either conversation could be closed by the other's
terminator:

```
[786768] [MAPBLE] rx: have      <- freshness conversation opens
[786888] [MAPBLE] rx: missing   <- fetch conversation opens, `have` still open
```

The phone read the 20-tile missing list as empty, pushed nothing, sent no skips,
and all 20 rows sat at "waiting" indefinitely with both sides reporting success.

So `TileSyncActivity::askForTiles()` goes out alone, and `askAboutFreshness()`
is called from whichever of these happens first
(`TileSyncActivity.cpp`, `updateProgress()`):

- every tile settled -- landed or skipped;
- 30 s with nothing in flight and nothing settling (`kStallVerdictMs`), the
  stall verdict;
- 30 s of an active-but-silent transfer, the phone-ANR'd case;
- nothing to fetch at all, straight from `onEnter()` or `trackPhone()`.

**Consequence worth knowing before testing this by hand:** on a visit with
missing tiles, the freshness check does not run until the fetch is over. A
rider who leaves the screen while tiles are still arriving never sees it. That
is not a bug, but it is the reason a `SyncScreen`-mode check can look like it
never fires.

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

## Console flush could freeze the activity task for seconds -- capped 2026-08-13, and the cap does not hold

**Read off the code, not measured.** Found in the BLE review
(`../../docs/ble-review-2026-08.md`, "Stability -- firmware" item 4).

Each block `MapBleConsole` hands to `sendCommandBlock()` can cost up to
`kConfirmTimeoutMs = 3000` ms waiting for the peer's confirm
(`lib/BlePositionServer/src/BlePositionServer.cpp:509`), and that wait runs on
the activity task -- the same task that services buttons and redraws. Before
this fix, `MapBleConsole::poll()` (`src/activities/map/MapBleConsole.cpp`)
drained the whole command ring and flushed every batch that filled up in one
call, so a paged listing (several commands queued back to back, e.g. a script
issuing many `tiles`/`missing` asks) against a hung-but-subscribed phone could
chain an unbounded number of 3 s waits: dead buttons for 10 s or more on the
map or sync screen.

Fixed with a per-call cap, `kMaxBlocksPerPoll = 2`
(`src/activities/map/MapBleConsole.h`): `poll()` reads the command ring one
byte at a time now, not in one 128-byte gulp, checking the cap before taking
each byte -- so once 2 blocks have gone out this call, the rest of the queued
bytes are left **in the ring**, in order, for the next `poll()` tick, instead
of being read out and discarded. The trailing `flushReplies()` call at the end
of `poll()` is skipped the same way once the cap is spent; whatever is still
batched (`batch_`/`batchLen_`, both members) rides into the next call rather
than becoming a 3rd send.

The claim made here was **"worst case is now 2 x 3 s = 6 s per `poll()` call,
not unbounded"**. That claim is **false, and was measured false 2026-08-23** --
see the section below. What the cap actually bounds is commands per `poll()`,
not blocks per `poll()`. 1 block would halve the nominal ceiling but doubles how
many ticks a healthy-link multi-line listing needs to clear -- doubling
perceived latency for the common case to shave the pathological one. 2 is the
trade-off taken, and it is the wrong knob.

The escape hatch was already written down here, and it is the whole bug:
**a single command whose own reply needs more than 2 blocks is not covered.**
The cap is checked between bytes, i.e. between commands, not inside one
command's own burst of `appendReply()` calls (`MapBleConsole.cpp`) -- those
still flush unconditionally, because refusing a flush there would leave
`appendReply()` writing past a full, fixed-size `batch_[kBatchBytes]`
(253 bytes) instead of failing safely. This section then said no command on
this console produces a reply anywhere near 2 blocks today (`have` for a whole
viewport was 83 bytes, see above), "believed unreachable in practice, not proven
so". `info` at MTU 23 needs **23** blocks, and that is not an unusual link: 23
is the MTU every connection starts at.

`flushTransferStatus()` is untouched and needs no change: it already runs
after `ble_.poll()` in the same tick on both screens
(`src/activities/map/MapActivity.cpp`, `src/activities/map/TileSyncActivity.cpp`),
so a parked transfer-status line now gets a send attempt *between* two capped
console batches instead of after a whole listing -- the cap can only shorten
its wait, never lengthen it.

**`FETCH_CANCEL` on exit skips the same doomed wait.**
`BlePositionServer::lastConfirmTimedOut()`
(`lib/BlePositionServer/include/BlePositionServer.h`) is set inside
`sendCommandBlock()` when a confirm wait expires and cleared the next time one
succeeds (`lib/BlePositionServer/src/BlePositionServer.cpp:509-548`). Both
`MapActivity::onExit()` and `TileSyncActivity::leave()` check it before sending
`FETCH_CANCEL`: a peer that already let a confirm time out will not hear a
cancel either, so sending it would only add one more 3 s freeze on the way out
for a line nobody receives.

**Untested on hardware** as of 2026-08-13: builds clean, host tests for
`MapCommandParser` pass unchanged (they do not exercise `MapBleConsole` or
`BlePositionServer`), but nothing on real BLE hardware has exercised the
capped `poll()` or the `FETCH_CANCEL` skip yet. Still true of hardware. Both
have since been exercised in the desktop simulator -- see the next two
sections.

## The cap does not bound the freeze -- measured 69.1 s, 2026-08-23

**Measured in the desktop simulator** (`simulator.md`, "BLE"): the firmware's
real `BlePositionServer` and `MapBleConsole` over a fake radio, against a peer
that stops confirming. **Not measured on hardware; there is no device.** The
arithmetic is the device's own, so the number is a property of the code rather
than of the transport -- but see the caveat at the end.

One `info` at MTU 23, `auto_confirm` off at the fake radio so nothing is ever
acknowledged:

```
window 75 s, one `info` (23 reply lines at MTU 23)
indications delivered: 23  clobbers: 22
  t=    761.6 ms   11B  b'INFO pos=0\n'
  t=   3762.4 ms   19B  b'INFO lat=0.0000000\n'   gap  3000.7 ms
  t=   6762.8 ms   19B  b'INFO lon=0.0000000\n'   gap  3000.4 ms
  ... 20 more, every gap 3000.x ms ...
  t=  66835.8 ms    3B  b'OK\n'                   gap  3063.4 ms

gaps: min 2981.9 ms, typical 3000.x ms, max 3063.4 ms, mean 3003.4 ms
a 23-line reply at this rate needs 69.1 s to drain
```

The firmware's own log agrees on its own clock -- 23 timeouts, `grep -c "reply
unconfirmed"` returns 23:

```
[5567]  [ERR] [BLEPOS] reply unconfirmed after 3000 ms, 11 bytes dropped
[8567]  [ERR] [BLEPOS] reply unconfirmed after 3000 ms, 19 bytes dropped
...
[71641] [ERR] [BLEPOS] reply unconfirmed after 3000 ms, 3 bytes dropped
```

**Why the cap does not fire.** It is checked in the loop over **input** bytes:

```cpp
for (size_t i = 0; i < kBytesPerPoll; ++i) {
  if (blocksSentThisPoll_ >= kMaxBlocksPerPoll) break;   // MapBleConsole.cpp:106
  if (ble.readCommandBytes(&c, 1) == 0) break;
  if (console_.feed(c, out)) redraw = true;              // MapBleConsole.cpp:108
}
```

One input byte -- the `\n` that terminates `info` -- makes `feed()` run the
whole command, which calls `appendReply` 23 times. Neither `appendReply`
(`src/activities/map/MapBleConsole.cpp:56-79`) nor `flushReplies`
(`:81-89`) consults `blocksSentThisPoll_`; `flushReplies` only increments it,
and its own header comment says it is unconditional because "the cap is
enforced by `poll()` instead". `poll()` cannot enforce it, because there is no
next input byte to stop at. The trailing guard at `:116` covers the final
partial batch only. So the cap limits **commands per poll**, and the freeze it
was written to bound is per **command**.

**The whole main loop stopped, not just the console.** `[MEM]` is printed from
the top of `loop()` every 10 s, right after `gpio.update()` (`src/main.cpp:561`,
`gpio.update()` at `:567`, the print at `:572-576`). In that run there is **no
`[MEM]` line at all between 5567 ms and 68641 ms** -- 63 seconds in which
`loop()` did not iterate once, so buttons were not read either.

**A failed block does not stop the next one.** `appendReply` ignores the return
value of `sendCommandReply`/`flushReplies`, so a dead-quiet peer costs 3 s **per
reply line**, not 3 s once. That is what turns 23 lines into 69 s.

**This is the third time a "bounded" claim in this tree has been committed and
then disproved** (`CLAUDE.md`: "Never write 'deterministic', 'bounded' or
'cannot spin' in a comment until you have checked it"). The comment carrying it
in the code (`src/activities/map/MapBleConsole.h:61-71`) was corrected in the same
pass as this doc.

**What would fix it:** check the cap where the blocks are actually emitted, i.e.
inside `appendReply`/`flushReplies`, and let a capped reply ride into the next
`poll()` the way the queued input bytes already do. Not attempted here.

**Open -- needs hardware:** whether button starvation is observable to a rider
as claimed. The evidence above is the missing `[MEM]` heartbeat, which is where
`gpio.update()` also lives, not a button pressed during the stall and timed.
What would settle it: hold a button through the freeze on a device and watch
when it lands.

## The truncation that caused "0 stale of 1" is still reachable -- reproduced 2026-08-23

**Measured in the desktop simulator.** The 2026-08-13 fix above removed the
common case by batching a whole listing into one indication. The mechanism
underneath it is still live whenever one logical line needs more than one chunk,
which at MTU 23 is any line over 19 bytes.

Withhold the confirm for the **first chunk of a two-chunk line** and the failure
appears whole, for the first time deliberately rather than in the field:

```
[  757.8 ms] INDICATE #0  18B b'INFO have_total=2\n'   -> confirming
[  758.8 ms] INDICATE #1  20B b'INFO have_12_2267_14'  <-- CONFIRM WITHHELD
[ 3765.8 ms] CLOBBER      20B b'INFO have_12_2267_14'
[ 3783.9 ms] INDICATE #2  20B b'INFO have_12_2267_14'  -> confirming
[ 3784.8 ms] INDICATE #3  12B b'04=60e20d54\n'         -> confirming
[ 3845.9 ms] INDICATE #4   3B b'OK\n'                  -> confirming
```

```
[2551] [DBG] [MAPBLE] rx: have
[5553] [ERR] [BLEPOS] reply unconfirmed after 3000 ms, 20 bytes dropped
[5553] [ERR] [BLEPOS] command block send aborted: 0 of 32 bytes delivered
```

What the peer is left holding:

1. The first row's first 20 bytes arrive with no `\n`. They are a **prefix, not
   a line**, so nothing terminates them.
2. `sendCommandBlock` returns on the failed chunk
   (`lib/BlePositionServer/src/BlePositionServer.cpp:574-588`), so the
   remaining 12 bytes are **never sent by anybody**. That half is permanently
   lost -- unlike a clobber, this is not a transport artefact.
3. The next block is appended to the same byte stream, so a `\n` splitter joins
   the orphan to the next row and yields one line,
   `INFO have_12_2267_14INFO have_12_2267_1404=60e20d54`, which parses as
   neither row.
4. **The count is the only signal.** `have_total=2`, one usable row. A reader
   that trusts the rows and ignores the total answers a freshness question about
   half the screen -- "0 stale of 1" for a viewport holding two stale tiles,
   which is exactly the field symptom.
5. `OK` still arrives, so the exchange looks successful and `mapcmd.py` exits 0.

The phone's own count check (`HaveReader`, `FreshnessChecker`, added in the same
2026-08-13 pass) is what catches this, and this run is the first evidence it has
something real to catch. Nothing on the **device** side notices, and nothing in
`mapcmd.py` reports "I am holding an unterminated fragment".

**One log line reads wrong here, and it hides the truncation.**
`command block send aborted: 0 of 32 bytes delivered` says 0 while 20 bytes were
on the wire and the peer had them: `sent` only advances after a *confirmed*
chunk (`BlePositionServer.cpp:578-587`). The number is "bytes known delivered",
not "bytes emitted". Read literally it says nothing left the device, which is
the opposite of the problem.

Withholding the confirm for a whole line's **only** chunk does *not* reproduce a
hole, and that is a property of the simulator rather than of the firmware: the
fake radio emits the payload and only later reports it clobbered, so the peer
receives every byte plus a `clobber` event, 3 s late. On hardware the clobbered
indication was genuinely lost. So `--sim` can reproduce the
truncate-and-merge case above, where the firmware itself stops sending, and
cannot reproduce a loss caused by the radio.

## `FETCH_CANCEL` on exit: the skip is exercised, 2026-08-23

**Measured in the desktop simulator**, three runs with one variable, which is
what makes it decisive. `lastConfirmTimedOut_` is set at
`lib/BlePositionServer/src/BlePositionServer.cpp:647` and cleared at `:658`; the
getter is `BlePositionServer.h:210`; the consumers are
`src/activities/map/MapActivity.cpp:2100` and
`src/activities/map/TileSyncActivity.cpp:525`.

| run | confirms | on the wire at exit | firmware log |
|---|---|---|---|
| control | on | `FETCH_CANCEL` | no skip line |
| withheld | off, one confirm times out first | nothing | `autosync: FETCH_CANCEL skipped: last confirm already timed out` |
| cleared | off, then on, then one good reply | `FETCH_CANCEL` | one `reply unconfirmed`, no skip line |

So the flag goes true on a timeout, suppresses the cancel, and is cleared by the
next successful confirm, exactly as written. `TileSyncActivity.cpp:525-528` is
line-for-line identical to `MapActivity.cpp:2100-2103` and was **not**
exercised -- reaching the Tile Sync screen needs menu navigation the run did not
script.

## Test scenarios for the freshness check

Written 2026-08-14, after the BLE fix plan touched this path twice: T4.3 gave
`FreshnessChecker` a generation guard and a stale-reply debt, T5.3 put the
listing on fast link parameters. Neither has been on hardware.

**Start from what is already covered, so nothing is re-tested by hand.**
`FreshnessCheckerTest` (`android/app/src/test/java/org/explorink/gpsbridge/`)
holds 25 plain-JVM tests, and they already pin: a matching content id is not
reported stale; a differing one is reported once with the expected id kept; a
whole viewport is one range read; an unreachable CDN answers `unknown` and never
`stale`; a truncated `have` listing answers `unknown`; the offline backoff; a
`CHECK_TILES` restart neither lets the stale write finish the new run nor feeds
the new reader the old reply; and the fast link is released exactly once on each
of success, truncation, silence, a hung index read, a dropped link and a restart
answered immediately. Those are settled on the laptop. Hardware scenarios should
only chase what a unit test cannot see.

**The blocker, stated first: there is no laptop stand-in for this conversation.**
`tools/blefakephone.py` speaks the missing-tile fetch (`NEED_TILES`, `missing`,
begin/chunk) and does **not** speak `CHECK_TILES`, `have`, `stale` or `checked` —
grep it for those words and they are absent, and its own comment says it fetches
tiles the device does not have at all, with no `?crc=`. So every scenario below
needs the real Android app today, which is why the autosync path has
`tools/autosync_gate.py` and this one has nothing, and why two of its bugs (the
truncated `have` listing, the dropped reply lines) were found late and by
accident rather than by a gate.

### The highest-value work here is a harness, not a checklist

Teaching `blefakephone.py` the four freshness verbs would make S1, S3, S4 and S6
below scriptable and repeatable, the way `autosync_gate.py` and
`ble_reply_gate.py` already are for their features. It needs: answer
`CHECK_TILES` by reading the device's `have` listing, byte-range read the CDN
`.idx` (the logic already exists on the app side and in
`../../docs/tile-index-spec.md`), reply `stale <z> <col> <row>` per differing
tile then `checked <n>`, and push each stale tile with `?crc=<content_id>`.
Until that exists, treat the list below as a manual pass.

### S1 — the happy path, both ends agreeing

Runnable today with the real app. Freshness must be switched on
(`mapTileFreshnessMode`, off by default).

1. Open the map with the phone connected. Wait for the check to run.
2. Expect on the device: `CHECK_TILES <count> fmt <version>` out, `have` listing
   answered, and a verdict line that is **not** `checked unknown`.
3. Read the counts on the panel against the store: the check queue draws dots,
   the fetch queue draws outlines (see "The check queue is dots" above).

Disproves: nothing agreeing end to end. This is the baseline every other
scenario is measured against, and the one that catches a format-version
mismatch between the ends.

### S2 — a tile that is genuinely stale gets replaced

Runnable today, needs a rebuilt tile on the CDN.

1. Note a tile the device holds and its `content_id`.
2. Rebuild that tile so its `content_id` changes, push it to the CDN, regenerate
   the index (`build_index.py`) and push the mirror — both steps, or the index
   still describes the old tile and the check cannot see the change.
3. Run the check.
4. Expect: `stale` for exactly that tile, the phone pushes it unasked with
   `?crc=<content_id>`, and the device's `contentId()` for it afterwards matches
   the published index.

Disproves: the whole feature. This is the only scenario that proves the signal
works rather than that the plumbing moves.

### S3 — restart mid-listing (T4.3's case)

Runnable today, and the one this session's work most needs.

1. Start a check on a store large enough that the listing takes several pages.
2. While it is still listing, trigger a second check — press the menu item
   again.
3. Expect: the new run completes with a correct count, and **no** `stale` for a
   tile the old run named. The app logs the discard
   (`owedListingReplies`).

Disproves: the second half of review item 3. A unit test pins the state machine;
only hardware pins it against a device that really does keep answering the dead
question.

### S4 — the known open gap: a reply the device drops

**Not runnable without new tooling**, and the scenario most worth building for.

The debt is paid only by the stale reply's terminating `OK`
(`docs/ble-map-transfer-protocol.md`, "A restart of the same ask still has a
stale reply in flight", second known gap). Firmware drops a whole reply after
`kConfirmTimeoutMs` (3000 ms) of an unconfirmed indication and says so:
`reply unconfirmed after N ms, M bytes dropped`. To force it, the peer must stop
confirming indications for 3 s while a listing reply is in flight — no current
tool does that, and the real app always confirms.

1. Peer stops confirming for >3 s mid-listing, then restart the check.
2. Expect, if the gap is real: the next listing's reply is eaten line by line
   and that check dies on the app's 15 s `REPLY_TIMEOUT_MS`.
3. The remedy is written down and not built: clear the debt when the new
   request's write is acked.

Disproves: whether the gap is reachable at all. Right now it is arithmetic and a
code read, nothing more.

### S5 — the loop guard holds

Runnable today, needs a CDN that serves a stale copy.

1. Make a tile report `stale` whose fetched replacement still does not match.
2. Expect: `StaleTilesList::onArrived()` remembers it was fetched because it was
   stale, and the second `stale` report becomes `giveUp()` rather than another
   entry — see "A stale fetch that does not fix the tile must stop asking".
3. Expect no endless fetch loop, and the give-up to survive `clear()`.

Disproves: the loop guard. There is real battery behind getting this wrong.

### S6 — T5.3's fast link, on a real link

Runnable today; needs the app's session log rather than the panel.

1. Run a check and read the app's `conn_params` events (T2.2) around it.
2. Expect the interval to be at the fast set while the listing runs and back to
   idle after it, exactly once per exit — the unit tests pin the call counts, this
   pins that the phone honours them.
3. Time the listing. The review's arithmetic said 9 indication round trips at
   688–1503 ms each is 6–14 s against a 15 s timeout; H1 measured intervals of
   15–30 ms on this build, so the expectation now is comfortably under that.

Disproves: that T5.3 bought anything measurable. It may turn out the link was
never the constraint on this build, which is a result worth having in writing.

### S7 — the check does not lie when it cannot answer

Runnable today.

1. Put the phone offline (airplane mode is enough) and run a check.
2. Expect `checked unknown`, **not** `checked 0`, nothing marked, and the offline
   backoff engaged so the device is not asked again immediately.

Disproves: the discipline the whole feature rests on — a check that reports
"all current" when it could not read the index is worse than no check, because it
marks tiles settled.
