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
scroll; the dialog itself never grows or moves. The title line carries an
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

### A hinted dialog needs the exact inverse formula, not an approximation

The fixed budget above is right for a dialog with no size hint. A dialog
opened with `setSizeHint()` (`OptionPopupSpec::minDialogWidth` -- "match the
list this replaces") needs the title to wrap to *that* width instead, or the
title decides `maxTextWidth` on its own and the hint never gets a chance to
bind. The first cut of this clamp was `minDialogWidth - innerPadding * 2`,
which is not the inverse of `dialogW`'s actual formula:

```
dialogW = (maxTextWidth + innerPadding*2 + selectionHPadding*2) * widthPercent / 100
```

Missing `selectionHPadding*2` and the `widthPercent` scaling meant a title
that "fit" the approximate clamp still pushed `dialogW` past the hint once
that formula added the same padding back on top a second time. Measured on
the S8 2026-08-24: the Add/Replace list at 280px, a one-line confirm title at
363px even with the clamp in place, both hinted at 280. The exact inverse --

```
hintedTitleMax = minDialogWidth * 100 / widthPercent - innerPadding*2 - selectionHPadding*2
```

-- forces a second wrapped line when the hint genuinely has no room for one,
and the two now land on the same width exactly. Same fix, same reasoning, in
both `optionPopupGeometry()` and `drawOptionPopup()` again -- they still have
to agree, and now they agree on the right number.

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
