# Plan 04 — missing tiles and the sync flow

Reviewed against `70da0b86`. Two commits landed on `develop` **while this review
was being written**: `412e0ed9` moved the fetch out of `MapActivity` into
`TileSyncActivity`, and `70da0b86` gave that screen a Waiting phase that tracks
whether a phone is subscribed at all. An earlier draft of this plan asked for the
first and part of the second; both were done better than the draft proposed. What
is below is against the shape after both.

## How it works now

**read**, at `412e0ed9`:

**Recording, on the map screen.** A tile that fails to open, or whose layer CRC
fails, sets a bit in `unavailableMask_` (`MapTileSource.cpp:76`, `:96`).
`renderViewport()` hatches it and calls `MISSING_TILES.record()`
(`MapActivity.cpp:1003`). A membership change arms a 10-minute flush
(`MapActivity.cpp:1014-1016`); a count-only bump does not
(`MissingTilesStore.h:32-36`).

**Fetching, on its own screen.** `TileSyncActivity` is entered from the home
menu (`HomeActivity.cpp:118`, `HomeMenuItem::TILE_SYNC`). It starts its own BLE
peripheral, attaches its own receiver, keeps its own `MapConsoleState`, sorts the
store into fetch priority, snapshots that order into `rows_`, and sends
`NEED_TILES <count> fmt <version>` (`TileSyncActivity.cpp:36-100`).

**Row state is derived, not accounted for** (`TileSyncActivity.h:41-53`):
active from `Status::activeTile`, skipped from the `IMapSkipObserver` callback,
done from "gone from the store", waiting otherwise (`stateOf`, `:228-244`).

That design decision is the right one and it is the reason two of the items below
are small rather than large. Deriving state from the store means there is no
second ledger to drift.

Two more things `412e0ed9` got right and worth naming so they are not undone:

- **The skip observer is a synchronous callback, not a snapshot.** A "last skip
  plus a counter" snapshot loses the second of two skips between two polls; the
  callback cannot (`MapCommandConsole.h`, the `IMapSkipObserver` comment).
- **Sync is preparation, not navigation.** The screen is reached from home, not
  from the map's CONFIRM menu, because nobody stops mid-trail to sync map data
  (`TileSyncActivity.h:24-27`). That is a product decision and it removed a
  render mode from an activity that had too many.

## Item 1 — arrivals on the map screen no longer clear the list

**read, and new in `412e0ed9`.** The map screen still owns a
`MapTransferReceiver` and still attaches it (`MapActivity.h:294`,
`MapActivity.cpp:385`), so a phone can still push a corridor tile while the map
is up — which `MapActivity.h:290-293` says is the point of it being attached
there.

But `drainTransferredTiles()` was removed from `MapActivity` in that refactor and
now exists only in `TileSyncActivity` (`:207-219`). `MapActivity`'s only
`MISSING_TILES` calls are `record()` (`:1003`), `isDirty()` (`:1014`) and
`flushIfDirty()` (`:468`, `:597`). Nothing calls `forget()`.

Failure scenario: a phone pushes tile `z12 2210/1420` while the rider is looking
at the map. The file lands, is CRC-checked, is renamed into place, and the map
stops hatching that square on the next reset. The store keeps its entry. The next
time the rider opens Tile Sync, that tile is a row, the phone sends the whole
file again, and it lands on top of a file identical to it.

Cost: one wasted transfer per corridor-pushed tile — which is exactly the waste
the `fmt` field in `NEED_TILES` was added to prevent
(`MapTileReader.h:148-157`).

Two ways to close it:

1. **Re-add the drain to `MapActivity::loop()`.** Four lines plus a
   `lastClearedTileSeq_` member. The comment justifying which task may write the
   store still holds — the activity task is the store's only writer, and
   `MapActivity` is an activity task (`TileSyncActivity.h:113-118`). The drain
   code is identical in both screens, so it wants to be a shared free function
   next to `MissingTilesConsoleSource.h`, not copy-pasted.
2. **Decide the map screen does not accept pushes.** Drop `transfer_` from
   `MapActivity` entirely. That also closes plan 03's step 6 window for free,
   and it fits the "sync is preparation" reasoning
   (`TileSyncActivity.h:24-27`) — if syncing happens at home on its own screen,
   the map screen has no reason to hold a transfer channel.

**(2) is the more consistent choice** and it deletes code rather than adding it.
It is a product decision, not a mechanical one: it removes the unsolicited
corridor push from the map screen. If that push is wanted, do (1).

Either way this needs deciding, because right now the map screen holds a channel
whose results it half-processes.

## Item 2 — a ring of landed tiles, not one slot

`Status` carries one `lastTile` plus a `tileSeq` counter
(`MapTransferReceiver.h:98-115`). The consumer compares its own copy and acts
when they differ (`TileSyncActivity.cpp:207-219`). Two tiles landing between two
`loop()` iterations collapse into one — documented as theoretical on the grounds
that a whole file takes seconds (`MapTransferReceiver.h:106-111`).

The new screen makes the consequence worse than it was, and plan 03 makes the
race more likely.

- **Worse consequence.** Row state is derived from the store. A missed
  `forget()` leaves that row reading **Waiting forever**, while the summary line
  counts it done, because the summary comes from `transfer.completed`
  (`TileSyncActivity.cpp:407`, `:437`). So a missed arrival ends the sync with a
  row still saying it is waiting. Before the refactor the same miss cost one
  stale list entry and nothing visible.
- **More likely.** `loop()` on this screen does a full-frame e-ink refresh on
  every settle (`:439-448`) — hundreds of milliseconds during which no drain
  runs. With plan 03's MTU and interval work, a small tile is a couple of
  seconds. The window for two arrivals inside one iteration stops being
  theoretical.

Fix: an 8-entry ring of `MapTileCoord` in `Status`, written by the host task,
drained by the activity task, same publish-under-critical-section discipline as
now (`MapTransferReceiver.cpp:400-414`). 8 × 9 bytes plus two indices.

## Item 3 — the completion test can finish early

**read.** Completion is `done + skipped_ >= rowCount_` where
`done = transfer.completed` (`TileSyncActivity.cpp:437`, `:442`).

`completed` counts **every** file that landed since the screen opened, including
files that are not tiles and tiles that are not on this run's snapshot
(`MapTransferReceiver.h:92-95` says so). So a phone that pushes anything not on
the list advances the completion counter, and a sync can declare itself done with
rows still Waiting.

Fix: count settled **rows**, not landed files. The screen already computes each
row's state (`stateOf`, `:228-244`); completion is "no row is Waiting or
Active". That is one loop over `rows_`, and it uses the same derived state the
list draws, so the summary line and the verdict can no longer disagree with the
rows.

Same change makes the summary line honest: `%lu / %lu` should be settled rows
over total rows, not `completed` over `rowCount_`.

## Item 4 — a phone that stays but stops answering

**Mostly fixed by `70da0b86`.** That commit added `Phase::Waiting` and
`trackPhone()` (`TileSyncActivity.cpp:126-144`), watching
`BlePositionServer::isCommandSubscribed()` — a new accessor that answers the
question `sendCommandReply()` could not, because NimBLE accepts an indication
into its queue whether or not anybody is subscribed
(`BlePositionServer.h:148-155`). A phone that walks out of range now puts the
screen back to Waiting instead of leaving a bar that will never move, and
re-subscribing re-sends `NEED_TILES` (`:59-63` of that diff, `askForTiles()` at
`.cpp:106`). That is a better answer than the timeout this plan originally asked
for: it says which of two different things went wrong, and it recovers on its own.

What is left is narrower: **a phone that stays subscribed and simply stops
sending.** An app bug, or a tile it cannot supply and does not `skip`. Then there
is no arrival, no skip, no unsubscribe — `phase_` stays Running and
`updateProgress()` (`:435-473`) has no deadline.

Fix, if it is worth it: one `millis()` deadline refreshed by any arrival, any
skip and any change in `transfer.received`, expiring to Finished with its own
verdict. `MapTransferReceiver::kStaleTransferMs` = 30000 is the existing constant
to reuse (`MapTransferReceiver.h:126`).

Lower priority than it was. The common failure — no phone, or the phone left — is
now handled and visible.

## Item 5 — `firstVisibleRow()` is O(n²), and it takes 200 critical sections

**read.** `firstVisibleRow()` calls `stateOf()` for every row until it finds an
unsettled one (`TileSyncActivity.cpp:270-293`). `stateOf()` calls
`transfer_.status()`, which takes a
`portENTER_CRITICAL` (`MapTransferReceiver.cpp:416-421`), and `stillMissing()`,
which is a linear scan of the store (`TileSyncActivity.cpp:221-226`).

At the 200-entry cap that is up to 200 critical sections and ~20,000 comparisons
per call. `drawList()` calls it once per repaint (`:374`) and `rowRect()` calls it again
per single-row update (`:302`, from `:466`).

Not a visible problem — a repaint happens at most every 2 s
(`kActiveRowRefreshMs`, `TileSyncActivity.h:177`) against a refresh that costs
hundreds of
milliseconds. But taking a critical section 200 times in a loop disables
interrupts 200 times while the NimBLE host task is trying to service a transfer,
and that is the one cost here that is not just wasted cycles.

Fix, small: hoist `transfer_.status()` out — take one snapshot per repaint and
pass it into `stateOf()`. That drops 200 critical sections to 1 and is a pure
refactor. The `stillMissing()` scan can stay; it is plain memory.

## Item 6 — resume a broken transfer

**read.** A link drop abandons the `.part` file and deletes it
(`MapTransferReceiver.cpp:80-90`); resume is a documented non-goal
(`BlePositionServer.cpp:113-119`).

Right call for one tile. Wrong call for a sync of 50 tiles at home over a phone
that may lock its screen.

The protocol already carries what resume needs: the chunk frame is keyed on a
byte **offset** checked against bytes actually written
(`MapTransferReceiver.h:32-36`, `.cpp:228-233`).

Minimal version, no new opcode:

- On `abandon()` from a link drop only (reason `nullptr`), keep the `.part` and
  remember `{finalPath, declaredTotal, declaredCrc, received}`.
- On the next `begin` for the same path with the same total and CRC, reopen for
  append, set `received_` to the file's size, and answer
  `RDY <total> <received>`.
- A sender that ignores the second number sends offset 0, mismatches, and the
  existing check abandons cleanly (`:228`).

Unchanged: the file is renamed into place only after the whole-file CRC read-back
matches (`:262-278`). A resumed file gets the same verification as a fresh one.

Own branch, own revision of `docs/ble-map-transfer-protocol.md` in the parent
repo, in the same commit. It changes the wire contract and the phone app is being
written against that doc.

## Checked and left alone

- **`record()` is a linear scan** over up to 200 entries
  (`MissingTilesStore.cpp:45-46`), at most 9 times per reset. ~1800 integer
  comparisons against a reset costing the better part of two seconds. No change.
- **`forget()` uses `erase()`, not swap-and-pop**, so priority order survives
  (`MissingTilesStore.cpp:76-87`). Correct.
- **The 10-minute flush is a rate cap armed once**, not a settle timer
  (`MapActivity.cpp:1010-1016`). The comment explains why re-arming would mean a
  long coverage gap never saves. `onExit()` also flushes (`:468`). Leave it.
- **`rows_` is heap, not a member array** — 200 entries is ~2.4 KB and the
  activity would carry it whether or not a sync ran (`TileSyncActivity.h:155-162`).
  Correct call, `makeUniqueNoThrow` with an OOM path that ends the screen with a
  verdict rather than crashing (`:51-62` of `onEnter()`).
- **Eviction picks the lowest count** (`MissingTilesStore.cpp:55-65`), which can
  evict a regional tile seen once in favour of a detail tile seen twice — the
  eviction policy and the tier-first fetch policy disagree. Unreachable at 200
  entries (200 z12 tiles is far more than a day's ride). If the cap ever drops,
  evict by `missingTileFetchBefore` so one policy governs both.

## Commit sequence

1. `fix(tilesync): finish on settled rows, not landed files` — item 3.
2. `fix(map): decide what a push to the map screen does` — item 1, whichever way.
3. `perf(tilesync): one transfer snapshot per repaint` — item 5.
4. `feat(ble): drain a ring of landed tiles` — item 2.
5. `fix(tilesync): time out a phone that stays but stops sending` — item 4's
   remainder, if wanted.
6. Resume (item 6) on its own branch.
