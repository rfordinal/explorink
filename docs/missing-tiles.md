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
> store and the `TileSyncActivity` that consumes it. One open item is still
> open: completion counts landed *files* rather than settled rows, so a sync can
> declare itself done with rows still waiting.
>
> The other -- arrivals on the map screen clearing nothing, a regression from
> `412e0ed9` -- was fixed on 2026-08-07 by
> `MapActivity::drainTransferredTiles()`, which the autosync work below needed
> anyway. Read off the code, not yet run against a real arrival.

## A tile that is out of date is a different list

This store is about tiles the device **does not have**. A tile that opens fine
and draws fine but has been republished since is a different question with a
different answer, and it is **deliberately not recorded here**. Two properties
of this store make it the wrong home:

- Its records persist, so a stale entry would survive the fetch that fixed it
  and the device would come back up asking for a tile it already holds.
- It evicts by hit count, so a burst of stale entries would be dropped first --
  or, in the live check, would push out genuinely missing tiles.

`StaleTilesList` (`src/activities/map/StaleTilesList.h`) keeps those in memory
instead, empty on every boot. See [`tile-freshness.md`](tile-freshness.md) for
the signal, the setting and the loop guard.

## A valid tile can still be a hole

`MapTileSource` counts a tile as unavailable when it has **no geometry in any
layer** (`MapTileReader::hasAnyGeometry()`), not only when it fails to open.

Such tiles exist on cards built before 2026-08-06. mapbuilder padded its fetch
bbox out to the coarsest LOD's tile grid but took tile identity from the
geometry, so every tile the padded extract happened to touch was written --
producing a ring of tiles holding one clipped sliver, or nothing at all
(../../docs/map-data-spec.md, "A tile is written only where the build was asked
to cover"). The builder no longer does this; the check stays because the cards
do.

Why it must count as unavailable rather than draw: the file is valid, so every
crc passes and nothing would hatch, `unavailableMask()` would stay clear, and no
entry would reach this store. The panel would show white -- **empty countryside**
-- over a square that is 13 km across at z11 and has never been surveyed. Hatch
says "no data here", which is the truth, and it is also the only state that gets
the tile onto the missing list where a fetch can ask for it.

The trade, stated plainly: a genuinely empty tile -- mid-lake, unmapped forest --
now hatches too. That is the safer of the two wrong answers. "No data" invites a
fetch; "empty countryside" invites riding into it.

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
(`src/activities/map/TileSyncActivity.{h,cpp}`). Verified end to end on hardware
2026-08-06 -- see "Verified vs assumed" at the end. What happens, in order:

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

### The link's real parameters are logged now

`info` reports `mtu` and `chunk_payload`, and the server logs the negotiated MTU
and the connection interval when a central connects
(`lib/BlePositionServer/src/BlePositionServer.cpp`, `ServerCallbacks::onConnect`
and `onMTUChange`). Step 1 of `docs/optimization/03-ble-link.md`, and the reason
it comes first: everything about transfer speed depends on two numbers the
**central** decides, and neither can be inferred from a return value.

At the 23-byte default MTU a chunk carries 15 bytes of file. At 256 it carries
248. That is the difference between a "fill the gaps" button worth pressing and
one nobody presses twice, and until this landed the device could not say which
link it had. A phone-side developer cannot see the number from their end at all,
which is why it is on the console and not only in the log.

Reported as a provider, not a pushed value: the MTU changes when a central
connects, and a number pushed once would report the last link's MTU forever. It
reads 0 with nothing connected, and `info` then omits both lines rather than
claiming an MTU of zero.

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

## Autosync: the map screen asking for what it just hatched

**Off by default.** Settings > Map > *Auto-sync tiles*
(`SettingsList.h`, `CrossPointSettings::mapAutoSyncTiles`). On, a frame that had
to hatch anything asks the connected phone for exactly those tiles, and the map
redraws itself when they land.

This is the mid-ride case the sync screen above deliberately is not. Nobody
opens a second screen at a junction, and the tiles that matter there are the
nine on the panel, not the two hundred in the store.

**It cost no new activity and no new allocation.** Everything it needs was
already on the map screen: `BlePositionServer::begin()` and `transfer_.attach()`
in `onEnter()` (`src/activities/map/MapActivity.cpp`), the command console the
phone answers on, and the store the hatch loop already writes to.

What was added, counted rather than estimated (2026-08-07): one setting byte,
**11** members on `MapActivity` (7 for autosync, 4 for the header row's drawn
state), and **158 non-comment lines** in `MapActivity.cpp` plus the store's
`markRefused`/`isRefused` and the Settings-screen wiring. Every member is on an
activity that is allocated once per screen entry, so none of it is resident while
the map is closed.

### The conversation

`NEED_TILES <count> fmt <version> view`, an unsolicited indication -- the sync
screen's ask with one extra word. `view` tells the phone to answer from `tiles`
(the current viewport, at most 32 entries, each flagged `missing` or `ok`)
rather than page `missing` (everything, up to 200). Full wire detail in
`../../docs/ble-map-transfer-protocol.md`, "The `view` ask".

The count is computed in the hatch loop itself
(`MapActivity::drawMapLayers()`): it already walks each missing tile to draw the
hatch and record it, so counting there costs nothing. It is **published, not
acted on** -- `autoSyncWantCount_` is read by `loop()`. A render must not start
a BLE conversation part-way through drawing a frame.

### Three rules, none of them optional

**Viewport only.** See above. The rest is what the sync screen is for.

**A refused tile is never asked for twice.** `skip` used to be counted and
nothing else. It now also sets `MissingTileHit::refused`
(`MissingTilesStore::markRefused()`), and `drawMapLayers()` skips a refused tile
when counting what to ask for. Without this the feature actively harms: a rider
parked at the edge of coverage re-hatches the same squares on **every viewport
reset** (that is why `record()` distinguishes a first appearance from a count
bump, above), so every reset would be a fresh ask for tiles the phone has
already said it does not have -- forever, on the phone's mobile data.

The flag costs nothing, and this is measured rather than reasoned: compiled
side by side with the old struct on the host (2026-08-07), both are **16 bytes**
and `refused` sits at **offset 1**, inside the padding the `uint32_t` alignment
already forces. So the 200-entry cap is still 3.2 KB. It is **not** persisted --
a tile the CDN lacks today may exist next week, and a restart is the natural
moment to find out.

Anything added to this struct should be measured the same way. "It will fit in
the padding" is a claim about the compiler, not about the code.

**One ask per `kAutoSyncIntervalMs`** (60 s, `MapActivity.cpp`), and never a
second while the first is outstanding. A rate cap, not a settle timer: more
hatching does not push the next ask further out.

### The globe

A circle with an equator and a meridian, left of the Bluetooth logo in the
header row (`MapActivity::drawHeaderStatus()`). Lit while `autoSyncPending_ > 0`
-- tiles asked for and not yet settled by an arrival or a `skip`.

The device has no radio that reaches the internet; the phone does. The glyph is
still honest about the thing that matters to the rider: **mobile data is being
spent on their behalf right now.**

It should cost **two waveform passes per fetch, not per tile** -- the row is
repainted only when its state actually flips, and only the strip, through
`displayBufferWindow()`. Read off the code, **not counted on hardware**: a
successful windowed repaint logs nothing, so counting them needs a build that
says when it repaints. The row, its geometry and its repaint policy are their own
topic: [`map-header-status.md`](map-header-status.md).

### Every arrival owes a redraw -- not just the one that settles an ask

`drainTransferredTiles()` arms `arrivalRedrawDueMs_` whenever `forget()` removed
an entry, **whatever the ask is doing**. The panel is hatching a square the card
now holds, and that is the one thing hatch must never mean.

Tying the redraw to an ask settling was the first version and it was visibly
wrong: a tile that arrived outside an ask -- pushed by hand, or landing after its
ask had already expired -- was filed away while the map kept the hatch. Seen on
the panel 2026-08-07.

It is a **settle** timer (`kArrivalRedrawSettleMs`, 5 s), pushed out by each new
arrival, not a rate cap. Arrivals come in bursts, each redraw is the better part
of two seconds of waveform, and the frame worth spending is the one after the
last tile. It is also deliberately not `armRedraw()`'s deadline: a button press
must not be made to wait behind a transfer.

### When an ask ends

- **Every tile settled** -- arrivals plus skips reach the count. The globe goes
  out. Any redraw owed was already armed by the arrivals themselves; a run where
  every answer was `skip` owes none, because the panel's hatch is still the right
  picture.
- **`kAutoSyncQuietMs` (45 s) with no bytes moving.** The deadline is on
  **silence, not on elapsed time**: `expireAutoSync()` rearms it every time the
  receiver's byte counters move, so a slow transfer stays alive and only a phone
  that walked away ends the ask.

  A flat three-minute budget for the whole ask was the first version, and the
  panel showed why it was wrong: **395 KB at 2.6 kB/s is 147 s for one tile**
  (measured 2026-08-07), so the ask expired mid-transfer and the tile that landed
  afterwards had no ask left to settle. Detail tiles twice that size exist on the
  card.
- **The rider leaves the map** -- `onExit()` sends `FETCH_CANCEL`, the same word
  `TileSyncActivity::leave()` sends and for the same reason: the abort opcode is
  a frame the central writes, so a peripheral's only cancel is on the command
  channel.

### What this fixed on the way past

Arrivals on the map screen cleared nothing from the store -- the regression from
`412e0ed9` noted at the top of this doc. `MapActivity::drainTransferredTiles()`
now exists, so a tile pushed while the map is up drops its own entry, and the
next sync stops asking for tiles the card already holds.

### A subscription is a subscription, notify or indicate

The command characteristic is created `WRITE | NOTIFY | INDICATE`
(`lib/BlePositionServer/src/BlePositionServer.cpp:232-233`), so the **central**
picks which. BlueZ picks notify; the Android app picks indicate.

`CommandCharCallbacks::onSubscribe` used to count only the indication bit, and
that quietly broke autosync for every notify-subscribing client:

- Replies reached them perfectly well. `onStatus` fires for a notification too,
  so `sendCommandReply()`'s confirm wait returns normally.
- But `isCommandSubscribed()` answered false, so **every unsolicited ask was
  withheld** -- `NEED_TILES` from this screen and from the sync screen both.

Measured on hardware 2026-08-07: a bleak client connected, subscribed, ran
`tiles` and received all four reply lines, while the device logged `command
channel unsubscribed` and never asked for anything. It now counts either bit
(`subValue & 0x0003`).

The transfer *status* characteristic still counts indications only, and there
the asymmetry is not a choice: it is created `INDICATE`-only, so notify is not a
subscription a central can make.

**Anything that gates behaviour on somebody listening should use
`isCommandSubscribed()`, never the reply path's return value.** `indicate()`
succeeds into an empty room.

### Verified vs assumed (autosync)

- **Verified end to end on hardware, 2026-08-07** (this branch, `fmt 3`, a
  laptop standing in for the phone via `tools/autosync_gate.py` in the parent
  repo). Every step of the gate passed:
  - **The ask goes out unprompted and is viewport-scoped**: `NEED_TILES 4 fmt 3
    view` after a frame that hatched 4 tiles, and `NEED_TILES 2 fmt 3 view` at a
    second gap. **The latency was not measured** -- the gate waits 20 s for the
    frame before it looks, so all that is known is "inside 20 s of the `pos`".
  - **`tiles` answers the viewport**, four entries, all `missing`.
  - **`skip` settles the ask** -- four skips, tally 1..4, and the globe went out.
  - **A refused tile is not asked for again.** After the four skips, the rate cap
    was waited out in full (65 s) and the same position re-hatched: **no second
    ask**. The positive control in the same run -- a different gap -- still
    produced one, so the silence was the flag, not a dead feature.
  - **An arrival clears its entry and redraws the map on its own.** With one ask
    outstanding, `tools/blepush.py` pushed the real tile: `[MAP] z13 4490/2852
    arrived, dropped from the list`, then the hatch was gone, the globe was out
    and the frame had repainted **with no `redraw` command sent**. A following
    `tiles` reported `ok`, so the file is readable at the path it landed on.
  - **The whole loop, on a dense tile, watched on the panel.** At a gap the card
    was missing two z13 tiles (23 KB and 322 KB), autosync asked, both were
    pushed as answers, and 5 s after the last one the map redrew itself into a
    real town: `2t 479w`, ink over the map area 3.37% -> 6.61%, globe out. No
    command was sent to make it happen. The 322 KB tile took 120 s, which the
    quiet timer rode out without expiring the ask.

    This is the check that caught both bugs above, and it caught them because it
    was watched **on the glass**. The gate answers every ask with `skip`, so it
    exercises the refusal path and never the arrival path -- a green gate said
    nothing about either.
  - **The globe draws and is not clipped** -- `CMD:SCREENSHOT` with an ask
    outstanding shows the ring, equator and meridian clear of the Bluetooth logo.
    At 14 px it reads as a crosshair-in-a-circle as much as a globe; legible and
    unambiguous against everything else in that row, but if it should look more
    like a globe, curving the meridian is the change.
  - **Transfer rate is the connection interval, again.** `tools/blepush.py` at a
    45 ms interval: 34,915 B in 13.1 s and 2,430 B in 0.9 s, both 2.6-2.7 kB/s.
    The phone app at 12.5 ms: 317,895 B in 34.6 s, **9.0 kB/s**. Same MTU (256,
    248 B per chunk) in every one of them, so the interval is the whole
    difference -- which is what `docs/optimization/03-ble-link.md` says and what
    the 2.4 -> 7.4 kB/s pair measured on 2026-08-06 already showed.
- **Verified on the host**: compiles clean (`pio run`, default env, 2026-08-07).
  260/260 tests green -- **which covers none of this change**: `MissingTilesStore`
  needs ArduinoJson and `PersistableStore`, neither of which the native tests
  link, and `MapActivity` has no host test at all. The suite says nothing else
  broke, not that this works.
  - Static RAM reads 17.6%, the same figure this doc recorded before the change.
    **Not a same-day A/B**: no baseline build of `develop` was measured, so this
    is "matches what was written down", not "measured unchanged". It is also the
    expected result either way -- `MapActivity` is heap-allocated, so its new
    members are not in `.bss`.
  - Device heap with the map screen up: **55.2-56.1 KB free** across five samples
    in the session log, at zoom rungs 0 and 1 with 1-2 tiles loaded.
- **Verified against the real phone app, 2026-08-07** -- the whole chain, with
  nothing standing in for anything. A tile under the rider's actual position was
  deleted off the card over WebDAV (`DELETE /trailink/base/13/4485/2843.tib`,
  204, then 404 to confirm), the map was reopened and the app connected:

  ```
  13:08:30.779  device wants 1 tiles, format 3, scope viewport, source is CDN
  13:08:30.970  list complete: 1 tiles of 1
  13:09:05.542  landed z13 4485/2843 (317895 bytes)
  13:09:05.557  fetch finished: done (1 sent, 0 skipped of 1)
  ```

  `scope viewport` is the app reading the `view` word and answering from `tiles`.
  The listing took **191 ms** -- one request, no paging. The tile came off the
  CDN at exactly the byte count the CDN serves, and the map redrew itself on the
  device.

  **9.0 kB/s** for that transfer (317,895 B in 34.6 s), against 2.6 kB/s for the
  same file pushed by `tools/blepush.py`. The difference is the connection
  interval and nothing else: the app asks for a high-priority link and Android
  gave it 10 units (12.5 ms), then handed it back to 24 units (30 ms) 180 ms
  after the fetch ended. That scoping works, and it is worth 3.5x.
- **Still not verified**:
  - **The quiet timer actually expiring.** The old flat budget was seen to expire
    (that is how it was found wrong), but nothing has yet gone 45 s silent with
    an ask open. What would settle it: start a push, kill the client mid-file,
    and watch the globe go out 45 s later with no further ask for that tile.
  - **Interleaved arrivals and skips** crossing each other inside one ask, and a
    tile arriving that the outstanding ask did not request.
  - **The rate cap under a moving rider**, where viewport resets come from real
    fixes rather than from `pos` commands.

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
- **Verified end to end on hardware, 2026-08-06** (build `develop-d02a5faf`,
  format v3, tiles served by the live CDN and pushed by the phone over BLE):
  - The whole chain ran: `NEED_TILES 10 fmt 3` out, the list paged back, the
    phone fetched from `tiles.trailink-app.com/v3/` over LTE, and **5 tiles
    landed with their CRC verified** -- 322 kB in 43 s. The other 5 came back
    `skip ... nosource` because the CDN genuinely does not hold them, and each
    arrival cleared its own entry from the store.
  - **The connection interval is the transfer's ceiling, and the phone can move
    it.** Logged on the device: connected at 24 units (30 ms), then 12 units
    (15 ms) one second later when the app asked for a high-priority link, then
    back to 30 ms in the same second the fetch ended. Throughput went from
    **2.4 kB/s to 7.4 kB/s** -- 3.1x, and the MTU (256, 248 B per chunk) had not
    changed between the two runs. The scoping works: the fast interval is held
    for the fetch and no longer.
  - The panel's own numbers were wrong twice before they were right, both times
    caught by looking at the screen rather than the code -- see "What the panel
    shows" below.
- **Still not verified**: a page of 20 `missing` replies over BLE (this run's
  list was 10, so the indication burst was never near the page size), and a
  transfer interrupted mid-file by a real link drop.

## What the panel shows, and two mistakes it took to get there

Both were numbers that were **true and still misleading**, which is the failure
mode worth remembering here:

1. **A stray percentage under the list.** `BaseTheme::drawProgressBar` always
   writes a centred percentage 15 px below its bar -- right for the single large
   bar it was written for (`FontDownloadActivity`), wrong for a list. Ten
   6-pixel row bars produced ten labels, each landing on the next row's text and
   each erased by the next row's fill, leaving one number under the list that
   read as overall progress and was actually the last row's state. It sat at 0%
   for a whole run. Rows now draw their own outline and fill, with no label.
2. **A full bar on a run that transferred nothing.** The overall bar counted
   *settled* tiles, and a tile the supplier does not have settles the run -- so
   0 landed of 5 read as 100%. It counts arrivals now, and the unavailable count
   is stated in the line above where it cannot be read as progress. Measured
   after the fix: `Fetch finished 0 / 5   5 not available   0 B`, bar at 0%.

Also: a tile the supplier lacks is shown as **"not available"**, not "skipped".
The wire verb stays `skip` -- that is the protocol -- but from the rider's side
nothing was skipped, and "skipped" reads as a choice somebody made.

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

## One queue on the sync screen, missing tiles first (2026-08-11)

**Measured on hardware, and it was broken.** The sync screen used to send both of
its asks at once -- `CHECK_TILES` (is what I hold still current?) and `NEED_TILES`
(here is what I do not have) -- 15 ms apart. The phone answers each with a command
on the same channel, so two conversations ran at once, and a reply listing on this
channel ends with a plain `OK`. Each conversation could therefore be ended by the
other's terminator.

What that looked like on the wire (serial capture, 2026-08-11):

```
[786768] [MAPBLE] rx: have      <- freshness conversation opens
[786888] [MAPBLE] rx: missing   <- fetch conversation opens, `have` still open
[788200] [MAPBLE] rx: checked 0
```

and on the phone, in the same second: `device wants 20 tiles`, then
`list complete: 0 tiles of 20`, then `fetch finished: done (0 sent, 0 skipped)`.

The device asked for 20 tiles and was told nothing. Worse than nothing: with no
arrivals and **no `skip` lines**, nothing could move a row, so all 20 sat at
`waiting` for as long as the rider cared to watch. That is the reported symptom --
"I open sync tiles, everything says waiting, nothing happens, I don't understand
what it is doing."

Three changes, all in `TileSyncActivity`:

- **One ask at a time, missing tiles first.** `askForTiles()` goes out alone;
  `askAboutFreshness()` is held until the fetch settles (`updateProgress`) or
  until there is nothing to fetch. Missing first because a square the rider has
  no map for beats one they have an older copy of.
- **A stall verdict.** `kStallVerdictMs` (30 s, matching the transfer channel's own
  stalled-transfer reclaim) with nothing in flight and nothing settling ends the
  run with `STR_TILE_SYNC_NO_ANSWER` instead of leaving rows in limbo.
  **Measured on hardware 2026-08-12** with a deliberately silent central
  (`tools/blefakephone.py --no-tiles`, which subscribes and ignores the ask):
  `asked for 17 tiles` at 2306767 ms, `no answer for 30000 ms, 0 landed,
  0 skipped` at 2336771 ms -- 30,004 ms -- and the panel read *Phone sent nothing
  / 0 / 17  0 B*. The freshness ask followed the verdict, so the queue's second
  half is released by a stall as well as by a normal finish. The
  protocol has no "I am done" from the phone and cannot usefully have one -- a
  phone out of range would not send it either -- so silence is the only signal
  there is, and a screen that reads silence as work is a screen that lies.
- **`queued`, not `not available`.** A square the supplier does not have is
  written down and asked for again -- the list is persisted (`missing_tiles.json`)
  and survives a reboot. The run states it once, in `STR_TILE_SYNC_NOT_BUILT`:
  *Not on the server yet. The request is saved and asked for again.* It does not
  claim anyone was told to build it: nothing reports these gaps upstream today,
  the map server being static files with no API
  (`../../docs/tile-index-spec.md`).

### A phone that turns up after the run gets asked

`trackPhone()` used to return immediately on `Phase::Finished`, so a screen that
had finished ignored every later subscribe. The rider who watches a run end with
nothing, *then* connects their phone, got no ask, no message, and nothing on the
panel to say the screen had stopped listening. Found on hardware 2026-08-11 while
setting up the stall-verdict test: a central subscribed to a finished screen
(`[BLEPOS] command channel subscribed`) and the screen never reacted -- no
`phone subscribed, asking` line at all.

A finished run now re-arms on a subscribe: `armRun()` re-snapshots the list and
zeroes everything a run reports, then the ask goes out again. Bounded by connect
events rather than polling -- it only fires on a false-to-true transition, so a
phone that stays connected cannot make it loop.

**Measured on hardware 2026-08-12**, by bouncing the phone's Bluetooth against a
finished screen: `command channel subscribed` at 86184 ms, then
`phone arrived after the run, asking again (0 tiles)` at 86189 ms, then the
freshness ask again -- which is also the proof that `armRun()` cleared
`freshnessAsked_`, since `askAboutFreshness()` returns early otherwise. Before the
fix the same subscribe produced no log line at all.

**Open:** the re-arm with a *non-empty* list, i.e. `askForTiles()` going out a
second time and `MapTransferReceiver::resetCounters()` actually zeroing the first
run's arrivals. The list was empty at test time, so that branch did not run. What
would settle it: a device with hatched squares, a finished run, then a phone
arriving.

`armRun()` exists because a second run on one visit needs the same starting state
a fresh entry has, and one of those pieces is easy to miss:
`MapTransferReceiver`'s counters are "since the screen opened", so without
`resetCounters()` the second run would start with the first one's arrivals already
on the board. That reset refuses while a transfer is in flight, and takes the same
critical section the publish path uses.

### The finished screen is a result, not a list

The row list is what a rider watches while a fetch works, and it is irrelevant the
moment it stops -- the maintainer's words, watching a real run. A finished run
therefore draws no list and no bar: the verdict at UI_12, the numbers under it, and
when squares were queued, the reason as the biggest thing on the screen. That is
the one question the screen exists to answer.

Two lines rather than one for the queued sentence, because the single-line version
ran off the right edge at that size -- measured on the panel, cut mid-word after
about 48 characters.

The phone side enforces the same rule independently -- a second ask arriving
mid-conversation is deferred, not answered (`android/README.md`). Both ends need
it: an older device build still fires both asks.

## Distance from the last fix decides what goes out first (2026-08-11)

Inside a LOD tier the order was hit count, so a square hatched forty times on last
week's ride outranked the one the rider is riding into now. A fetch is routinely
cut short -- the rider leaves, the battery goes, the phone walks out of range -- so
the first minute has to spend itself near the rider.

`MissingTileAnchor` (`../src/MissingTilePriority.h`) carries the last known fix in
tile coordinates, one pair per tier, and the comparator's keys are now: tier,
distance, hit count, col/row. Distance is Manhattan in whole tiles -- a coarse
bucket, not a measurement, computed in integers because this runs inside a sort
comparator on a core with no hardware floating point.

- The anchor comes from `SETTINGS.mapHasLastFix`, i.e. the **persisted** fix, not a
  live one (`../src/activities/map/MapMissingAnchor.h`). The sync screen has no
  viewport and is normally used at home with the GPS off; the persisted fix is the
  best available statement of where the rider is, and it survives a power cycle.
- No fix ever taken means `valid == false` and the old order stands, unchanged.
- **The same anchor must reach the console source.** `missing` re-sorts the store
  when the phone starts paging (`MapCommandConsole`, offset 0), while this screen
  drew its rows from its own snapshot. Two different orders would label rows for
  tiles the phone was never told about, and every arrival would tick the wrong
  row. `MissingTilesConsoleSource::setAnchor()` is that wiring.

Verified: 12 native tests in `test/missing_tile_priority` (near-beats-far,
tier-still-first, ties still fall through to count then col/row, no-fix
unchanged, order still total). **Not yet measured on hardware** -- what a real
list looks like once sorted around a real last fix.
