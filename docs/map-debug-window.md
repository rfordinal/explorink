# The map's debug window

`SETTINGS.mapDebugInfo` turns on a box of diagnostic text over the map. Off by
default. Map menu, "Debug info" (`src/activities/map/MapActivity.cpp`, the
`debugInfoIdx` branch).

This file is about the window itself: who may write into it, where it lands,
and what it costs on the panel. What each line *means* belongs to the feature
that writes it.

New word here: a **slot** is one reserved row of the window. A feature asks for
one at startup and keeps the id. It never picks a y coordinate.

## Why it is an object and not four y coordinates

Before 2026-09-05 the readout was `MapActivity::drawDebugLine(int y, char*)`
plus a `line1Y`/`line2Y`/`line3Y`/`line4Y` ladder, recomputed at each of the
two call sites that drew it. Three problems, all of them structural:

- **The top was a compile-time constant.** `kTextTopY = kHeaderMarginTop +
  kHeaderRowHeight + kTextGapBelowHeader` = 42, in every mode. Hike mode's
  header bar ends at 58 (`MapActivity::headerBarHeight()`), so line one's
  backing started 19 px inside the elevation/lat-lon row and painted over it.
  A constant cannot see `mode_`.
- **Lines ran under the compass.** The trim was against `getScreenWidth() -
  2*kTextX` = 464 px. The compass halo's left edge is at 385
  (`kCompassCenterMarginRight` 56, `kCompassGlyphRadius` 36,
  `kCompassHaloMargin` 3). The compass is drawn earlier in the frame, so a long
  line erased it.
- **A third writer had nowhere to go.** The GNSS work wanted a line. With the
  ladder, that meant picking a number and hoping the other two call sites had
  not picked it.

So the window is now `MapDebugOverlay`
(`src/activities/map/MapDebugOverlay.h`, `.cpp`), a member of `MapActivity`.

## The contract

```cpp
uint8_t slot = debug_.reserve("gnss");        // once, in MapActivity's constructor
debug_.set(slot, "sats %u fix %s", n, state); // whenever, from your own loop
debug_.clear(slot);                           // when the line stops applying
```

Reservation happens in one block, in `MapActivity::MapActivity()`. **The order
of the `reserve()` calls is the order on the panel.** That is why they sit
together instead of next to the code that writes them: the list is how anyone
finds out what the window can show.

Today's six slots, in order: `fix`, `render`, `route`, `routefit`, `routeerr`,
`transfer`. `kMaxSlots` is 8.

`set()` is a `vsnprintf` into the slot's own buffer. No drawing, no panel, no
allocation. Safe to call every loop iteration with the same text -- the repaint
path compares against what was drawn and does nothing.

An empty slot takes no row. Rows below it move up and the box gets shorter.

## Geometry, recomputed every frame

`MapActivity::layoutDebugOverlay()` is the only place these numbers exist:

| bound | value | why |
|---|---|---|
| top | `mapContentTop() + kDebugGapBelowHeader` (5) | `mapContentTop()` is mode-aware, so Hike's second header row pushes the window down instead of being covered by it |
| text left | `kTextX` (8) | unchanged from the old readout |
| right limit | compass halo left `- kDebugGapBeforeCompass` (6) | the compass is drawn earlier in the frame; past this the box erases halo and glyph |
| bottom limit | `getScreenHeight() - kScaleMarginBottom` (50) | the same clearance line the scale bar and busy badge bottom out on |

5 reproduces the old spacing exactly in Ride and Cycle: the bar ends at 36, the
old constant put the box's top edge at 39. Nothing changes on the modes where
the look was tuned. In Hike the box top moves from 39 to 64.

The right limit applies in every mode, with no condition. The halo spans y
48..126 and the window starts at 42 (Ride) or 64 (Hike), so the box always
overlaps the halo's band. There is no mode in which it could safely run to the
screen edge.

## Staying inside the box

Two different failures, two different answers.

**Too wide: trim.** `MapDebugOverlay::paint()` chops characters off the end
until `getTextWidth()` fits the inner width. Not cosmetic:
`GfxRenderer::drawText` does not clip and `GfxRenderer::drawPixel` answers
every off-panel pixel with a `LOG_ERR`, so one overlong line is several hundred
error lines over USB CDC.

**Too many rows: drop.** A slot with no room left below `bottomLimit_` is not
drawn, and logs once -- `slot '<owner>' dropped: window holds N rows`. Once per
slot, not once per frame: a window too short is short every frame, and at 1 Hz
that is a log nobody can read past. This is why `reserve()` takes an owner tag:
it is what that line names.

`reserve()` itself only refuses when the table is full, and returns
`kInvalidSlot`. `set()` on that id is a no-op, so a caller that ignores the
return value goes silent rather than corrupting a neighbour. The vertical
refusal cannot happen at `reserve()` time -- capacity depends on the mode, and
the mode changes while the map is up.

## One box, not one backing per line

The window is a single white rect with a 1 px black frame, sized to the
reserved width, not to the text actually in it.

The old readout sized each line's backing to its own `getTextWidth()`. That was
a real fix at the time (2026-08-08): the backing was erasing the header icons,
because `drawHeaderStatus()` runs earlier in the same frame
(`docs/map-header-status.md`). **That reason is gone.** The window now starts
below `mapContentTop()` and stops at the compass halo, so there is nothing
underneath it but map.

And a fixed width is what makes the windowed repaint honest: a line that gets
shorter leaves no stale ink outside a backing that moved with it.

## The panel

Two paths reach the panel.

**Full frame.** `debug_.draw(renderer)` at the end of both frame paths
(`renderFromFix()`'s reset and `renderRouteOverview()`). Framebuffer only --
the frame's own refresh carries it.

**Polled.** `MapActivity::updateDebugOverlay()`, called from the map loop next
to `updateHeaderStatus()` and `updateHikeElevationLine()`, same shape as both:

- gated on `SETTINGS.mapDebugInfo` and `headerRowDrawn_` (nothing to keep
  honest before a frame put the window there),
- polled every `kDebugPollMs` (1 s),
- repaints only when `dirty()` -- some slot's text differs from what was drawn,
- floor of `kDebugRepaintMs` (5 s) between two panel refreshes, so a
  per-second counter cannot spend a waveform pass per second,
- `windowRefreshAffordable()` before `displayBufferWindow()`, because
  `displayBufferWindow()` allocates per call and a refused allocation aborts
  the device rather than failing.

This polled path is the point of the whole design. A feature writing its slot
from its own loop reaches the panel without triggering a frame and without
knowing anything about waveform cost.

**The box never shrinks between full frames.** `highWaterRows_` holds the
tallest it has been since the last `draw()`. Map pixels a taller box covered
were filled white and are not coming back without a re-render, so a shorter box
would leave a white scar. A row that empties leaves a blank row inside the box
until the next full frame.

## Two frame paths, two sets of slots

The follow frame and the route overview show different things. Each fills its
own slots and clears the other's, rather than sharing two rows whose meaning
depends on which frame drew last.

- follow: `fix`, `render`, `routeerr`, `transfer`; clears `route`, `routefit`
- overview: `route`, `routefit`; clears `fix`, `render`, `routeerr`

## Cost

`kMaxSlots` (8) x `Slot` (12 owner + 64 text + 64 drawn + 2 flags) = about
1.1 kB, a fixed member of `MapActivity`. No allocation on a screen that already
fights for heap during a viewport reset (`docs/map-memory.md`).

## Status

**Verified on the desktop simulator, 2026-09-05.** Not on hardware. Built
`pio run -e simulator` (SUCCESS, no warnings from these files), run headless
(`SDL_VIDEODRIVER=dummy SDL_RENDER_DRIVER=software`, `CROSSPOINT_SIM_SD`
pointed at a card holding `mapbuilder/cdn` and a `settings.json` with
`mapDebugInfo: 1`), a fix at 48.35 17.30 sent with `tools/blepos.py --sim`,
the mode switched with `tools/mapcmd.py --sim`. Two 480x800 captures, 1:1:

- **Hike.** The box starts below the elevation/lat-lon row and does not touch
  it. Two rows: `48.35000 17.30000 h0 #1`, `hike z0 m1 1t 188w 7ms`. The
  compass is whole -- the box's right edge stops short of the halo.
- **Ride.** Same box one row higher, below the single header row, place name
  (`Kráľová, Modra`) and compass both intact.

Before the change the Hike frame would have put line one's backing 19px inside
that second header row. That comparison is arithmetic, not a before-grab --
nobody captured the broken frame.

**Open -- still needs a hardware pass.** The simulator says nothing about
waveform cost or ghosting. What would settle it: `CMD:GOTO_MAP` on a device,
debug info on, Hike mode, a screenshot; then leave a slot updating and check
that (1) the 1 px frame reads as a boundary and not as clutter on e-ink, (2)
the polled repaint refreshes the box alone without disturbing the map, (3)
`kDebugRepaintMs` is a floor a rider would not notice.
