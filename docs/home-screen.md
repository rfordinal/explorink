# The Home screen

The first screen after boot. One brand block, one list of five rows, nothing
live. Redesigned 2026-08-21 from a design sketch: mountain line art with the
ExplorInk mark and wordmark over it, then the rows.

Row count changed 2026-10-25: Pins and Wallet used to sit in this list,
dimmed and unselectable, because neither had anywhere to go yet. Dimmed
rows read as "coming soon" on a shipped product, which this is not
building toward on any fixed date -- so both rows are gone rather than
dimmed. See "Dimmed rows, removed 2026-10-25" below for what that
mechanism used to do and why it is gone, not just unused.

Status: the seven-row layout was **verified on the X4 2026-08-21** -- flashed,
screenshotted off the panel (`tools/screenshot_gate.py`), heap read off serial.
The five-row layout (Pins and Wallet removed) is **unverified** as of
2026-10-25 -- builds clean, not yet flashed. See the end.

## Layout, in device pixels

The panel is 480x800 in Portrait. Lyra is the default theme
(`src/CrossPointSettings.h:360`), and the numbers below are its metrics
(`src/components/themes/lyra/LyraTheme.h:11-32`).

| Band | Top | Height | Where it comes from |
|---|---|---|---|
| status header | 5 | 56 | `metrics.topPadding`, `metrics.homeTopPadding` |
| brand block | 64 | 224 | `kHeaderGap` + `HOMEHEADER_HEIGHT` |
| rows | 296 | 5 x 60 = 300 | `kRowHeight` |
| button hints | 760 | 40 | `metrics.buttonHintsHeight` |

That leaves 164 px of air above the hints, up from 44 px when the list held
seven rows (`src/activities/home/HomeActivity.cpp`, `render()` passes
`pageHeight - menuTop - metrics.buttonHintsHeight` as the menu rect height, so
the rect shrinks to match `kRowCount` and the extra space falls below the last
row, not into it -- `BaseTheme::drawHomeMenu` top-aligns and never stretches
rows to fill a taller rect). Left as open air rather than taller rows or
vertical centering: not asked for, and a bigger gap under a five-row list is
not a defect to design around until someone judges it wrong on the glass.

The row height is Home's own constant, not `metrics.menuRowHeight` (64 in
Lyra): at 64 the list ends 16 px above the hints, which read as one block on
the panel -- judged on the device, not in the preview.

The rows are **contiguous** -- no `menuSpacing` between them, unlike the old
four-row menu -- which is what buys the brand block its 224 px. `HomeActivity::loop()` walks touches with the row
height as the step for the same reason
(`src/activities/home/HomeActivity.cpp`, `rowTouch`).

## The rows

| Row | Destination | Icon |
|---|---|---|
| Explore | `goToMap()` -- straight to the map, no picker | the ExplorInk mark |
| Trips | `goToRouteSelect()` (falls through to the map with no trips) | lucide `backpack` |
| Sync | `goToTileSync()` | lucide `refresh-cw` |
| File Transfer | `goToFileTransfer()` | lucide `wifi` |
| Settings | `goToSettings()` | lucide `settings` |

Changed 2026-08-22: Explore used to open the trip picker first (falling
through to the map only when the card had no trips). Explore now jumps
straight to the map every time -- the fast path a rider wants when walking out
the door. The trip picker moved to Trips, which is where "pick which trip"
already belonged conceptually (`src/activities/home/HomeActivity.cpp:210-213`).

`ENABLE_PREVIEW_BENCH` adds the grayscale bench as a sixth row before
Settings, same as before (`platformio.ini`).

Pins and Wallet are not on this list, and never had their own row to open:

* **Pins** exists only as a popup inside the map (`docs/pins.md`,
  `src/activities/map/MapActivity.h:526`). A Home row would need an entry path
  into that popup, which is not built.
* **Wallet** has an activity, on the `wallet-viewer` branch. It is not on
  `develop`, so there is nothing to open here yet.

Both used to sit in the list anyway, drawn dimmed and refusing selection, so
the menu could name a feature before it existed. Removed 2026-10-25 (see
below): a dimmed row is the right call for a feature landing on a known
schedule, and neither of these has one.

`HomeMenuItem` still carries `PINS` and `WALLET` (`src/activities/ActivityManager.h:20-38`)
even with no row naming them -- removing the enum values is unnecessary churn
that would have to be undone the day either gets a real destination.

## Dimmed rows, removed 2026-10-25

Through 2026-10-24 this screen carried Pins and Wallet as **dimmed** rows:
drawn in full black, then half their ink dropped to white a scanline at a
time (`BaseTheme::dimDisabledRow`, `kDimStep` in `BaseTheme.cpp`), refusing
selection (`HomeActivity::nextSelectable()` skipped them, `activate()`
checked `enabled` before dispatch). `HomeActivity::Row` and
`BaseTheme::HomeRow` each carried an `enabled` bool to drive that.

Taken out because a dimmed row visually promises "coming soon", and neither
feature has a ship date -- so it kept reading as a promise this product was
not making. `HomeActivity::Row` and `BaseTheme::HomeRow` lost the `enabled`
field, `HomeActivity::nextSelectable()` became the simpler `stepRow()` (a
plain wrap, nothing to skip), and `BaseTheme::drawHomeMenu()` lost its call
into `dimDisabledRow()`.

`dimDisabledRow()` itself is **not** gone: `OptionPopup` still dims its own
disabled rows through the same helper (`BaseTheme.cpp:1569`) for options that
are genuinely unavailable in the current state (not merely unbuilt), which is
the case dimming still fits. If Pins or Wallet gets a real destination later,
it is a new row in the table below, not a reinstated disabled one.

The line-screen tone itself (half the ink, shape intact, not a checkerboard)
was picked on the laptop rasteriser and held up on the glass 2026-08-21: a
tone that reads as grey on an LCD can read as flat on the panel (`../CLAUDE.md`,
`CMD:SHOWIMAGE`). That finding stays correct and still applies wherever
`dimDisabledRow()` is called today -- it is recorded here because this is
where it was discovered, not because Home still uses it.

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

## Hardware pass, 2026-08-21 (seven-row layout, historical)

Verified on the X4, panel screenshots via `CMD:SCREENSHOT`, back when the list
held seven rows including dimmed Pins and Wallet:

* The brand block draws, and the traced 1-2 px ridge lines resolve on the glass
  at threshold 215 -- they do not turn to mud.
* Dimmed rows read as "not yet", not as a broken render. The line screen held
  at this text size.
* Heap: `Free 124,124 B, Min Free 123,916 B, MaxAlloc 114,676 B` (serial
  `[MEM]`, Home screen up, 10 s cadence). Build: RAM 17.8 % (58,332 bytes),
  flash 60.2 %.

These numbers are for the removed seven-row build and are not evidence for the
five-row one below.

## Five-row layout, 2026-10-25

Pins and Wallet removed, status above. Unverified on hardware as of this
writing -- builds clean (`pio run`), not yet flashed. Still to check on the
real device:

* That the brand block and the five remaining rows draw as the layout table
  above predicts, with the extra 120 px of air landing below Settings and not
  squeezed into the rows.
* That Up/Down and swipe wrap across exactly five rows (`stepRow()`).
* That a tap lands on the row it looks like it lands on, with a 60 px step and
  no gap between rows.
* Heap, against the 2026-08-21 seven-row baseline above -- expect a small drop
  (two fewer icons drawn, two fewer `tr()` lookups per frame), not a jump.
