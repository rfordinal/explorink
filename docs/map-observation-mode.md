# Observation mode: panning without losing the fix

The map screen's CONFIRM menu can switch the four direction buttons from the
zoom/marker ladders to a pan. Feature added and verified on hardware
2026-08-08. This file documents the state model, the pan math, and a button
race found (and fixed) while testing it that affects every row in the menu,
not just this one.

## What it is

`MapActivity::MapScreenMode` (`MapActivity.h:334`) is `Follow` or `Observe`.
Follow is the normal ride/hike/cycle screen (`MapActivity.h:52-58`'s button
table). Observe re-reads the same four buttons as a direction pad
(`MapActivity::handleButtons()`, `MapActivity.cpp:1264`): each press pans the
viewport half a screen the way it points, via `panBy()`
(`MapActivity.cpp:1394`). Button hints switch to `<`/`>` (front) and `^`/`v`
(side) -- plain literals, not `tr()`, same reasoning as the zoom ladder's
`+`/`--` (`MapActivity.cpp`, `drawZoomSideHints()`'s comment): a direction pad
reads as arrows, not words.

`MapScreenMode` is independent of `MapRideMode` (ride/hike/cycle,
`MapRideMode.h`): one picks what the frame is *for*, the other picks what the
buttons *do*. A rider can look around in any ride mode.

Entered and left from the CONFIRM menu (`MapActivity::openMapMenu()`,
`MapActivity.cpp:1314`) via a row that toggles, `toggleObserveMode()`
(`MapActivity.cpp:1362`). The row is hidden until a fix has actually drawn a
frame (`hasReceivedAny_`) -- same "no row that cannot do anything" rule the
Whole-route row already uses.

## The pan itself: a half-screen step through the current projection

`panBy()` does not track a separate "where am I looking" coordinate. It reads
the projection the frame *on screen* was drawn with (`proj_`, whether that
frame came from a real fix or the previous pan step), inverse-projects the
screen point half a screen away from the anchor
(`MapProjection::screenToMerc()`), converts back to lat/lon
(`MapProjection::mercToLonLat()`), and calls `renderViewport()` with that as
the new anchor -- the exact same function a ladder step or a real fix uses. A
pan is, mechanically, a viewport reset around a synthetic coordinate instead
of a GPS one.

**Not coalesced** the way `stepZoom()`/`stepMarker()` are (`armRedraw()`'s
settle timer). A pan step's target depends on the frame the *previous* step
drew, which does not exist until that render actually runs -- and `loop()`
cannot poll another button press until the blocking render returns anyway
(single-threaded, no async input polling on this path -- see the race below).
Coalescing would either collapse a burst onto the same target or need its own
accumulator, for no real benefit.

## Two coordinates, not one

`lastLatE7_`/`lastLonE7_`/`lastHeading_` (`MapActivity.h`) are "the fix a
ladder step re-renders around" -- and `panBy()` repoints them at the pan
target on every step, same contract, because `renderViewport()` always writes
its own arguments there.

That is wrong for one question: where is the rider *actually*, so "Follow
mode" can render around them instead of wherever panning left off. That
answer lives in its own fields, `observeReturnLatE7_` /
`observeReturnLonE7_` / `observeReturnHeading_` / `observeReturnSeq_`
(`MapActivity.h:381` area) -- set once on entry from whatever
`lastLatE7_` etc. held at that moment, and kept current by `applyFix()`
while Observe is active (`MapActivity.cpp:1673`), which records an
incoming fix there instead of redrawing -- same idea as `overviewShown_`'s
early return, just with its own return coordinate. `toggleObserveMode()`
renders around `observeReturnLatE7_` etc. when leaving Observe.

The position marker is not drawn while observing
(`renderViewport()`'s `if (screenMode_ != MapScreenMode::Observe)` guard
around `drawPositionMarker()`): the anchor is a pan target the rider chose to
look at, not a GPS fix, and a marker glyph on it would claim otherwise.

## Finding: CONFIRM's press/release race reopened the menu after every Select

**Verified on hardware, 2026-08-08.** Picking any row from the map menu
(Refresh, Mode, Observation mode, Whole route) closed the popup, ran the row's
action -- and then the menu reappeared, as if the row had done nothing. First
seen testing Observation mode, but the mechanism has nothing to do with that
row specifically; it is the menu's own button wiring, and would have hit
Refresh and Whole-route identically.

**Mechanism:**

- `OptionPopup` selects a row on CONFIRM's *press* edge
  (`components/OptionPopup.h:97-101`).
- `MapActivity::handleButtons()` opens the menu on CONFIRM's *release* edge
  (`MapActivity.cpp:1300`, pre-fix).
- Button state only updates once per outer Arduino `loop()`, in
  `gpio.update()` (`main.cpp:520`) -- called *before* the current activity's
  own `loop()` runs, not during it.
- Every row's action renders the map (`renderViewport()`), which blocks for
  the better part of two seconds (`MapActivity.h:59-63`'s badge comment).
  Nothing polls the GPIOs during that block.
- A human's press-release cycle is far shorter than that render. By the time
  it returns and the *next* outer `loop()` calls `gpio.update()`, the
  physical button reads as released while the last polled state was
  "pressed" -- a fresh `wasReleased(Confirm)` edge, fired for the first time
  on this next poll, no matter how long the render took.
- The popup is already closed by then (the Select set `active = false`), so
  `MapActivity::loop()`'s popup-input branch is skipped and `handleButtons()`
  runs -- sees that release, and calls `openMapMenu()` again.

**Fix:** the exact pattern already used for Back's identical problem
(`suppressBackRelease_`, same file). `suppressConfirmRelease_`
(`MapActivity.h:541`) is set in `loop()` when the popup was active and
consumed a CONFIRM *press* (`MapActivity.cpp:1140`), and cleared the one time
`handleButtons()`'s CONFIRM-release check would otherwise reopen the menu
(`MapActivity.cpp:1306-1308`).

Unlike Back's case, no extra redraw is needed alongside the suppress: Back's
press-driven close has no action of its own, so nothing else repaints the map
underneath it. Every Select branch already renders as part of doing its job.

Not a new problem class: `EpubReaderActivity` already has the identical guard
under a different name, `ignoreNextConfirmRelease`
(`EpubReaderActivity.h:48`, set at `EpubReaderActivity.cpp:532,542,550` after
a bookmark add or KOReader sync launch that renders synchronously off a
Confirm press). `MapActivity`'s menu just did not have its own copy of that
guard. **Open:** whether any *other* `OptionPopup`/list-select consumer with
a release-edge open and a synchronous-render Select is still missing this --
worth grepping for (`wasReleased(...Confirm)` next to an `OptionPopup` in the
same file) rather than assuming `EpubReaderActivity` and `MapActivity` are
the only two.
