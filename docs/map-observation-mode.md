# Observation mode: panning without losing the fix

The map screen's CONFIRM menu can switch the four direction buttons from the
zoom/marker ladders to a pan. Feature added and verified on hardware
2026-08-08. This file documents the state model, the pan math, a button race
found (and fixed) while testing it that affects every row in the menu, not
just this one, and two findings from getting the button hints' arrow glyphs
right, also verified on hardware the same day.

## What it is

`MapActivity::MapScreenMode` (`MapActivity.h:334`) is `Follow` or `Observe`.
Follow is the normal ride/hike/cycle screen (`MapActivity.h:52-58`'s button
table). Observe re-reads the same four buttons as a direction pad
(`MapActivity::handleButtons()`, `MapActivity.cpp:1264`): each press pans the
viewport 30 % of a screen the way it points, via `panBy()`
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

## The pan itself: a 30 % step through the current projection

`panBy()` does not track a separate "where am I looking" coordinate. It reads
the projection the frame *on screen* was drawn with (`proj_`, whether that
frame came from a real fix or the previous pan step), inverse-projects the
screen point `MapActivity::kPanStepPercent` of a screen away from the anchor
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

The same "no fix reaches the screen while observing" fact is also why
`MapActivity::loop()` stops BLE advertising (and drops any connection) while
Observe is active and no transfer is moving bytes -- `ble-advertising.md`,
"Observe mode: no radio when there is nothing to send or receive".

The live position marker is not drawn on the pan anchor while observing
(`renderViewport()`'s `if (screenMode_ != MapScreenMode::Observe)` guard
around `drawPositionMarker()`): the anchor is a pan target the rider chose to
look at, not a GPS fix, and a marker glyph on it would claim otherwise.

**Changed 2026-08-21, verified on hardware (maintainer, same day):** the
rider's real last fix (`observeReturnLatE7_`/`Lon_`) is now separately
projected into whatever the rider panned to and drawn there in the
sleep-marker style -- `MapActivity::drawObserveFixMarker()`, called from the
`else` branch of the same guard (`MapActivity.cpp`, `renderViewport()`).
Before this, Observe drew no marker at all, which read as "the fix was lost"
rather than "you are looking somewhere else". Same shape and same reasoning
as `drawSleepMarker()`'s ring-and-dot (`MapMarkerMetrics.h`'s
`kSleepMarker*`): no heading, because a stale heading dressed as current
would be exactly the kind of claim this mode already avoids for the anchor.
Skips silently, same as today, when the fix projects off the current
viewport -- no off-screen arrow, that would be a separate feature.

## Zoom while observing: a hold, plus two menu rows

Added and **flashed and verified on the panel 2026-08-17.** Serial log
of that pass: four holds produced `observe: hold zoom in` / `... zoom out`
followed by the matching `zoom step` line, with no `pan: half-screen step`
between them -- the release ending a hold did not pan. Both menu rows stepped
the ladder too (`zoom step` with no `observe:` line before it). 600 ms felt
right to the maintainer in that sitting: one person, one session, no misfires
seen. Not a measurement of the threshold -- what would settle it is a gloved
hand on a moving bike, which is where an accidental zoom would show up.

Observe took all four direction buttons for the pan, which left the zoom
ladder unreachable while looking around -- and the map screen has no spare
button (`MapActivity.h`, "There is no spare button"). The ladder comes back on
a **hold of the same two buttons that carry it in Follow**: Up held past
`kObserveZoomHoldMs` (600 ms, `MapActivity.cpp:72`) steps one rung in, Down one
rung out (`handleButtons()`, `MapActivity.cpp:1953` onward).

Three consequences, all deliberate:

- **A pan fires on button release in Observe, not on press.** A press cannot
  be told from the start of a hold until it ends. All four buttons moved to
  release together -- half a direction pad answering on press and the other
  half on release is a difference the hand feels.
- **One rung per hold, no repeat.** The step arms a redraw that blocks
  `loop()` for the better part of two seconds, so a repeat rate would be
  fiction, and the ladder is seven rungs wide
  (`MapViewport::kZoomStepCount`, 1..45 m/px).
- **The release that ends a hold-zoom does not also pan.**
  `observeHoldZoomed_` (`MapActivity.h:496`) latches that, cleared by the
  Up/Down release it belongs to, and defensively cleared when neither button
  is down and no release is pending (`MapActivity.cpp:1987`, `:1995`).

The rung change re-anchors on `lastLatE7_`/`lastLonE7_` through
`renderCurrent()` -- which `panBy()` has been repointing at the pan target all
along (see "Two coordinates, not one" above). So zooming keeps what the rider
panned to and does not snap back to the fix. Same anchor point the pan itself
measures from (`MapViewport::kAnchorScreenX`, `markerYForStep()`), so pan and
zoom agree about which pixel stays still.

A hold is invisible on a still panel. So the CONFIRM menu carries **Zoom in**
and **Zoom out** rows while Observe is active (`openMapMenu()`,
`MapActivity.cpp:2159` area, `STR_MAP_ZOOM_IN` / `STR_MAP_ZOOM_OUT`) -- that is
where a rider who never guesses the hold finds the ladder. Each row is hidden
at its end of the ladder rather than shown doing nothing, the same "no row that
cannot do anything" rule the Whole-route and Observation-mode rows follow. The
rows render immediately instead of waiting out `stepZoom()`'s settle timer: a
menu row cannot be pressed in a burst, and the popup's pixels need a frame
anyway.

Follow mode gets no such rows. There the two side buttons *are* the ladder.

**Open, needs hardware:** whether 600 ms is right. Too short and a deliberate
pan zooms by accident; too long and the hold feels dead. Nothing about
pan-on-release has been felt on the device either -- that is the other thing
the first ride will answer.

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

## Finding: no builtin font has arrow glyphs except OpenDyslexic, and only its Bold cut is thick enough

**Verified on hardware, 2026-08-08.** `<`/`>`/`^`/`v` (the first cut of the
Observe-mode hints) read fine but looked like an afterthought; real Unicode
arrows (U+2190-U+2193) were the ask.

`fontconvert.py`'s default codepoint list already includes the Arrows block
(`(0x2190, 0x21FF)`, `lib/EpdFont/scripts/fontconvert.py`'s `intervals`), but
that only matters if a source face actually has the glyphs -- the tool skips
anything `face.get_char_index(cp)` returns 0 for
(`fontconvert.py:403-405`). Checked every builtin source face
(`lib/EpdFont/builtinFonts/source/*/`) with `fontTools`: Ubuntu, NotoSans,
NotoSansHebrew, NotoSansArabic, Ubuntu-Vietnamese and NotoSerif all answer
`False` for U+2190-U+2193. **OpenDyslexic is the only one that has them**,
already in the repo for the reader's accessibility font option
(`lib/EpdFont/builtinFonts/source/OpenDyslexic/`).

Regenerated `ubuntu_10_regular.h`/`ubuntu_10_bold.h` with
`OpenDyslexic-Bold.otf` appended to the fontstack (lowest priority, same
"borrow a glyph from a fallback face" pattern the Hebrew/Arabic/Vietnamese
supplements already use) and `--additional-intervals 0x2190,0x2193` --
`lib/EpdFont/scripts/convert-builtin-fonts.sh` doesn't have this font pinned
to a variable, so it was a one-off invocation, not a change to that script.
`OpenDyslexic-Regular`'s cut of the same four glyphs exists too but reads
thin at 10 px; `-Bold`'s is visibly heavier side by side
(`PIL`-rendered comparison, not just read-off-the-code) and was used
instead, on both the regular and bold variants of `ubuntu_10` -- the glyph
data for U+2190-U+2193 is identical in both, since neither Ubuntu face
supplies it.

`notosans_8_regular.h` got the same treatment in an earlier pass, then the
side hints moved to `UI_10_FONT_ID` anyway (see below) -- reverted rather
than shipped unused.

## Finding: default arguments do not virtualize, so per-theme font choice can't be a default value

**Verified on hardware, 2026-08-08, the hard way.** Both `drawButtonHints()`
and `drawSideButtonHints()` (`BaseTheme.h`) needed a per-caller font
override, so MapActivity's Observe-mode hints could ask for a bigger font
than whatever a screen's theme normally draws hints in. The obvious approach
-- give the parameter a default value, a different one in each theme's own
override -- shipped, built clean, and then grew the button-hint text on
every other screen in the firmware, not just the map.

Cause: `GUI` (`components/UITheme.h:50`) is
`UITheme::getInstance().getTheme()`, typed `const BaseTheme&`
(`UITheme.h:22`) -- a fixed static type, regardless of which concrete theme
(`LyraTheme`, `RoundedRaffTheme`, `BaseTheme` itself) is actually selected at
runtime. A default argument is resolved against the **static type at the
call site**, not the override that ends up running. Every existing
`GUI.drawButtonHints(...)` call in the codebase omits the new parameter, so
every one of them got `BaseTheme`'s declared default (`UI_10_FONT_ID`) baked
in at compile time -- then virtual dispatch correctly ran `LyraTheme`'s (or
`RoundedRaffTheme`'s) actual body, with that wrong value, silently replacing
the `SMALL_FONT_ID` those bodies used to hardcode. `LyraTheme`'s and
`RoundedRaffTheme`'s own declared defaults were never reachable through
`GUI` at all -- dead code from the moment they were written.

Fix: a sentinel, not a default. `fontId = 0` at every layer (0 is already
reserved as fontIds.h's "not found" value, `fontIds.h:15`, never a real
font ID); each override checks `if (fontId == 0) fontId = <its own font>;`
in the function body, where virtual dispatch has already picked the right
one. Same fix applied to `btn3FontId`/`btn4FontId`, the follow-up parameters
that let Exit/Select stay at each theme's normal size while only the arrow
slots (btn3/btn4) pick up the bigger font -- a single shared `fontId` for
all four positions was the first attempt, and grew Exit/Select too.

**General, not map-specific**: any virtual method on `BaseTheme` (or any
base class dispatched through a base-typed reference) that wants a
per-override default value has this exposure. A default argument can only
safely vary by override if every override's caller reaches it through that
override's own static type -- never true for anything called via `GUI`.
