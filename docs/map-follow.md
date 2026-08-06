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

1. **Heading drift ≥ 4 steps (90°)** → `ReAnchor`. The map is track-up, so the
   frame is only correct for the heading it was drawn with.
2. **Marker within 80 px of a screen edge** → `ReAnchor`. `kKeepInMarginPx`, one
   marker ring plus slack: inside this frame the marker's 64x64 box never
   straddles the panel edge, and there is still map ahead to look at.
3. **12 moves since the last full frame** → `ReAnchor`. Windowed refreshes are
   differential and ghost. `kMaxPartialMoves`, the starting point named in
   the map workspace's
   `firmware-implementation-plan.md`, open decision 4 ("every 10-20
   marker updates, needs on-device tuning"). **Unverified** -- the number that
   fits is whatever real ghosting turns out to allow.
4. **Movement under 8 px on both axes** → `Skip`. `kMinMovePx`. Below this a
   waveform buys a marker that visibly did not move.

Every redraw reason is checked before the movement floor. A rider standing still
who has turned 90°, or one crawling along with a spent ghosting budget, still
gets the redraw -- the floor must not swallow it. `MapFollowDecide`'s
`HeadingDriftReAnchorsEvenStandingStill` and
`GhostingBudgetBeatsTheMovementFloor` pin that.

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
would need 3.7 km in one direction to reach the 80 px frame, and the budget
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

## The heading decides the frame, once

The map is drawn track-up: `renderViewport()` passes the fix's heading straight
into `proj_.reset()`, so that heading is "up" on screen for the whole life of the
frame. Assumed track-up throughout the design
(the map workspace's `roadmap.md`, "Map rotation model") and confirmed with the user
2026-08-05, replacing the earlier forced-north `kNoRouteDisplayHeading`.

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
