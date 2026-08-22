# Follow the marker, not the map

A GPS fix does not redraw the map. It moves the marker inside the frame the panel
is already holding, and refreshes only the rectangles that changed. The map is
redrawn when the marker runs out of room, not when the rider moves.

Status: **verified on real hardware 2026-08-05** by replaying a recorded ride
(`blereplay.py`, 176 packets, Bratislava, zoom step 2 / 6 m/px / LOD z12). No
crash, no leak, heap flat at 58,824 bytes for the whole run. The measured numbers
are in "What the ride measured" below; anything still unmeasured says so.

> **Optimisation review, 2026-08-06.** Before changing this path, read
> [`optimization/05-map-activity-structure.md`](optimization/05-map-activity-structure.md)
> — it extracts the patch save/restore into a `MapMarkerLayer` and names the
> ordering invariant that is currently held only by a comment (the patch must be
> saved after every other layer is drawn). `kMaxPartialMoves` is still untuned;
> [`optimization/07-power-and-lifecycle.md`](optimization/07-power-and-lifecycle.md)
> says what measurement settles it.

> **This policy has no idea a route is loaded**, and on a switchback road that
> costs a redraw every 500 m or worse.
> [`route-navigation.md`](route-navigation.md) has the measurements. It also puts a
> number on `kMaxPartialMoves`: at 12 the marker crosses 18 % of the screen it was
> given before a full frame is forced.
>
> **Since 2026-08-07, check 1 does not apply when a route is loaded.**
> `Request::routeHoldsFrame` turns it off, leaving keep-in and the budget as the
> only triggers, because the marker's arrow is already drawn relative to the frame
> and carries the heading without turning the map. Verified on the panel: the same
> 136 fixes over a switchback pass gave 11 full redraws and 9 different frame
> orientations without a route, and 0 redraws in 1 orientation with one. So the
> table below describes **free-ride** behaviour; read `route-navigation.md` before
> touching this ladder.

## An unbounded window aborts the device (2026-08-22)

A marker move refreshes **one window over both boxes**, the old marker's and the
new one's, because a windowed refresh costs the same panel time as a full one
whatever its area (measured 2026-08-05, below). Nothing bounded that union.

Two far-apart boxes make it the whole panel. And a full-panel window is not
merely slow: `GfxRenderer::displayBufferWindow()` returns `bool` and every call
site here falls back to a full refresh on `false`, but the driver under it
allocates `(w/8)*h` bytes through a **throwing** `std::vector`
(`freeink-sdk/.../driver/Ssd1677Driver.cpp`). On a `-fno-exceptions` build a
throwing allocation is `abort()`, never `false`. So the fallback never gets a
chance.

**Measured 2026-08-22, from a coredump off the device:** `abort()` in
`loopTask`, `operator new` -> `__cxa_throw` -> `std::terminate` ->
`panic_abort`, with the driver asking for **48,000 bytes** for a window of the
whole panel. The panel froze, the log stopped, and no button on the device could
bring it back (`../../../docs/device-notes.md`).

The same failure was already measured 2026-08-17 on the menu-close path and is
described in a comment in `restoreMenuBackdrop()`. That comment says not to
refresh the whole panel there -- and the code three lines above it computes
`h = screenHeight - rect.y`, which *is* the whole panel whenever a dialog
reaches `y == 0`. Documented, and unguarded.

**When the marker jumps far enough to matter:** right after a viewport
re-anchor -- a pin's `Show`, a console `goto`, a route overview, anything that
frames somewhere the rider is not. The next fix then moves the marker across the
panel, and the union of the old and new boxes is most of the screen.

`Nearby -> View on map` is the one that made this ordinary rather than rare: it
re-anchors on a POI that can be kilometres away, and a rider browsing points
crosses that transition many times in a session. The feature did not create the
bug, it made it reachable.

**Fixed** by deciding affordability before the call, against the largest block
the heap can actually give: `MapActivity::windowRefreshAffordable()` compares
the driver's own `(w/8)*h` arithmetic plus a 12 kB margin against
`ESP.getMaxAllocHeap()` -- the same number the `MEM` log line prints. Both
panel-capable call sites go through it: the marker union and the menu-close
window. Everything else that windows here is fixed furniture (the busy badge at
34 px, the header strip at 36 px, the pin notice, the side hints).

**Still open, and the real fix:** the driver should not be able to abort at all.
`Ssd1677Driver::displayWindow` wants a no-throw allocation and a `false` return;
every caller in this repo already handles `false`. That is in the `freeink-sdk`
submodule, so it is its own change in its own repo, and until it lands every new
windowed refresh has to remember this on its own.


## Why

A viewport reset is the most expensive thing the map screen does: tile reads off
the SD card, a full `MapRenderer` pass, then a whole-panel waveform. Put at "the
better part of two seconds" by `MapActivity.h`'s coalescing note, which is where
the button settle timer comes from. The real per-reset number is logged every
time -- `renderViewport()` emits `framebuffer ready in %lu ms` -- so it is
checkable, but no figure is quoted here as measured.

The Android bridge sends on distance, not on a timer -- 10 or 25 m per packet
(`android-install.md` in the unpublished map workspace, which is where the
Android side is documented). At 6 m/px a 10 m step moves the marker under
two pixels. Paying a full reset for that is paying everything for nothing: the
map on the panel is still correct, and only a 64x64 patch of it is wrong.

## The decision

`src/activities/map/MapFollow.h` owns it, as pure integer arithmetic with no
renderer, projection or HAL dependency -- which is why it is unit-tested on the
host (`test/map_follow/MapFollowTest.cpp`).

`MapActivity::applyFix()` (`MapActivity.cpp`) projects the new fix **through the
projection the frame on screen was drawn with** -- deliberately not a fresh one.
The question is "where does this fix fall in the picture already up". Then
`MapFollow::decide()` answers with one of three actions:

| Action | What happens | Cost |
|---|---|---|
| `Skip` | Nothing is drawn, the panel is not touched | zero |
| `MoveMarker` | Restore the patch, redraw the marker, windowed refresh | one small waveform |
| `ReAnchor` | Full `renderViewport()` | tiles + full frame + full waveform |

The checks, in order (order is load-bearing -- see below):

1. **Heading drift ≥ 4 steps (90°), and at least 2 partial moves since the last
   full frame** → `ReAnchor`. The map is track-up, so the frame is only correct
   for the heading it was drawn with. The move-count part is
   `kMinPartialMovesForHeadingReAnchor`, added 2026-08-08 -- see "Heading
   thrash, round two" below; before that this check fired on drift alone,
   moved or not.
2. **Marker within the keep-in margin of a screen edge** → `ReAnchor`. One
   marker ring plus 26 px of slack: inside this frame the marker's box never
   straddles the panel edge, and there is still map ahead to look at. 80 px
   while the marker was one size; **per rung since 2026-08-12** (66 at rung 5,
   59 at rung 6), because the marker shrinks at the coarse rungs -- `zoom-rungs.md`,
   "Three things that are per rung now". `MapActivity` passes it as
   `Request::keepInMarginPx`; `kKeepInMarginPx` is now the fallback for callers
   with no rung in hand.
3. **12 moves since the last full frame** → `ReAnchor`. Windowed refreshes are
   differential and ghost. `kMaxPartialMoves`, the starting point named in
   the map workspace's
   `firmware-implementation-plan.md`, open decision 4 ("every 10-20
   marker updates, needs on-device tuning"). **Unverified** -- the number that
   fits is whatever real ghosting turns out to allow.
4. **Movement under the rung's move floor on both axes** → `Skip`. Below this a
   waveform buys a marker that visibly did not move. 8 px at every rung until
   2026-08-12, which was 8 m of ground at rung 0 and 360 m at rung 6; now per
   rung (12, 10, 8, 8, 6, 3, 2 px), which holds the ground step level instead of
   the pixel step -- `zoom-rungs.md`. `MapActivity` passes it as
   `Request::minMovePx`; `kMinMovePx` is the fallback.

Every redraw reason is checked before the movement floor. A rider standing still
who has turned 90°, or one crawling along with a spent ghosting budget, still
gets the redraw -- the floor must not swallow it. `MapFollowDecide`'s
`HeadingDriftReAnchorsEvenStandingStill` and
`GhostingBudgetBeatsTheMovementFloor` pin that.

### The marker ladder's last rung broke check 2 -- fixed 2026-08-15

**Reported by the maintainer**: the marker reached the bottom of the screen
(the look-ahead ladder's deepest step) and the map would not stop redrawing;
pressing the button that pulls the marker back up made it stop.

**Verified by reading the code, not yet on hardware.** `kMarkerLadder`'s last
entry is 760 (`src/activities/map/MapViewport.h:181`), on an 800 px screen.
Check 2's margin (`keepInMarginPx`, above) is `ring + kKeepInSlackPx`
(`MapActivity.cpp:2717`) -- 80 px at rungs 0-4 (full-size marker), 66 at rung
5, 59 at rung 6 (`MapMarkerMetrics.h:65-82`). `insideKeepIn()`
(`MapFollow.cpp:14-16`) requires `y < screenHeight - marginPx`, i.e. `y < 720`
at the worst case. 760 fails that at every rung (720-741, all below 760).

That is not "close to the edge", it is **outside the box the rest of this
policy assumes the marker settles inside**. `decide()` checks the *new fix's*
absolute position (`MapFollow.cpp:31`), and after a `ReAnchor` the fix sits
exactly at `markerYForStep()` -- so parking on this step means every following
fix, moving or not, fails check 2 and forces another `ReAnchor`. The map
cannot stop redrawing once the marker is at this step, by construction, not
because of GPS movement.

**Fix**: `MapActivity::stepMarker()` now refuses to move onto a step whose
`markerYForStep()` would already violate the largest margin (rungs 0-4's 80
px, the worst case across every rung) -- `MapActivity.cpp`, `stepMarker()`.
The ladder's last rung becomes unreachable via the button rather than reached
and stuck; the ladder itself was not renumbered.

**Open**: whether this is the whole story, or GPS jitter alone (without ever
reaching step 4) can also land a resting fix outside the margin at a coarser
rung -- unmeasured. Needs a ride at rung 5/6 with the marker left at a
mid-ladder step to check.

## Erasing the marker

Single-buffer mode has no shadow copy of the frame (`EINK_DISPLAY_SINGLE_BUFFER_MODE`,
CLAUDE.md), so the map pixels under the marker exist nowhere once the marker is
drawn over them. They are saved first:

- `MapActivity::saveMarkerPatch()` reads the marker's 64x64 halo box out of the
  framebuffer into `markerPatch_` via `GfxRenderer::readFramebufferRegion()`
  (`lib/GfxRenderer/GfxRenderer.cpp:1560`), which snaps the rect outward to the
  controller's multiple-of-8 columns and is orientation-aware.
- 720 bytes, allocated once in `onEnter()` next to the tile source. A move must
  not allocate (CLAUDE.md's heap-fragmentation rule) and this is far past the
  256-byte stack-local cap. Against a full frame's 48,000 bytes, and against the
  SD read that re-rendering the patch from tiles would cost.
- `moveMarker()` writes the patch back, **then** saves the new position's
  background, then draws. Restore-before-save matters: with the two boxes
  overlapping -- the common case -- saving first would capture the marker it is
  about to erase.
- The patch is taken **after** the map, hatch, compass, readout and button hints
  are all in the framebuffer, at the very end of `renderViewport()`. A marker low
  on the screen sits over the hints; save earlier and restoring it would paint
  the hints through the marker's old position.

If the save fails (OOM, or a box the read rejected), follow is off: `applyFix()`
sends every fix to `renderViewport()`. Correct picture, expensive picture, no
stale marker.

## The refresh

`GfxRenderer::displayBufferWindow()` (`lib/GfxRenderer/GfxRenderer.cpp:1556`)
down to `FreeInkDisplay::displayWindow()`. Byte-aligned columns are handled for
the caller. Same call the busy badge already uses (`showBusy()`).

Two boxes change per move -- where the marker was and where it is. `moveMarker()`
sends **one window over both, always**.

**A windowed refresh is not cheaper than a full one. Measured on the X4,
2026-08-05, reproduced over two full replays of the same ride.** Every windowed
refresh took **500 ms** (62 of them in the first run, 61 in the second), and so
did every whole-panel refresh in the same runs -- the only values the driver's own
`Wait complete: refresh (N ms)` ever printed were 499 and 500. The waveform is a
fixed cost; the window only narrows which pixels it touches. Area does not enter
into it.

That inverts the obvious design. An earlier version of this code split a
far-apart pair of boxes into two windows, on the assumption that a refresh costs
by area — which would have doubled both the latency and the panel current for no
gain. One union window, however wasteful its area looks, is right: **the thing to
minimise is the number of refreshes, not their size.**

So the saving is not on the panel at all. It is the framebuffer work:

| | framebuffer | panel | total |
|---|---|---|---|
| full viewport reset | 7,892-10,254 ms (median ~8,300) | 500 ms | ~8.9 s |
| marker move | ~3 ms | 500 ms | ~0.5 s |

**~17x cheaper per fix**, and every millisecond of it comes out of tile reads and
the `MapRenderer` pass, not out of the waveform. `MapActivity.h`'s coalescing note
still says "the better part of two seconds" for a reset; at zoom step 2 over
Bratislava (22,904 ways, 4 tiles, 1.57 MB read) it is four times that.

### The entry frame is HALF, not FULL -- fixed 2026-08-17

Opening the map screen used to blink several times through an apparent negative
before the map appeared. Reported from the panel; not a normal e-ink clear.

`onEnter()` sets `pendingEntryCleanRefresh_` (`MapActivity.cpp:1755`) so the
first real frame of the activation gets a non-differential refresh -- the fast
LUT is differential and cannot clear whatever the previous screen left. That part
is right. What was wrong is which non-differential mode it asked for: it asked
for `FULL_REFRESH`, and on X4 `FULL_REFRESH` selects the OTP full waveform
`0xF7` (`Ssd1677Driver.cpp:59,186`), which is the multi-flash one. Several
inversions per refresh, by design of the waveform.

Which mode means what on this hardware, what the driver promotes behind the
caller's back, and why `FULL` is never the right ask:
[`refresh-modes.md`](refresh-modes.md).

`HALF_REFRESH` (`0xD7`) is the single-pass absolute clean, and it is the only
clean primitive stock X4 firmware uses in normal operation -- the driver says so
(`Ssd1677Driver.cpp:360`) and the sleep screen already relies on it for exactly
this reason (`SleepActivity.cpp:154`, "#2471's blinking complaint"). It clears
the panel and seeds the differential baseline just as `FULL` does, without the
flashing.

So all three entry frames -- the viewport, the route overview, the waiting banner
(`MapActivity.cpp:3726,4116,4495`) -- now pass `HALF_REFRESH`.

Entry still costs two refreshes, not one: `renderLoadingTiles()` paints the
"reading tiles" logo first (`FAST_REFRESH`, `MapActivity.cpp:3768`) because the
tile read behind it takes seconds and needs feedback. One fast differential pass
plus one clean pass.

**Verified on the X4 2026-08-17**, build `319a8c5f`, off the driver's own
`Wait complete: refresh (N ms)` line after a `CMD:GOTO_MAP` on a device with a
persisted fix:

| frame | mode | waveform |
|---|---|---|
| `renderLoadingTiles()` logo | `FAST_REFRESH` | 500 ms |
| the map | `HALF_REFRESH` | 1,684 ms |
| the next fix's re-anchor | `FAST_REFRESH` | 500 ms |

So `HALF`'s waveform is **3.4x a `FAST`'s**, which is worth knowing on its own:
the 500 ms figure in "The refresh" above is a `FAST`/windowed number and does not
generalise to the absolute waveforms.

**That column is the waveform, not the frame.** `Wait complete: refresh (N ms)`
brackets only the BUSY wait after the refresh is triggered (`EpdBus.cpp:220`);
the controller RAM writes happen before it (`Ssd1677Driver.cpp:398-407`) and are
not in it. The modes write different amounts: a non-differential mode writes both
planes before the wait and, in single-buffer mode, both again after to reseed the
differential baseline; `FAST` writes one. Off the log gaps between `framebuffer
ready` and the start of each wait -- both frames being full `renderViewport()`
calls, so the remaining draw work is identical -- that is 95 ms for the `HALF`
frame against 27 ms for the `FAST` one, which puts ~68 ms of the difference on
the extra plane writes. Derived from two timestamps, not instrumented directly.

`FULL_REFRESH` was never timed before the change, so what `HALF` saves in
milliseconds is unknown. **That the flashing is gone is not something the log can
show** -- one `Wait complete` line per frame does not rule out a multi-inversion
waveform, because `0xF7` is also one trigger and one BUSY wait, just a longer
one. The evidence is the mode the code now asks for, plus the maintainer looking
at the panel on 2026-08-17 and confirming it. For this claim a person's eyes are
the instrument; nothing in the firmware can be.

### Entry could also drop the first real fix -- fixed 2026-08-17

Same report: after opening the map, a BLE position that arrived did not move the
map off the fix restored from the card. Only riding corrected it.

The bulk of that is on the phone -- its send policy had no reason to send at all
on a reconnect, see the parent workspace's `send-interval-analysis.md`. The
firmware had a second, narrower version of the same bug.

`onEnter()` seeds the persisted fix as if it had been received:
`hasReceivedAny_ = true`, `showingPersistedFix_ = true`, `lastDrawnSeq_ = 0`
(`MapActivity.cpp:1746,1919-1928`). The BLE intake then only accepts a packet
whose `seq` differs from `lastDrawnSeq_` (`MapActivity.cpp:2072`), which is the
right guard against re-drawing a packet already drawn. But `seq` is a rolling
0-255 counter the phone owns (`BlePositionServer.h`), so a phone whose counter
happened to sit at 0 had its first real packet read as "already drawn" and
discarded -- and nothing advances that counter while the rider is parked.

Fixed by accepting any packet while `showingPersistedFix_` is still set: a frame
that carries the card's fix has never had a real packet drawn into it, whatever
the counter says.

1 in 256 by itself, so this is not what the maintainer saw -- the phone-side bug
is. Read off the code; not reproduced on hardware, and not reachable to test
without forcing the phone's counter.

**The phone-side half is verified on the X4 2026-08-17**, build `319a8c5f`
against the app of the same day. Opening the map with the rider standing still,
the log shows the persisted fix drawn and then, 56 ms after `onEnter done`, a
real packet arriving and re-anchoring the frame:

```
[24864] [DBG] [MAP] onEnter done
[24920] [DBG] [MAP] ble fix: seq 2, heading 0, speed 0 km/h, accuracy 10 m
[24921] [DBG] [MAP] renderViewport start: ... seq=2
```

The rider had not moved, so before the app fix nothing would have arrived at all.
`seq 2` says only that this was the second packet the app process had sent -- its
counter is per process, not per link, and was not touched by the fix.

### That 8.9 s is a city number. Rural is ten times cheaper

**Measured 2026-08-06** on a forested pass in the Malé Karpaty, route loaded,
`framebuffer ready in N ms` off the device log, two runs per rung:

| rung | ways | bytes | framebuffer | full reset |
|---|---|---|---|---|
| 1 (3 m/px) | 186 | 113 kB | 536 ms | ~1.04 s |
| 2 (6 m/px) | 563 | 51 kB | **400 ms** | ~0.90 s |
| 3 (12 m/px) | 2,083 | 309 kB | 1,133 ms | ~1.63 s |
| 4 (20 m/px) | 4,443 | 433 kB | 1,750 ms | ~2.25 s |

Same rung 2, same firmware: 400 ms against Bratislava's ~8,300. **Cost per reset
is a property of the geometry under the screen, not of the device**, so no single
figure describes it -- quote the terrain with the number, and measure where the
change will actually run.

Two things follow. The **waveform becomes the bulk of the cost** out of town (500
of 900 ms), so out there the only lever is redrawing less often, not rendering
faster. And **a coarser rung costs more, not less**, because it covers more ground
and so more geometry -- rung 4 is 4.4x rung 2 here. Reaching for zoom-out to buy
cheaper redraws gets the sign wrong.

[`route-navigation.md`](route-navigation.md) has the measurement in context.

## What the ride measured

Replay of `trailink-gps-20260804-152206.jsonl` (Bratislava, 5.7 x 3.6 km,
`--speed 4`), zoom step 2, ride mode. 176 packets on the wire; the device
processed **117** of them -- `BlePositionServer::getLatest()` keeps only the
newest, so packets arriving inside one `loop()` iteration coalesce, which is
existing behaviour and not part of this change.

| outcome | count | cost each |
|---|---|---|
| `Skip` -- panel untouched | 31 | 0 |
| `MoveMarker` -- one windowed refresh | 71 | ~0.5 s |
| `ReAnchor` -- full redraw | 14 | ~8.9 s |

Before this change all 117 would have been full redraws: **~1,040 s of work
against ~160 s**, six and a half times less. Zero patch-save failures, zero
rejected windows, zero errors. Heap flat: 58,824 bytes free throughout, minimum
48,612 -- unchanged from before the replay started, so the one-allocation-per-
session patch buffer leaks nothing.

**The keep-in frame never fired.** All 14 re-anchors came from the other two
checks: 11 from heading drift, 3 from the ghosting budget. At 6 m/px the marker
would need 3.7 km in one direction to reach the 80 px frame (rung 4's margin,
still 80), and the budget
always ran out first. The margin is a safety bound, not the governing constant.

### Heading thrash, and why the fix is not in this firmware

**7 of the 14 re-anchors happened within one marker move of the previous one**
(moves between consecutive re-anchors: 0, 8, 1, 8, 12, 12, 0, 12, 1, 0, 11, 0, 6,
0). A rider going round a corner turns through more than 90 degrees in a couple
of fixes, and each crossing re-orients the map: two ~8.9 s full redraws back to
back, for a heading that is transient.

The obvious firmware fix is a dwell -- turn the map only once the new heading has
persisted for a few fixes. **It was not built, deliberately**, because the phone
already does exactly that and is the right place for it. `HeadingTrend.kt` derives
heading from the trend across a five-fix window and rejects the window outright
when the leg-to-leg bearings disagree with the overall trend by more than 45
degrees -- which is precisely what a corner looks like. Through a corner the phone
holds its previous bearing and sends nothing new to turn the map by.

**Measured on hardware, and it does not help on this ride.** The ride was
re-derived end to end under the new phone logic
(`tools/rederive_heading.py --write-log`, parent repo) and replayed against the
panel:

| stream | fixes processed | redraws | per fix | back-to-back |
|---|---|---|---|---|
| as sent, run 1 | 117 | 14 | 12.0% | 7 |
| as sent, run 2 | 116 | 14 | 12.1% | 7 |
| re-derived with `HeadingTrend` | 125 | 18 | **14.4%** | 7 |

Slightly *worse*, not better, and the thrash count did not move. 15 of the 18
redraws were heading drift.

An offline estimate had predicted 15 redraws with only 2 back-to-back. **That
estimate was wrong, and the way it was wrong is worth keeping.** It counted
*packets* between consecutive re-anchors, while the device counts *marker moves*
between them -- and the device only processes about 60% of the packets that arrive
(`BlePositionServer::getLatest()` keeps the newest, so anything landing inside one
`loop()` is coalesced). Two different denominators, so the two "back-to-back"
numbers were never comparable. `rederive_heading.py` is still the right tool for
comparing heading *streams*; its thrash column is not a prediction of device
behaviour.

So `HeadingTrend` is not a fix for this, whatever else it improves -- it makes the
heading track the rider's real direction more faithfully, and a faithful heading
through a corner is exactly a heading that changes. The other ride
(`20260804-080910`) looks like a large win offline (44 re-anchors down to 11), but
that stream had 22% of its packets jumping 90 degrees or more, which reads as a
worse derivation being replaced rather than as thrash being cured. Unverified on
hardware.

The dwell still does not belong here, and the measurement does not change that.
The project's standing split is that **the navigation head lives in the phone**:
the phone has 1 Hz fixes, real speed and history to judge with, the device sees
only what was sent, and a second filter in series would be worse informed than the
first. See `architecture-plan.md`, "The navigation head lives in the phone".

What *is* the device's own call is the threshold -- "how far off can this frame be
before it is no longer worth holding" is a comparison against what is drawn, not
an inference about the rider, so it belongs here. `kMaxHeadingDriftSteps` at 5
(112.5 degrees) was built, flashed and replayed against the same log:

| drift limit | fixes | moves | skips | redraws | per fix | back-to-back |
|---|---|---|---|---|---|---|
| 4 (90 deg) | 125 | 70 | 37 | 18 | 14.4% | 7 |
| 5 (112.5 deg) | 122 | 72 | 34 | **15** | **12.3%** | 6 |

**Kept at 4.** Three redraws saved across a 22-minute ride does not buy a map that
can sit 112 degrees off the direction of travel, with the road ahead running
sideways across the screen. (An offline estimate had again promised more -- 8
redraws, not 15. Same lesson as above: estimate streams here, measure the device.)

The reason the win is small is worth keeping, because it will apply to any future
attempt at this: **the two redraw triggers partly substitute for each other.**
Raising the heading limit means more marker moves survive, which means the ghosting
budget runs out sooner -- the gap list shows three 12-move budget redraws at limit
5 against two at limit 4. Buying fewer heading redraws partly buys budget redraws
instead, and only the difference is real.

Open, and now the top item on this screen: heading drift caused 15 of 18 redraws,
and 7 landed within one marker move of the previous one. At ~8.9 s each that is
the single most expensive thing the map screen does, and neither of the two levers
tried (the phone's derivation, the device's threshold) moved it much. The
framebuffer cost itself -- 8 to 14 s for one frame -- is the other half of the
problem and probably the more promising one to attack.

### Heading thrash, round two: gate it on movement, not on the turn

**Measured on hardware, 2026-08-08.** Three real rides recorded the same day
(`docs/rides/trailink-gps-2026080{7-142303,7-173058,7-201221}.jsonl`, parent
repo -- motorcycle, ride mode, zoom step 4 / 20 m/px, no route), replayed
packet-for-packet against the real on-device `MapFollow::decide()` over serial
(`tools/replay_ride.py`'s method, `pos <lat> <lon> heading <h>` per packet):

| ride | packets | redraws | heading | budget | keep-in |
|---|---|---|---|---|---|
| 1 | 339 | 24 | 20 | 4 | 0 |
| 2 | 389 | 23 | 19 | 4 | 0 |
| 3 | 369 | 20 | 13 | 7 | 0 |
| **total** | **1097** | **67** | **52 (78%)** | **15 (22%)** | **0** |

Keep-in fired zero times across all three -- confirms the margin is a safety
bound, not a governing constant, this time at 20 m/px rather than the 6 m/px
this doc's earlier measurement used.

**The new number, and why it points somewhere the earlier round did not**: of
the 52 heading redraws, **29 (56%) had 0 or 1 marker moves since the last full
frame** -- the marker had barely moved when the heading swung past the 90°
limit. That is GPS heading noise at low speed or a stop, not a turn in
progress. Ride 2's packets 230-246 show it clearest: six re-anchors in a row,
heading swinging 14→5→15→4→0→11→15, the marker not moving at all between any
of them (`pkt 230/234/236/237/241/242/246`, each `0 moves in` in the replay
log).

This is a different lever from both of the ones "Heading thrash" above already
tried and rejected:

- **Not the phone's `HeadingTrend` dwell** (raising confidence that a *turn* is
  real from a 5-fix trend) -- that was tested and made the redraw count
  *worse* (14 → 18), because it also holds back genuine corner turns.
- **Not `kMaxHeadingDriftSteps` itself** (how far a turn has to go before it
  counts) -- raising it to 5 saved 3 redraws out of 18 and cost orientation
  accuracy for every real turn, kept at 4.

This lever is orthogonal to both: it does not ask "was this really a turn", it
asks "has the marker actually moved since the frame was drawn" -- a fact
`MapFollow` already tracks (`partialMoves`), not an inference about the rider.
`kMinPartialMovesForHeadingReAnchor = 2` (`MapFollow.h`) adds this as a second
condition on check 1: heading drift only forces a redraw once at least 2
partial moves have landed since the last one.

**The trade-off, taken deliberately**: `HeadingDriftReAnchorsEvenStandingStill`
used to test the opposite -- a rider turned 90° with zero movement still got an
immediate redraw. That test is now
`HeadingDriftAloneDoesNotReAnchorWhileStandingStill`
(`test/map_follow/MapFollowTest.cpp`) and expects `Skip` instead. A rider who
turns while genuinely parked or stopped at a junction no longer gets the map
re-oriented until they move enough to cross the floor -- at 20 m/px that is
about 320 m (2 x 8 px x 20 m/px, the floor of the day -- rung 4's floor is 6 px
since 2026-08-12), which on these rides is about
five packets (median 68 m between sent packets across all three logs) and
measured out at 3.1 packets per marker move.
For a device whose whole premise is track-up navigation *while moving*, this
reads as the right side of the trade: the case given up is a stationary
picture nobody is navigating by yet; the case bought back is not spending a
~2 s (rural) to ~9 s (city, per the table above) redraw on heading jitter from
a stopped GPS.

**Status: host-tested and host-replayed; not yet on hardware.** All 20
`test/map_follow` tests pass with the new gate, including
`HeadingDriftReAnchorsOnceMovingEnough`, which checks the other half -- a
genuine turn while moving still gets its redraw, just once `partialMoves`
crosses 2. The three-ride numbers in the table above are from the *old*
behaviour (replayed on the panel to establish the baseline this change
targets). The same three logs have since been replayed through the same
`MapFollow::decide()` compiled as a host binary -- see the next section for
what that says, and for the one thing it still cannot answer.

### Sweeping the thresholds off the device

`test/map_replay` walks a recorded ride's packet stream through the real
`MapFollow::decide()` on the laptop. No serial port, no device, no lock: it
links `MapFollow.cpp`, `MapProjection.cpp` and `MapViewport.cpp` into a host
binary (`test/map_replay/CMakeLists.txt`) and reimplements only the bit that
lives in `MapActivity` -- `applyFix()`'s state machine
(`MapActivity.cpp:1534-1602`) and `renderViewport()`'s three state effects
(`MapActivity.cpp:1850`, `MapActivity.cpp:2001-2004`).

```
cmake -S test -B build/test && cmake --build build/test --target map_replay

# what the constants do today, per ride
build/test/map_replay/map_replay ../../docs/rides/trailink-gps-2026080*.jsonl

# sweep the movement floor across every ride in one run
build/test/map_replay/map_replay --sweep-min-moves 0,1,2,3,4,6,8 <rides>

# the correctness gate below, re-runnable
build/test/map_replay/map_replay --check test/map_replay/hardware-baseline.txt <rides>
```

Three rides is about 30 ms, so a seven-value sweep across all three costs less
than one packet of the serial replay it replaces. `tools/replay_ride.py` in the
parent repo is still the ground truth and still the only thing that measures
the *panel*; this measures the decision.

**The gate: it reproduces the hardware run exactly.** Replayed with the
movement floor at 0 (the firmware the hardware numbers were measured on), all
three rides match the X4 on all five reported columns:

| ride | packets | redraws | heading | budget | keep-in |
|---|---|---|---|---|---|
| 142303 | 339 = 339 | 24 = 24 | 20 = 20 | 4 = 4 | 0 = 0 |
| 173058 | 389 = 389 | 23 = 23 | 19 = 19 | 4 = 4 | 0 = 0 |
| 201221 | 369 = 369 | 20 = 20 | 13 = 13 | 7 = 7 | 0 = 0 |

Exact, not close. That is the expected result -- same packets, same decision
code, no float in the path that the device does not also have -- but it is
worth having as a gate, because the two ways to get it wrong (projecting a fix
through a *fresh* projection instead of the frame's own, and resetting the
wrong state on a re-anchor) both produce numbers that still look plausible.
`hardware-baseline.txt` carries the measurement so the gate can be re-run
after any change to `decide()`, and `--check` exits non-zero on a diff.

**The sweep, `kMinPartialMovesForHeadingReAnchor` across all three rides**
(drift limit 4, budget 12, zoom step 4, 1097 packets total):

| floor | redraws | heading | budget | keep-in | thrash (<=1 moves in) |
|---|---|---|---|---|---|
| 0 | 67 | 52 | 15 | 0 | 29 |
| 1 | 48 | 30 | 17 | 1 | 12 |
| **2** | **43** | **25** | **18** | **0** | **0** |
| 3 | 43 | 26 | 17 | 0 | 0 |
| 4 | 39 | 22 | 17 | 0 | 0 |
| 6 | 37 | 20 | 17 | 0 | 0 |
| 8 | 33 | 14 | 19 | 0 | 0 |

**2 is the right number, and the data says why 3 is not.** The floor at 2 cuts
redraws 67 -> 43 (-36%) and takes the whole measured thrash pattern with it.
Going to 3 buys **nothing at all** -- the same 43 redraws, one of them simply
moved from the budget column to the heading column. Every further step costs
proportionally more delay on a genuine turn for less: 4 saves 4 more redraws
(-9%) for double the wait, 8 saves 10 more (-23%) for four times the wait. At
20 m/px a floor of 2 is ~320 m of riding; a floor of 8 is ~1.3 km, which is a
map left facing the wrong way for a whole leg. Nothing in this sweep argues for
moving off 2.

**Not all 24 saved redraws are free, and the sweep shows the leak.** Budget
redraws go 15 -> 18 as the floor goes 0 -> 2: three of the frames that no
longer get re-anchored by heading survive long enough to run out of ghosting
budget instead. That is the same substitution this doc's earlier round found
between the drift limit and the budget, measured directly this time instead of
inferred from two runs.

**The 2D sweep settles which of the two heading levers to pull** (`redraws
(heading + budget)`, all three rides):

| drift limit | floor 0 | floor 2 | floor 4 |
|---|---|---|---|
| 3 | 92 (79+13) | 56 (44+12) | 48 (36+12) |
| 4 | 67 (52+15) | **43 (25+18)** | 39 (22+17) |
| 5 | 51 (30+21) | 40 (20+20) | 35 (13+22) |
| 6 | 47 (26+21) | 39 (17+22) | 35 (14+21) |

Read down the `floor 0` column and the earlier round's finding reappears:
raising the drift limit trades heading redraws for budget redraws almost
one-for-one past 5 (30+21 at 5, 26+21 at 6 -- four redraws for another 22.5
degrees of staleness). Read across the `drift 4` row and the movement floor
does not do that: it removes 24 redraws and hands 3 back. **The floor is the
better lever**, and it is better for a reason the numbers only illustrate: it
costs orientation accuracy *only while the rider is not moving*, where a
raised drift limit costs it on every real turn. `drift 4 + floor 2` (43) also
beats `drift 6 + floor 0` (47) outright while keeping the map within 90
degrees of the direction of travel, so there is no case left for raising
`kMaxHeadingDriftSteps`.

Two more things the sweep answers cheaply:

- **The 5-decimal console replay costs nothing.** `tools/replay_ride.py:163`
  types `pos %.5f %.5f`, about 1.1 m of rounding, where a real BLE packet
  carries 1e7 fixed point (`MapCommandParser.h:47-50`). Replayed at both
  precisions the counts are identical, every ride, every column -- 1.1 m is
  0.05 px at 20 m/px. The hardware baseline is not distorted by how it was
  fed.
- **The gate holds on the other rungs, but the balance shifts.** Floor 0 -> 2
  at zoom 2 (6 m/px) is 101 -> 89 redraws and at zoom 3 (12 m/px) 79 -> 62,
  both wins, but at 6 m/px the budget is already 50 of the 89 -- the ghosting
  budget governs the close rungs, heading governs the far ones. Same split
  this doc's original 6 m/px measurement found.

**What this cannot answer, and still needs the device.** It counts decisions,
not milliseconds and not waveforms: nothing here says what a redraw costs (the
cost table above is hardware-measured and stays the only source), whether the
panel ghosts at any of these budgets (`kRouteFramePartialMoves`' comment: a
framebuffer dump of a ghosted panel looks perfect), or whether a map that holds
its orientation for 320 m of riding *reads* right to someone on a motorcycle.

**Flashed, 2026-08-08, and the count did not match the sweep.** Floor 2 on
real hardware, ride 142303 replayed the same way the correctness gate above
was measured (`tools/replay_ride.py`, console `pos`): **30 redraws, not 14**
-- 14 heading, 15 budget, 1 keep-in, where the sweep says 14 total (8+6+0).
The gate above still passes exactly at floor 0 on this same build, so the
harness is not simply wrong. Two things are ruled out: `MapActivity` wiring
(`applyFix()` calls the identical function for both the console and BLE
paths, `MapActivity.cpp:1143,1174`) and the console's own `pos`-unchanged
shortcut (`MapActivity.cpp:1164`, skips `applyFix()` entirely on a repeated
lat/lon -- present but not the cause, since it would have shown up as an
*undercount* against the floor-0 gate too, and that gate is exact). Leading
suspect, not confirmed: the device's persisted last fix before packet 1 --
two hardware runs of this same ride under the *old* firmware already gave
different totals (32, then 24) for exactly this reason, and neither hardware
session controlled for it. `test/map_replay` always starts clean from the
ride's own first packet; a real device does not. Settling this needs a
controlled re-run -- same known starting fix before every replay, not a new
one each session -- not another guess from here.

**One assumption inside the harness**, worth knowing before trusting a number
it produces: every marker move is taken to succeed. On the device a rejected
`displayBufferWindow()` falls back to a full refresh and zeroes `partialMoves_`
(`MapActivity.cpp:1512-1522`). That path logs `marker window rejected` and did
not appear in the hardware runs behind the gate above, which is why the counts
match exactly -- but a ride that trips it would replay optimistically here.

### Watching a ride instead of counting it

`map_replay --events` prints every packet's outcome (skip/move/reanchor, with
the fix's screen position and `partialMoves` at decision time) instead of only
the summary -- for reading a specific stretch, not for a table.

`map_replay --frames DIR` writes `<ride>.frames.csv` per ride: one row per
**ReAnchor** (packet, timestamp, lat/lon/heading, reason, moves-in) -- every
moment the device's picture actually changes. `tools/render_ride_video.py`
(parent repo) turns that into an MP4: one `map_preview` call per row renders
that exact view with the real `MapRenderer`, held on screen for the real gap
(clamped, `--min-hold-ms`/`--max-hold-ms`) until the next one. Encoded via
`imageio-ffmpeg` (`pip install imageio-ffmpeg`, a user-scoped binary, no
system package); falls back to an animated GIF automatically if that is not
installed. A `.manifest.txt` next to it maps `mm:ss` back to a packet number
and reason, so "at 1:24 I see an unnecessary render" is a lookup, not a
re-count.

In this mode only ReAnchor gets a frame -- MoveMarker slides a 64x64 patch
inside a frame that is otherwise pixel-identical ("The decision" above), and
the video showed it: the marker never appeared to move or turn between
redraws, which reads as a bug the first time you watch it and is not one --
it is `--frames`' scope cut being visible.

**`map_replay --track DIR` covers that gap**, one row per **packet** (skip
and move included, not just reanchor), each with the frame's own heading
alongside the fix's. `tools/render_ride_video.py --track` renders the real
picture only when a row is `reanchor` (or the ride's first packet, tagged
`init`), then draws a small red dot and direction line on a *copy* of that
held picture for every packet in between -- the real, continuous GPS fix,
independent of whether the device's own marker moved to show it or the frame
even redrew. Cheap: with 14 real redraws over 339 packets, only 14
`map_preview` calls happen, the other 325 frames are a PIL composite each.

The direction line's angle is `(fix_heading - anchor_heading)` steps off
"up", matching `MapFollow::relativeHeadingStep()` exactly -- "up" is the
frame's own anchor heading, not true north. **Getting this position right
took a second pass**: a `ReAnchor` event's `(x, y)` from `MapFollow::decide()`
is the fix projected through the *old* frame -- exactly the drift that
triggered the reset, meaningless in the new one. The first version of
`--track` used it anyway and the dot landed nowhere near the marker on every
reanchor with more than a couple of moves in, found by eye in the rendered
frames, not by a test. Fixed by recording the *post-reset* position for a
reanchor row instead -- always `(anchorX, markerY)` with relative drift 0,
which is what `renderViewport()` actually leaves on screen. Worth remembering
for the next field added here: `MapFollow::decide()`'s `(x, y)` is pre-reset
by definition, and anything drawn after a reset needs the reset's own
answer, not the question that caused it.

## The heading decides the frame, once

The map is drawn track-up by default: `renderViewport()` passes
`frameHeadingFor()`'s answer into `proj_.reset()`, so that heading is "up" on
screen for the whole life of the frame. Assumed track-up throughout the design
(the map workspace's `roadmap.md`, "Map rotation model") and confirmed with the user
2026-08-05, replacing the earlier forced-north `kNoRouteDisplayHeading`.

Two settings can override what `frameHeadingFor()` answers: `mapRotationMode`
(`MAP_ROTATION_NORTH_UP` pins it at 0, i.e. true north) and `mapHeadingMode`
(`MAP_HEADING_MANUAL` freezes it at whatever it was when the rider switched
Manual on, via `updateManualHeadingCapture()`). Both live in the Settings
screen (category Map) as the value the map opens with, and both are also
quick-toggle rows in CONFIRM's own menu (`MapActivity::openMapMenu()`) so a
rider can flip them mid-ride without leaving the map. Either value also flips
`Request::routeHoldsFrame` to true (`MapActivity::frameOrientationLocked()`,
`MapActivity.cpp`) for the same reason a loaded route does: the frame is not
tracking the fix's heading, so heading drift is not a reason to `ReAnchor`.
Default for both is the behaviour above, unchanged.

Two things follow from it:

- **The marker's arrow is drawn relative to the frame**, not to true north:
  `MapFollow::relativeHeadingStep(fix, anchor)`. On a fresh frame that is 0 --
  straight up, by construction. A rider who turns 45° after the frame was drawn
  gets an arrow 45° off up, which is exactly the turn they made. Beyond 90° the
  frame itself is redrawn (check 1).
- **The compass rotates.** `drawCompass(headingStep)` turns the whole glyph about
  its arc centre by the frame's heading, so it points at true north instead of up
  the screen. The "N" label's *position* rotates; the letter stays upright,
  because `GfxRenderer` has no arbitrary-angle rotated text (only
  `drawTextRotated90CW`) -- and an upright letter on a turning bezel is how a
  compass rose reads anyway. The glyph is anchored by its arc centre with a
  circular halo, not a bounding box, since it sweeps a disc.

This is also why the marker ladder means anything. With the map pinned north-up,
"look-ahead 95%" only put the road ahead on screen when the rider happened to be
heading north.

## What a marker move does not update

The debug readout at the top of the screen keeps the lat/lon/heading/seq of the
last **full** frame. Refreshing it would be a second window on the far side of
the panel -- its own waveform, for a debug line. `LOG_DBG` carries every fix's
real numbers instead (`marker move to ...`, `fix #N skipped: ...`).

## Console

`pos` goes through the same decision as a BLE fix, so the console (over USB or
BLE, `MapCommandConsole.h`) exercises this path for real -- as does the map
workspace's `mapcmd.py`, which drives it. A `pos` a metre away is skipped and says so in the log --
that is the behaviour under test, not a dropped command.

`zoom`, `marker`, `mode`, `heading` and `redraw` with the position unchanged do
**not** go through it. Each is an explicit instruction to change the picture, and
a `heading` command that only nudged the marker's arrow would read as a dead
console.
