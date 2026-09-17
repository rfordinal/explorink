# The map's CONFIRM menu: layout, and closing it without a redraw

The map screen's CONFIRM button opens one flat popup, in this order: Look
around / Follow mode, Zoom in, Zoom out, Whole route, Pins, Mode, Rotation,
Heading mode, Zoom, Reload map, Debug info (`MapActivity::openMapMenu()`,
`MapActivity.cpp:2432`). Look around and Whole route are conditional rows --
they only show once a fix has drawn a frame, or once a route is loaded. Zoom
in / Zoom out are conditional too: observation mode only, and each hides at its
end of the ladder (`docs/map-observation-mode.md`, "Zoom while observing"),
which is what makes the list eleven rows at most. This file covers three things
changed 2026-08-12: how the rows are laid out, how closing the menu got
cheap, and why the button hint says "Options".

Status: **flashed and looked at on the panel 2026-08-12 -- layout, boxed value
and the fast close all read correctly.** Numbers (backdrop size, refresh time)
are still unmeasured; those are called out per section.

## Rows: label left, value boxed on the right

Rows used to be one string, `"Mode: Ride"`, centred. They are two columns now:
`OptionPopup::showWithValues()` (`OptionPopup.h:63`) takes `options` and a
parallel `values`, and `MapActivity::openMapMenu()` fills both. An empty value
means the row is a plain action (Reload map, Whole route, Look around).

`BaseTheme::drawOptionPopup()` (`BaseTheme.cpp:963`) draws it:

- Labels start at one left edge when `leftAlign` is set
  (`BaseTheme.cpp:1057`), instead of each row being centred on its own width.
  A value column only reads as a column when the labels line up.
- The value sits flush right, inside a box that is filled black on the
  selected row, with the value drawn white in it (`BaseTheme.cpp:1067-1080`).
  This is the same "this is the changeable part" cue the Settings list gives
  (`LyraTheme::drawList()`, `LyraTheme.cpp:336-347`).
- Unselected rows show the value as plain text, no box -- also as in Settings.

Two width calculations exist and must agree: the drawing pass
(`BaseTheme.cpp:975-990`) and `OptionPopup::getLayout()` (`OptionPopup.h:187`),
which is what touch hit-testing uses. Both add `label + selectionHPadding +
value + 2 * BaseTheme::optionPopupValuePadding()` for a row with a value. The
padding constant is shared (`BaseTheme.h:262`) so the two cannot drift.

The old three-argument `drawOptionPopup()` call still works -- `values`
defaults to empty and `leftAlign` to false, so every other popup in the
firmware (confirmations, pickers) draws exactly as before.

## Closing the menu: one window refresh, no tile read

Closing the menu used to cost a full `renderCurrent()`: tiles off the card,
the whole frame recomposited, a whole-panel waveform. That is seconds (the
zoom-rung measurements in `docs/zoom-rungs.md` put a viewport reset at 3.6 s
on rung 6) for a dismiss that changed nothing.

The menu now saves the pixels it is about to cover:

- `MapActivity::captureMenuBackdrop()` (`MapActivity.cpp:1784`) runs after
  `show()` (the layout the rect comes from needs the rows) and before the
  popup's first draw (the framebuffer still holds the map). It takes
  `OptionPopup::frameRect()` (`OptionPopup.h:153`) -- the dialog plus its
  frame -- and copies that region out with
  `GfxRenderer::copyRegionToBuffer()`.
- `MapActivity::restoreMenuBackdrop()` (`MapActivity.cpp:1812`) writes it
  back, repaints the map's own four button hints over the popup's
  (`drawMapButtonHints()`, `MapActivity.cpp:1750`), and refreshes one window:
  full screen width, from the dialog's top edge to the bottom of the panel, so
  the dialog and the hint band go in the same refresh.

Used on the two closes that change nothing on the map: a Back dismiss
(`MapActivity.cpp:1557`), a tap outside the dialog on a touch panel
(`MapActivity.cpp:1569`), and the Zoom mode row, whose setting has no runtime
effect yet (`MapActivity.cpp:1933`). Every other row redraws for its own
reasons and drops the buffer first, so it is never held across a card read
(`MapActivity.cpp:1888`).

The panel state after a restore is exactly the pre-menu frame, marker
included, because the capture happened after that frame was composited. So
`markerPatchValid_`, `viewportDrawn_` and `busyShown_` still describe what is
on the glass and the restore deliberately touches none of them.

**Cost.** One heap buffer, allocated on open and freed on close. Its size is
the dialog's region, rounded out to byte boundaries by `getRegionByteSize()`.
That is why the dialog is now bounded (next section): every row it shows is
RAM, for as long as the menu is up.

Measured on the panel 2026-08-12: the map screen sits at **54,040 bytes free**
(`mapcmd.py info`, rung 6, twelve tiles held). Computed from the metrics, the
capped six-row dialog needs **~9 KB**; the unbounded eight-row one needed
~20 KB. `captureMenuBackdrop()` logs the real byte count and the free heap on
every open (`MapActivity.cpp`, `menu backdrop %u bytes`) -- read it off the
serial log rather than trusting the estimate.

Two guards, and neither can strand the popup's pixels on the panel:

- The capture is skipped when it would leave less than
  `kMenuBackdropHeapReserve` (24 KB) free (`MapActivity.h`). Everything that
  runs while the menu is up -- BLE tile transfers, the console, a settings
  write -- draws from the same pool, and a convenience must not starve the
  work.
- On OOM the capture logs and returns false.

Either way every close path falls back to `renderCurrent()` -- the behaviour
before this existed.

Precedent for the technique in the same file: the marker patch
(`saveMarkerPatch()`), which saves and restores the box under the position
marker so a fix can move it without redrawing the map.


### A whole-panel window refresh can abort the device

**Measured, the hard way, 2026-08-17.** The close path used to widen its refresh
to the whole panel when the popup had drawn side-hint boxes (they sit outside the
dialog, so the backdrop does not cover them). Opening the Pins list then killed the
device:

```
MapActivity::restoreMenuBackdrop() -> GfxRenderer::displayBufferWindow()
  -> HalDisplay::displayWindow -> Ssd1677Driver::displayWindow
  -> std::vector<uint8_t>::_M_create_storage -> operator new
  -> bad_alloc -> __terminate -> abort()
```

`Ssd1677Driver::displayWindow()` allocates a `std::vector<uint8_t>` of
`(w / 8) * h` bytes
(`freeink-sdk/libs/display/FreeInkDisplay/src/driver/Ssd1677Driver.cpp:440-442`)
-- 48,000 bytes for a full panel -- and **a second one the same size** when it is
handed a previous frame (`:454`). So a whole-panel window asks for 48 KB, or
96 KB differentially, against the 38,292 bytes free and the 34,804-byte largest
block in the crash report. The first allocation fails, and with `-fno-exceptions`
a failed `operator new` aborts rather than returning null (`CLAUDE.md`, Resource
Protocol 9).

So: **never refresh the whole panel to fix up something small.** The close now
refreshes the dialog's own window and, only when it has to, a second small window
over the side-hint strip. That strip's geometry comes from
`BaseTheme::sideButtonHintsRect()` -- added for this, because the alternative is
copying the theme's private constants and letting them drift.

The same ceiling applies to anything else that reaches for a big window: two small
refreshes are cheap, one big one can be fatal.

### A row callback must not open a popup

`OptionPopup::handleInput()` invokes the row callback, so a `show()` from inside
that callback reassigns the very `std::function` that is executing -- it destroys
the running callable under its own call. The Pins rows record what they want
(`MapActivity::PinPopup`) and `loop()` opens it one iteration later.

This is also where the double render went: the opener no longer paints, and
`handleInput()`'s own `requestUpdate()` does it once.
## The dialog has a ceiling, and the list scrolls through it

`BaseTheme::optionPopupGeometry()` decides how many rows are on screen at once
and caps it two ways: `kOptionPopupMaxVisibleRows` (6) and
`kOptionPopupMaxHeightPercent` (50% of panel height). Rows past the window
scroll; the dialog itself never grows or moves. Both caps are for `Auto`
dialogs only -- a fixed box budgets rows against its own height instead (see
"Two fixed boxes, defined per board"). The title line carries an
`n/m` counter whenever the list does not fit, so a scrolled list does not read
as a short list that lost rows.

`OptionPopup` owns the window position (`scrollTop`) and drags it with the
selection, wrapping at both ends like the selection always did. It opens
scrolled to the current value, which matters for a picker whose current value
is row nine.

Both passes now read one geometry function -- the drawing pass and
`OptionPopup::getLayout()`, which builds the touch rects. There is no second
copy of the layout maths to drift.

Alongside that, settings-style rows use compact spacing
(`BaseTheme::optionPopupSpacing()`, `compact = true`): half the vertical air,
and no width slack, because their width is computed exactly (label + gap +
boxed value) instead of being measured and padded. Horizontal padding is
untouched -- it costs one column, not one per row.

Numbers for the map menu, computed from the Lyra metrics (not measured): row
height 48 -> 36, dialog height 529 -> ~306 px, width ~289 -> ~233 px. The
eight rows still exist; six are on screen. The Pins row and observation mode's
two zoom rows (both 2026-08-17) make it eleven at most, still six on screen --
the window scrolls, the dialog does not grow.

Both caps apply to every popup in the firmware, not just the map's. Nothing
else builds a list long enough to hit them today, except the font-family
picker, which now scrolls instead of drawing a dialog the height of the panel.

## The title wraps now -- it used to run off both edges

The dialog box has always been capped to the panel width (`optionPopupGeometry()`'s
`dialogW`, clamped by `metrics.optionPopupDialogSideMargin`). The **title text**
was not: `drawOptionPopup()` drew it with `renderer.drawCenteredText()`, centred
on the full panel width, with no wrap and no truncation. Every title in the
firmware used to be short enough for that to never show -- until a dynamic one
was not. Reported on the S8 2026-08-24: `MapActivity::confirmPinReplaceSlot()`'s
title (`Replace <label> here?`, plus an age suffix on a stale fix) ran off both
sides of the dialog, and of the screen, instead of wrapping inside it.

Fixed generally, not for pins specifically: `wrapOptionPopupTitle()`
(`BaseTheme.cpp`, anonymous namespace) is a plain greedy space-split, sized for
a one-sentence dynamic title rather than the paragraph wrapping
`DictionaryDefinitionActivity::wrapText()` does for the reader. It wraps to
`pageWidth - optionPopupDialogSideMargin * 2 - innerPadding * 2` -- the widest a
dialog is ever allowed to be, not whatever this particular dialog's content
happens to need, so the wrap width cannot depend on `dialogW` (which itself
depends on the title once wrapped) and the two never have to be computed in a
particular order relative to each other.

Both `optionPopupGeometry()` and `drawOptionPopup()` call it with that same
fixed budget and get the same lines back -- `geometry.titleLineCount` records
how many for sizing (`chromeHeight`, `dialogH`), and `drawOptionPopup()`
re-wraps to get the actual strings to draw. Every other title in the firmware
is one word or a short static phrase, well under the budget, so this changes
nothing for them: `titleLineCount` stays 1 and the loop draws once, same as
the old single `drawCenteredText()` call.

## Two fixed boxes, defined per board

**Every popup the map opens is one of two boxes, and each box is the same rect
every time.** `BaseTheme::OptionPopupSize` (`src/components/themes/BaseTheme.h`)
names them:

- `Menu` -- the browsing box. The CONFIRM menu, the pin lists, the Nearby
  screens.
- `Confirm` -- the yes/no box, smaller on purpose, so a confirmation reads as a
  different kind of dialog and not as the next list.
- `Auto` -- measure the content, the historical behaviour. Every screen outside
  the map still uses it.

The sizes are theme metrics, as a percent of the panel the HAL reports:
`optionPopupMenuWidthPercent` / `optionPopupMenuHeightPercent` and the
`Confirm` pair (`ThemeMetrics`, same file). Percent, not pixels: the number is
per board without being written per board, so a wider panel gets a box of the
same proportion rather than an X4 pixel count. `optionPopupFixedBox()` resolves
one, centres it, and clamps the width to the panel's side margins.

Today: `Menu` 70% x 40%, `Confirm` 60% x 30%. On a 480x800 panel that is
336x320 and 288x240; on a 540x960 T5 S3 Pro, 378x384 and 324x288.

The first cut was 92% wide and it was wrong on the panel: 494 px of box for
rows whose longest label and value together need barely 300 (screenshot, T5 S3
Pro, 2026-09-06). A fixed box does not get to be as wide as a dialog may be --
it has to be as wide as the content usually is, because it no longer shrinks.

**Why a fixed box and not a floor.** The mechanism before this was
`setSizeHint()`: "be no smaller than the dialog you replaced". It was not
enough. A floor does not stop a popup with wider content growing past it, does
not stop a popup with more rows growing taller, and a dialog is centred -- so
any change of height moves the top edge. A moved edge is a different refresh
window and a dead backdrop, which is a full re-render (tiles off the card, a
whole-panel waveform, seconds). The fixed box removes all three: menu, list and
confirmation share one rect, `captureMenuBackdrop()` is taken once at the
`Menu` box, and `Confirm` sits inside it because it is smaller on both axes and
centred the same way. `dropBackdropIfPopupOutgrew()` stays as the guard, and
with these metrics it never fires.

**The close refreshes three small windows, not one band.**
`restoreMenuBackdrop()` used to refresh the full width of the panel from the
dialog's top edge down to the bottom, so that one window covered both the
dialog and the button hints under it. That is `ceil(W/8) * (ScreenH + DialogH)/2`
bytes, and the driver wants it as one block:

| | X4 480x800 | T5 S3 Pro 540x960 |
|---|---|---|
| old full-width band | 33,600 B | 45,696 B |
| dialog window now | 42 x 324 = 13,608 B | 48 x 388 = 18,624 B |
| hint band now | 60 x 40 = 2,400 B | 68 x 40 = 2,720 B |

`windowRefreshAffordable()` refuses anything that does not leave 4 kB under the
largest free block, which on a map screen was measured at 43 to 45 kB on the X4.
The T5 S3 Pro band sat right on that line, and a refused window means a full
re-render -- the exact redraw the backdrop exists to avoid. The popup only ever
dirties three places (its frame, the hint band, the side-hint strip a pin list
takes), so refreshing those three costs a third of the band and nothing between
them was touched anyway.

**A chained popup is one dialog, so the backdrop is not spent on the way in.**
Picking `Pins` in the menu closes the menu popup and queues the pin list
(`PinPopup`, opened from `loop()` -- a row callback may not `show()` from inside
`handleInput()`). The close path treated that as a dismissal: it restored the
map, which flashed the map onto the panel for one frame before the pin list
drew, and `restoreMenuBackdrop()` drops the backdrop it uses -- so the pin list
had none, and closing *it* re-rendered the viewport from the card.

Measured on the T5 S3 Pro 2026-09-07, serial log: `menu backdrop 19100 bytes
(382x388), free heap 119388` at menu open, then `render 2331 ms` with the busy
badge on the Back out of the pin list. Nothing failed -- no rejected window, no
refused capture. The backdrop had been spent one popup too early.

So the close does nothing at all while `pendingPinPopup_` or
`pendingNearbyPopup_` names a follow-up: the popup's pixels stay up until the
next popup draws over the same rect, and the backdrop waits for whoever closes
the chain. A chained handler that draws a real frame instead of a popup
(`showPinOnMap()`, `savePin()`) drops the backdrop itself.

This one is not new. It predates the fixed boxes -- with `setSizeHint()` the
pin list matched the menu's size, so the same double-restore happened and the
same full re-render followed. What the fixed box changed is that the flicker
became obvious: the two boxes are now identical, so the map blinking between
them has nothing to hide behind.

**What still adapts.** The row count. The title may wrap to two or three lines
and eat a row, so `optionPopupGeometry()` budgets the rows against the box's own
height instead of `kOptionPopupMaxHeightPercent`, and drops
`kOptionPopupMaxVisibleRows`: the cap exists to stop a dialog growing with its
list, and a box that cannot grow needs no second brake. Rows past what the box
holds scroll, same as before. Space left under the last row stays empty -- that
emptiness is what keeps the rect constant.

**The title wraps to the box, in both passes.** `optionPopupGeometry()` and
`drawOptionPopup()` each compute the same budget from the box's width, and each
reserves the `n/m` counter corner unconditionally in fixed mode. Reserving
always costs a slightly narrower title on a list that turns out to fit;
guessing costs a counter drawn over the title's last word, because whether the
list scrolls is not known until after the title is wrapped, and the draw pass
must reach the identical number without knowing it either.

That last part is the lesson the size hint paid for on the S8, 2026-08-24: a
title wrapped to the full-screen budget wins `maxTextWidth` on its own and
drags the dialog wider than the box it was told to match. The wrap budget has
to be the box's, or the box is not a box.

**Verified on the T5 S3 Pro, 2026-09-07.** Menu, pin list and confirmation all
open at their box, the map does not blink between them, and Back out of the pin
list puts the map back with no busy badge and no wait -- maintainer, on the
panel: "vsetko funguje dokonalo". The `Menu` box at 70% x 40% (378x384 there)
reads right; 92% did not, and the first cut's screenshot measured 494x382
against 496x384 computed, which is how the box maths was confirmed.

One claim here is arithmetic, not measurement: the byte table above. The
full-width band was never observed being refused -- the log showed the capture
succeeding with 119 kB free and `MaxAlloc` at 77,812. The band split is
therefore a cost reduction that stands on its own, not the fix for the
re-render; that was the backdrop being spent one popup early (above). Whether
the old band would have been refused on a busier heap is open and no longer
worth chasing.

Still unmeasured: the close's actual wall-clock time against the full redraw it
replaces, and all of this on an X4 or X4 Pro -- the numbers in the table for a
480x800 panel are computed, and no such device has run this code.

## The hint says "Options", not "Select"

CONFIRM on the map screen opens a menu; it does not pick anything. The hint
now reads `STR_MAP_OPTIONS` ("Options", `english.yaml`), on all five frames
that draw the map's hint row (`drawMapButtonHints()` plus the waiting, loading
and overview frames). "Select" stays inside the popup, where a row really is
picked (`OptionPopup::processRender()`, `OptionPopup.h:136`), and in
`RouteSelectActivity`, which really selects a route.

Only `english.yaml` carries the new key; every other language falls back to
English until translated (`docs/i18n.md`).

## Open

Verified by eye on the panel 2026-08-12: the left-aligned rows, the black
value box on the light-grey selected row, and a menu close that puts the map
straight back. That was the eight-row dialog, before the cap.

The capped, compact, scrolling dialog was flashed and looked at the same day
and reads correctly on the panel.

Not verified:

- The backdrop's real byte size (the log line now prints it) and how close the
  24 KB reserve comes to refusing a capture in practice. Reading it needs a
  serial monitor, which resets the device on open, so it has not been read
  yet.
- The window refresh time for a menu close, against the full redraw it
  replaces.
- Row labels are still the Settings-screen strings ("Rotation", "Heading
  mode"). Shorter ones would narrow the dialog further; they are shared keys,
  so shortening them changes the Settings screen too. Heading mode's value
  string is not shared, though: `STR_MAP_HEADING_FROZEN` ("Frozen") is its own
  key, split off from the generic `STR_MANUAL` so renaming it does not also
  rename Zoom's "Manual" value (`english.yaml`, `SettingsList.h:264`).
