# The map's CONFIRM menu: layout, and closing it without a redraw

The map screen's CONFIRM button opens one flat popup: Refresh, Mode, Look
around / Follow mode, Whole route, and the zoom / rotation / heading / debug
toggles (`MapActivity::openMapMenu()`, `MapActivity.cpp:1841`). This file
covers three things changed 2026-08-12: how the rows are laid out, how closing
the menu got cheap, and why the button hint says "Options".

Status: **flashed and looked at on the panel 2026-08-12 -- layout, boxed value
and the fast close all read correctly.** Numbers (backdrop size, refresh time)
are still unmeasured; those are called out per section.

## Rows: label left, value boxed on the right

Rows used to be one string, `"Mode: Ride"`, centred. They are two columns now:
`OptionPopup::showWithValues()` (`OptionPopup.h:63`) takes `options` and a
parallel `values`, and `MapActivity::openMapMenu()` fills both. An empty value
means the row is a plain action (Refresh, Whole route).

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

**Cost.** One heap buffer, allocated on open and freed on close. The size is
the dialog's region: `getRegionByteSize()` rounds the rect out to byte
boundaries, so it is roughly `dialogWidth/8 * dialogHeight` bytes -- about
26 KB for the biggest dialog the map menu builds (eight rows, Lyra metrics).
**Open: not measured.** Log a `menu backdrop` line and the free heap on a real
device to confirm the number and the headroom.

On OOM the capture logs and returns false (`MapActivity.cpp:1791`), and every
close path falls back to `renderCurrent()` -- the behaviour before this
existed. Nothing new can strand the popup's pixels on the panel.

Precedent for the technique in the same file: the marker patch
(`saveMarkerPatch()`), which saves and restores the box under the position
marker so a fix can move it without redrawing the map.

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
straight back. Not verified by measurement:

- The backdrop's real byte size and the free heap while the menu is open.
- The window refresh time for a menu close, against the full redraw it
  replaces.
