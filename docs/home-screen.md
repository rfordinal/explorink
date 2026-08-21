# The Home screen

The first screen after boot. One brand block, one list of seven rows, nothing
live. Redesigned 2026-08-21 from a design sketch: mountain line art with the
ExplorInk mark and wordmark over it, then the rows.

Status: **built, not yet run on hardware.** Every number below is read off the
code or measured on the laptop rasteriser. What a device pass has to check is
listed at the end.

## Layout, in device pixels

The panel is 480x800 in Portrait. Lyra is the default theme
(`src/CrossPointSettings.h:360`), and the numbers below are its metrics
(`src/components/themes/lyra/LyraTheme.h:11-32`).

| Band | Top | Height | Where it comes from |
|---|---|---|---|
| status header | 5 | 56 | `metrics.topPadding`, `metrics.homeTopPadding` |
| brand block | 64 | 224 | `kHeaderGap` + `HOMEHEADER_ON_SCREEN_HEIGHT` |
| rows | 296 | 7 x 64 = 448 | `metrics.menuRowHeight` |
| button hints | 760 | 40 | `metrics.buttonHintsHeight` |

That leaves 16 px of slack above the hints. The rows are **contiguous** -- no
`menuSpacing` between them, unlike the old four-row menu -- which is what buys
the brand block its 224 px. `HomeActivity::loop()` walks touches with the row
height as the step for the same reason
(`src/activities/home/HomeActivity.cpp`, `rowTouch`).

## The rows

| Row | Destination | Icon |
|---|---|---|
| Explore | `goToRouteSelect()` (falls through to the map with no routes) | the ExplorInk mark |
| Trips | none yet -- **dimmed** | lucide `backpack` |
| Pins | none yet -- **dimmed** | lucide `map-pin` |
| Wallet | none yet -- **dimmed** | lucide `wallet` |
| Sync | `goToTileSync()` | lucide `refresh-cw` |
| File Transfer | `goToFileTransfer()` | lucide `wifi` |
| Settings | `goToSettings()` | lucide `settings` |

`ENABLE_PREVIEW_BENCH` adds the grayscale bench as an eighth row before
Settings, same as before (`platformio.ini`).

Why three rows are dimmed rather than absent:

* **Trips** has no activity at all.
* **Pins** exists only as a popup inside the map (`docs/pins.md`,
  `src/activities/map/MapActivity.h:526`). A Home row would need an entry path
  into that popup, which is not built.
* **Wallet** has an activity, on the `wallet-viewer` branch. It is not on
  `develop`, so there is nothing to open here yet.

A dimmed row refuses selection: `nextSelectable()` skips it in both directions,
touch on it returns early, and `activate()` checks `enabled` again before
dispatch. `initialMenuItem` pointing at a dimmed row lands on the next enabled
one instead (`HomeActivity::onEnter()`).

`HomeMenuItem` carries `TRIPS`, `PINS` and `WALLET` so the table can name them
(`src/activities/ActivityManager.h:29-35`). No `goTo*` exists for any of them.

## Dimming, with no grey to dim with

BW mode has two levels: a pixel is black or white (`docs/eink-grayscale.md`).
`drawText()` takes a bool, not a colour, so a "grey" label is not available.

So a dimmed row draws in full black and then loses **every second pixel row** to
white across the row's width (`BaseTheme::drawHomeMenu`, `kDimStep`). Half the
ink, shape intact. A checkerboard was the other candidate and speckles at this
text size; the line screen was picked on the laptop rasteriser and is one of the
things a device pass has to confirm, since a tone that reads as grey on an LCD
can read as flat on the panel (`../CLAUDE.md`, `CMD:SHOWIMAGE`).

The selected row is a filled black block with white icon, label and chevron --
`drawMono1bpp()`'s `state` argument is what makes a white glyph possible
(`lib/GfxRenderer/GfxRenderer.h:266-278`). The hairline separator is skipped
above the selected row and above the first row: the block is its own boundary.

## The assets, and the two scripts that bake them

**Brand block**: `src/images/home-header.svg` -> `src/images/HomeHeader.h`, via
`scripts/gen_home_header.py`. 480x224, 13 kB of flash.

* The mountains were traced with potrace from the design sketch, so the art is
  editable vector, not a screenshot. Edit the SVG, re-run the script.
* The logo mark (`src/images/logo.svg`) and the wordmark
  (`../docs/branding/explorink-wordmark-path.svg` in the parent repo) are placed
  in that SVG as paths, over white knockout plates that keep them readable
  against the lines.
* Threshold 215, no dithering. Measured on the laptop rasteriser: dithering
  drops the ink from 13.8 % to 10.0 % of the block and eats the 1-2 px ridge
  lines.
* The buffer is written **pre-rotated 90 degrees** -- raw 224x480 for art
  authored 480x224 -- because `GfxRenderer::drawImage()` rotates the origin and
  not the pixel data, and the blit indexes the source as `w/8` bytes per row.
  The raw width must be a multiple of 8. Same trap as the wordmark on the boot
  screen: `docs/sleep-screen.md`, "The wordmark image".

**Row icons**: `src/components/icons/home_icons.h`, via
`scripts/gen_home_icons.py`. 32 px row glyphs, 22 px chevron, Lucide sources
from `freeink-sdk/libs/assets/Icons/lucide/icons/`, emitted as `freeink::Icon`
structs in the same format the SDK generator uses.

The script exists next to the SDK's `gen_icons.py` rather than calling it for
two reasons: that one needs `rsvg-convert` (not installed here -- this one falls
back to cairosvg, like `scripts/gen_pin_icons.py`), and it is square-only, while
the Explore glyph is the ExplorInk mark at 32x31.

## What a hardware pass has to check

* The brand block's fine lines on the panel. 1-2 px strokes at threshold 215
  looked right on the laptop; the panel is the only judge of whether the ridge
  hatching resolves or turns to mud.
* Whether the dimmed rows read as "not yet" and not as "broken render".
* The 16 px of slack: that the hints strip does not clip the Settings row.
* Touch: that a tap lands on the row it looks like it lands on, now that the
  step is 64 px with no gap, and that a tap on a dimmed row does nothing.
* Heap after the change (`../CLAUDE.md`, "Check heap after firmware changes").
  The build's static numbers: RAM 17.8 % (58,332 bytes), flash 60.2 %.
