# The Home screen

The first screen after boot. One brand block, one list of seven rows, nothing
live. Redesigned 2026-08-21 from a design sketch: mountain line art with the
ExplorInk mark and wordmark over it, then the rows.

Status: **verified on the X4 2026-08-21** -- flashed, screenshotted off the panel
(`tools/screenshot_gate.py`), heap read off serial. What is still unverified is
button and touch behaviour; see the end.

## Layout, in device pixels

The panel is 480x800 in Portrait. Lyra is the default theme
(`src/CrossPointSettings.h:360`), and the numbers below are its metrics
(`src/components/themes/lyra/LyraTheme.h:11-32`).

| Band | Top | Height | Where it comes from |
|---|---|---|---|
| status header | 5 | 56 | `metrics.topPadding`, `metrics.homeTopPadding` |
| brand block | 64 | 224 | `kHeaderGap` + `HOMEHEADER_HEIGHT` |
| rows | 296 | 7 x 60 = 420 | `kRowHeight` |
| button hints | 760 | 40 | `metrics.buttonHintsHeight` |

That leaves 44 px of air above the hints. The row height is Home's own constant,
not `metrics.menuRowHeight` (64 in Lyra): at 64 the list ends 16 px above the
hints, which read as one block on the panel -- judged on the device, not in the
preview.

The rows are **contiguous** -- no `menuSpacing` between them, unlike the old
four-row menu -- which is what buys the brand block its 224 px. `HomeActivity::loop()` walks touches with the row
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
text size. The line screen was picked on the laptop rasteriser and then held up
on the glass (2026-08-21), which is the only judge that counts: a tone that reads
as grey on an LCD can read as flat on the panel (`../CLAUDE.md`,
`CMD:SHOWIMAGE`).

The selected row is a filled black block with white icon, label and chevron --
`drawMono1bpp()`'s `state` argument is what makes a white glyph possible
(`lib/GfxRenderer/GfxRenderer.h:266-278`). The hairline separator is skipped
above the selected row and above the first row: the block is its own boundary.

## The assets, and the two scripts that bake them

**Brand block**: `src/images/home-header.svg` -> `src/images/HomeHeader.h`, via
`scripts/gen_home_header.py`. 480x224, 13 kB of flash, drawn with
`drawMono1bpp()`.

* The mountains were traced with potrace from the design sketch, so the art is
  editable vector, not a screenshot. Edit the SVG, re-run the script.
* The logo mark (`src/images/logo.svg`) and the wordmark
  (`../docs/branding/explorink-wordmark-path.svg` in the parent repo) are placed
  in that SVG as paths, over white knockout plates that keep them readable
  against the lines.
* Threshold 215, no dithering. Measured on the laptop rasteriser: dithering
  drops the ink from 13.8 % to 10.0 % of the block and eats the 1-2 px ridge
  lines.
* **`drawImage()` cannot place a block this wide, and fails silently.** Measured
  on hardware 2026-08-21: the first pass drew no header at all. `drawImage()`
  rotates the origin and not the bits, and in Portrait the rotated origin is
  `(panelHeight - 1 - x) - height` (`lib/GfxRenderer/GfxRenderer.cpp:1211-1215`).
  Art 480 px wide sits at `x = 0`, so that is `479 - 480 == -1`, and the blit
  takes `y` as a `uint16_t`: `destY` becomes 65535, `destY >= displayHeight`
  breaks on the first row
  (`freeink-sdk/libs/display/FreeInkDisplay/src/FreeInkDisplay.cpp:217-219`).
  Nothing is written and nothing is logged.

  So this asset is **not** pre-rotated, unlike `ExplorinkWordmark` and every
  other bitmap in `src/images/`. `drawMono1bpp()` plots row->y and col->x
  through `drawPixel()`, applies the orientation itself, and its `state`
  argument is also what lets the row glyphs draw white on the selected block
  (`lib/GfxRenderer/GfxRenderer.h:262-278`). The wordmark's pre-rotation trap
  (`docs/sleep-screen.md`, "The wordmark image") only applies to `drawImage()`
  callers.

**Row icons**: `src/components/icons/home_icons.h`, via
`scripts/gen_home_icons.py`. 32 px row glyphs, 22 px chevron, Lucide sources
from `freeink-sdk/libs/assets/Icons/lucide/icons/`, emitted as `freeink::Icon`
structs in the same format the SDK generator uses.

The script exists next to the SDK's `gen_icons.py` rather than calling it for
two reasons: that one needs `rsvg-convert` (not installed here -- this one falls
back to cairosvg, like `scripts/gen_pin_icons.py`), and it is square-only, while
the Explore glyph is the ExplorInk mark at 32x31.

**This does satisfy the icon rule**, and the note now sits at both use sites --
the top of `src/components/icons/home_icons.h` (generated, so it comes from the
script's banner) and the row table in `HomeActivity.cpp`. It is written down
because a session read the rule as broken here and objected. Two facts settle
it: every row glyph names a Lucide source SVG, and the one glyph that is not
Lucide is the ExplorInk brand mark, which the maintainer asked for and which has
no Lucide equivalent. The deviation is the *rasteriser*, not the icon source:
same `freeink::Icon` output format, same threshold 110, cairosvg instead of
rsvg-convert. Install `librsvg2-bin` and the two paths agree.

## Hardware pass, 2026-08-21

Verified on the X4, panel screenshots via `CMD:SCREENSHOT`:

* The brand block draws, and the traced 1-2 px ridge lines resolve on the glass
  at threshold 215 -- they do not turn to mud.
* Dimmed rows read as "not yet", not as a broken render. The line screen holds
  at this text size.
* Heap unchanged: `Free 124,124 B, Min Free 123,916 B, MaxAlloc 114,676 B`
  (serial `[MEM]`, Home screen up, 10 s cadence). Build: RAM 17.8 %
  (58,332 bytes), flash 60.2 %.

Still unverified -- nothing has driven the input yet:

* That Up/Down skip the three dimmed rows in both directions and wrap.
* That a tap lands on the row it looks like it lands on, with a 60 px step and
  no gap between rows, and that a tap on a dimmed row does nothing.
