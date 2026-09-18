# Telling the rider the device is working

The panel holds a stale picture for seconds while a frame is composed. Nothing
in that picture says so. A rider who taps and sees nothing cannot tell a slow
redraw from a dead key.

This doc holds what exists, what it does not cover, what it costs, and why the
obvious extension was refused.

## What exists: the map's busy badge

`MapActivity::showBusy()` (`src/activities/map/MapActivity.cpp:1010`) paints a
34 px hourglass above the button hints, bottom right, and refreshes that
rectangle only. The glyph is drawn by `drawBusyBadge()` (`:838`), the geometry
is the `kBusy*` block (`:224-231`), and `busyRect()` (`:829`) is the rectangle.

Built 2026-08-05 and confirmed on hardware then, after a redraw at close zoom
was measured at 6,452 ms and a slow redraw looked exactly like a dead button
(parent repo `docs/PROGRESS.md`, "Busy indicator on the map, asked for while
riding").

Three properties worth keeping:

- **It goes up before the slow work, not after.** The badge is stamped into the
  frame already on the panel, so it appears while the new frame is still being
  composed. A glyph stamped after composition would arrive when the wait is
  over.
- **One badge per burst.** `busyShown_` (`MapActivity.h:1300`) latches, so three
  quick zoom presses that coalesce into one redraw also pay one badge.
- **Taking it down is free.** The frame that erases it is the redraw the rider
  was waiting for. Nothing is spent to remove it.

## What it does not cover

**All 27 `showBusy()` call sites sit behind a press, a menu row or a gesture.**
A redraw the rider did not ask for carries no badge:

- `applyFix()` renders a whole viewport on a `ReAnchor` with no badge
  (`MapActivity.cpp:6427` and `:6495`). That is the follow path: the marker
  neared an edge or the heading turned, so the map redraws itself while the
  rider is only looking at it.
- **No activity outside the map has a badge at all.** Every other screen renders
  through the render task (`src/activities/ActivityManager.cpp:55-65`), which
  knows nothing about this. **Since T-2024 the map renders there too**, and the
  badge stayed: the frame is no faster, so the wait it answers is unchanged
  (`activity-manager.md`, "The map screen: how it joined the model").

## What it costs, and why it was not extended

**One windowed refresh, 500 ms on the X4 and X3, ~1,030 ms on the T5 S3 Pro**
(`refresh-modes.md`, "What it costs"). A windowed refresh costs the same as a
whole-panel one: the waveform is a fixed price and the area does not enter into
it. The take-down is free, so the price of a badge is one refresh per redraw,
not two.

Against it: a plain map redraw blocked the loop for **2.80 s**, and opening the
map for **4.34 s** -- measured before T-2024 moved the compose to the render
task. The panel still takes that long; `loop()` no longer waits for it (`input-gestures.md`, measured on an X4 Pro 2026-09-14).

So on a redraw the rider started, 500 ms buys feedback on a 2,800 ms wait, and
that trade was taken in 2026-08. On a redraw the device started, the same
500 ms would be paid on every fix that re-anchors the frame, forever, to warn
about a finger that usually is not there.

**Maintainer's decision, 2026-09-16: too high. An on-screen busy mark is not
the answer, and the badge stays only where it already is.** The question it was
meant to answer (T-2018, parent `docs/TODO.md`) stays open with no on-screen
form.

### What a cheaper answer would have to beat

- **The frontlight.** Hardware on the X4 Pro, reacts instantly, touches no
  refresh at all, so a brief dim at the start of a compose says "working" for
  free. Two catches: invisible in daylight, intrusive at night, and the plain
  X4 and the X3 do not have one. Never tried.
- **Composing the mark into the frame being pushed anyway.** Costs nothing and
  arrives when the render *ends*, which is when it is no longer needed.
- **Making the compose shorter**, so there is less silence to announce.

## The half that is already loud

The wait has two parts and only one of them is silent. Composing the frame --
tile reads and rendering, the bulk of those 2.8 s -- shows nothing. The panel
push that follows announces itself: a `HALF_REFRESH` visibly inverts the screen
on the way through (`refresh-modes.md`).

Any feedback worth adding therefore belongs to the compose, which is exactly the
stretch that happens before the panel is touched -- and that is why a free
signal has to come from something other than the panel.

## The second symptom: a gesture mistimed, not just misread

Verified on an X4 Pro 2026-09-15, while testing the tap-expiry rule: **double
taps made during a redraw often came out as single taps**, while the same double
tap on a settled map was flawless. The recogniser was cleared as the cause.
What differs is that during a redraw there is nothing to tap *against*, so the
two presses drift past the window.

Widening the window is not the fix: it is paid as latency on every tap on every
screen, and 500 ms was measured to fuse two deliberate single taps -- free
tapping on that key runs down to 240 ms intervals. The window is 300 ms
(`kHomeKeyDoubleTapWindowMs`, branch `t266-gt911-task`, not on `develop` as of
2026-09-16).

So this one is **unsolved**. It is the stronger of the two arguments for
feedback and it survives the decision above.
