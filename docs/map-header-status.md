# The map screen's header status row

Top-right of the map, above the compass: a globe, a Bluetooth logo, four signal
bars and the battery block. Drawn by `MapActivity::drawHeaderStatus()` and
`drawHeaderStatusStrip()` (`src/activities/map/MapActivity.cpp`).

No other screen has one. Battery goes through `GUI.drawHeader()`, the same call
every other activity makes, so its position matches the info-list screens
exactly; everything left of it is this screen's own, because no other activity
has a wireless link worth showing.

## What each part means

| Part | Source | Meaning |
|---|---|---|
| Battery | `GUI.drawHeader()` | same as every other screen |
| Signal bars | `bleBarsForRssi(ble.rssi())` | link quality to the phone |
| X over the bar slot | `connIntervalMs() == 0` | no central connected |
| Bluetooth logo | always | what the bars are about |
| Globe | `autoSyncPending_ > 0` | tiles are being fetched over the phone's data |

The globe is not an internet indicator in the literal sense -- this device has
no radio that reaches the internet. It is about the thing the rider cares about:
the phone is spending mobile data on their behalf right now. See
`missing-tiles.md`, "Autosync".

## Two rects, one source

`headerStatusRect()` gives the strip: the globe slot, the logo, the bars, plus
the opaque backing's padding. Deliberately **excludes** the battery block, which
`GUI.drawHeader()` clears and draws itself.

Everything drawn must be inside it, because it is also what the windowed
repaint refreshes. Two consequences worth stating:

- **The globe's slot is in the rect whether or not the globe is drawn.** A rect
  that shrank when the globe went away would leave the globe's pixels on the
  panel with nothing to erase them.
- **`updateHeaderStatus()` calls `drawHeaderStatusStrip()`, never
  `drawHeaderStatus()`.** The latter also redraws the battery, which is outside
  the window -- that would put a battery in the framebuffer the panel will not
  show until the next full frame, and if the charge moved in between, the two
  disagree.

The row needs an opaque white backing for the same reason the compass halo and
the busy badge do: it lands on live map lines, not on blank margin.

## The debug readout sits below this row, not inside it

`MapActivity::drawHeaderStatus()` calls `GUI.drawHeader(renderer, Rect{0,
kHeaderMarginTop, screenWidth, kHeaderRowHeight}, ...)` -- a **full-width**
clear, not just the right-hand icon strip `headerStatusRect()` covers above.
`kHeaderRowHeight` (`MapActivity.cpp`, `BaseMetrics::values.batteryHeight +
10` = 22) is that rect's height, named so the debug readout's own top offset
(`kTextTopY = kHeaderMarginTop + kHeaderRowHeight + kTextGapBelowHeader`) can
be derived from it instead of guessed.

Two bugs this fixed, both found on hardware 2026-08-08, in the same session
the readout got a toggle and a white backing:

- **Backing box wider than the text erased the header icons.** The first cut
  sized the readout's backing to nearly the full screen width. `drawHeaderStatus()`
  runs before the readout, so a wide white box painted straight over the
  battery/BLE icons it had just drawn, on the same frame. Fixed by sizing each
  line's own backing to that line's own `getTextWidth()`, in `drawDebugLine()` --
  never wider than the glyphs it is behind.
- **A fixed 18px line gap was shorter than the font's own line height,** so
  each line's backing (drawn after the line above it) erased the bottom few
  pixels of that line's text. `ubuntu_10_regular`/`bold`'s `EpdFontData`:
  `advanceY` 24, `ascender` 20, `descender` -4 (`lib/EpdFont/builtinFonts/
  ubuntu_10_regular.h`) -- so line spacing is now `renderer.getLineHeight(
  UI_10_FONT_ID) + 2*kDebugPad` (30px), derived from the font instead of a
  guessed constant.

Before either fix, `kTextTopY` was a guessed `16` with no relation to this
row at all -- close enough to its own `[6, 28)` band that the readout read as
glued to the status row rather than sitting under it, even on the frames
where the backing fix alone had already stopped it from erasing the icons.

**Verified on hardware, 2026-08-08**: `CMD:GOTO_MAP` + `tools/screenshot_gate.py`
grabs, before and after each fix. The last grab shows a clean white gap
between the status row and the first debug line, and a clean gap between the
two debug lines -- no erased icons, no overlapping text.

## "Connected" is the connection interval, not the MTU

Worth its own heading because the obvious signal is the wrong one, and it was
wrong here for a day.

`negotiatedMtu() != 0` does not mean a central is connected. It means a central
**completed an ATT MTU exchange**, and the central decides whether to do one --
the device only states a preference (`BlePositionServer.h`, `negotiatedMtu()`).

Measured 2026-08-07: a BlueZ client connected, subscribed to the command
channel, ran `tiles` and received all four reply lines with the link plainly
working, while `negotiatedMtu()` stayed 0 for the whole session. This row tested
the MTU, so it drew the "no link" X the entire time. The proof was `info`
omitting its `mtu` line -- the same session's `INFO` block had zoom, tiles and
ways but no `mtu` and no `interval`.

It is not even consistent per client: an earlier connection from the same tool
that day *did* exchange (`MTU now 256` at +46 ms). So the failure is
intermittent, which is worse than always-broken.

`connIntervalUnits()` is the honest signal. `ServerCallbacks::onConnect` sets it
unconditionally from `info.getConnInterval()`, and `onCentralDisconnect()` zeroes
it, so it tracks the connection and nothing else.

**The rider's own phone was never affected** -- the Android app does exchange
(256 measured, twice, 2026-08-07 and 2026-08-06). This only ever lied to a
client that did not, which is why the morning's bug report was about the bars not
*clearing* rather than about them never appearing.

`info`'s own `mtu` line stays gated on the MTU being non-zero. That one is about
the MTU, so omitting it when there is none is correct, not the same bug.

## The repaint policy

**The row used to be drawn only by a full frame.** Measured in a real session
(2026-08-07): a phone connected, the bars appeared correctly, the rider closed
the GPS app on the phone -- and the bars stayed on the panel. Nothing was wrong
with the state. `BlePositionServer::onCentralDisconnect()` clears `mtu_`,
`commandSubscribed_` and the connection handle
(`lib/BlePositionServer/src/BlePositionServer.cpp:462-474`), so `negotiatedMtu()`
already answered 0. There was simply nothing that repainted the row to say so.

`MapActivity::updateHeaderStatus()`, called from `loop()`, is that. It compares
live state against what was last drawn (`transferIconShown_`,
`drawnLinkConnected_`, `drawnBleBars_` -- all written by
`drawHeaderStatusStrip()` on every path that draws the row, full frames
included) and refreshes only the strip when they differ.

Three rate limits, each for its own reason:

- **`kHeaderPollMs`, 2 s** -- how often the state is looked at at all.
  `rssi()` is a NimBLE host call (`ble_gap_conn_rssi`), and asking it every
  `loop()` tick to answer a question about a 14 px icon is not a trade worth
  making. Half a hertz is still prompt for a link that dropped.
- **`kHeaderBarsRepaintMs`, 30 s** -- the floor between two repaints caused by
  nothing but a moving bar count. RSSI sitting on a `bleBarsForRssi` threshold
  flips the count back and forth, and every flip is a real waveform pass.
- **No limit at all on a structural change** -- a link appearing or dropping, or
  a transfer starting or ending. That one changes what the row *means*, and the
  bug above is exactly what happens when it waits.

Nothing repaints before `viewportDrawn_`: the waiting banner draws no header row
at all, and painting one onto it would leave a floating status row over a screen
with no map.

## Fixed height, a separator, and the place name (2026-08-11)

The header used to be a set of independent clear-rects the map happened to
get painted over underneath (`drawHeaderStatus()`'s old `batteryClearBottom`
/ `stripBottom` / `kHeaderExtraMargin` dance). It is now a fixed-height
contract: one white strip `[0, kHeaderBarHeight)`, a 1px black separator row
at `kHeaderSeparatorY` (== `kHeaderBarHeight`), and the map's own content
starting only at `kMapContentTop` (`MapActivity.cpp`).

**The map does not draw above that line at all -- it is not drawn and then
covered.** `GfxRendererCanvas` (`src/activities/map/GfxRendererCanvas.h`)
already clipped every draw call to the screen bounds (roads loaded for a
wider tile range than the viewport need this, see the class comment); it
now takes a `minY` constructor argument, threaded through `onScreen()`,
`fullyOnScreen()`, `outcode()` and `clipToRect()` in place of the `0` they
all used before. `MapActivity` constructs it with `kMapContentTop` at both
call sites (`renderViewport()`, `drawRouteOverview()`). `test/map_preview`'s
`PpmCanvas` has no header and is unaffected -- it never took this parameter.

`kHeaderBarHeight` (36px) is derived from the row's own former clear-bottom
math (`kHeaderMarginTop + 5 + kHeaderRowHeight + kHeaderExtraMargin + 1`), not
a fresh guess, so the existing icon layout (battery bottom ~28px, BLE strip
backing bottom ~31px) needed no retuning and the marker ladder
(`MapViewport::kMarkerLadder`, 400-760px) was never close to this band to
begin with.

**The place name, left side.** `drawHeaderPlaceName()` shows the nearest
named place to the marker, read from `MapNearestPlaces`
(`src/activities/map/MapRenderer.h`) -- populated by `MapRenderer::render()`
during the same places walk that draws the dots (`MapRenderer.cpp`), not a
second pass and not a second SD read (`IMapSource.h`: "The second pass is a
second seek" is exactly the cost this avoids). Two independent nearest-picks,
by squared screen-pixel distance to the marker:

- **fine**: nearest place with rank >= 2 (village/suburb/hamlet/farm)
- **coarse**: nearest place with rank <= 1 (city/town)

Shown as `"fine, coarse"` when both are set (e.g. "Karlova Ves, Bratislava"),
whichever one alone when only one is, and nothing when neither loaded tile
carries a name near the marker. **There is no third option that guarantees
the pair is correct** -- the tile format has no admin hierarchy linking a
suburb to its city (`mapbuilder/build_config.json`'s `place_ranks` is a flat
rank, not a tree), and "nearest of each tier separately" is a proximity
approximation of that pair, not a lookup of it. Both picks are also bounded by
whatever tiles are already loaded for the current viewport (`MapViewport::
kMaxTiles`, 3x3 worst case) -- there is no wider, dedicated query for the
coarse tier, so a rider zoomed in tight in open country between towns will
often see the fine name alone, or nothing.

Truncated to fit with the same loop `drawDebugLine()` uses, against
`headerStatusRect()`'s own left edge minus a small gap -- so it stops before
the icon cluster rather than running under it.

**Position, tuned on hardware.** Neither margin matched the debug readout's
own: `kHeaderPlaceNameLeftX` is `kTextX + 2` (2px further right), and the
vertical centring formula gets `+ 3` on top of the plain centred value (0,
1 and 2 were each tried and screenshotted in turn and still read as too
tight against the top edge). Both are `MapActivity.cpp` constants/literals
next to `drawHeaderPlaceName()`, not derived from anything else -- if the
font or `kHeaderBarHeight` changes, re-check by eye rather than assuming the
same offset still reads right.

## Verified vs assumed

- **Measured on hardware, by the maintainer**: with the phone's GPS app, the bars
  appear on connect and are correct; before the fix they did **not** clear when
  the app was closed (2026-08-07, the session that prompted this doc).
- **Read off the code**: the disconnect state was always right -- both `mtu_` and
  the connection interval are zeroed at `BlePositionServer.cpp:462-474`.
- **Measured on hardware, 2026-08-07**: **no full frame runs when the link
  changes.** A 36 s serial capture spanning a connect, 12 s connected and a
  disconnect contains zero `[MAP] renderViewport` lines. So any change to this row
  in that window can only have come from the windowed strip repaint.
- **Not separately measured**: that the panel's X *appeared* in that same window.
  The screenshot proving the X and the log proving no full frame are from two
  different runs, because both need the one serial port. The two together are
  strong but they are not one observation. What would settle it in one: a build
  that logs each strip repaint, so a single capture shows the repaint and the
  absence of a render.
- **Corrected the same day**: the earlier version of this section claimed the
  screenshot showed the strip repainting "with the map frame underneath
  untouched". One image cannot show that nothing else was repainted. The claim
  that survives is the one above -- no render ran in the window.
- **Verified on hardware, 2026-08-07**: the globe draws at 14 px, clear of the
  Bluetooth logo and not clipped by the strip rect. Honest note on the glyph: a
  circle with a straight equator and a straight meridian reads as a
  crosshair-in-a-circle about as much as a globe. It is unambiguous against
  everything else in that row, but curving the meridian would make it a globe
  rather than a target.
- **Not verified**: the 30 s bar-repaint floor doing its job on a link whose RSSI
  actually sits on a threshold. Every run here was at close range with a steady
  signal. What would settle it: walk the phone to the edge of range with the map
  up and count repaints over a few minutes.
- **Not verified**: the exact latency from disconnect to the X appearing. The
  poll is 2 s by construction, but nothing has timed it.
- **Verified on hardware, 2026-08-08**: the debug readout's backing box no
  longer erases this row's icons, and the readout itself sits below the row
  with a visible gap -- see "The debug readout sits below this row, not
  inside it" above.
- **Verified on hardware, 2026-08-11**, `CMD:SCREENSHOT` via
  `tools/screenshot_gate.py`: the fixed header strip, the 1px separator, and
  the map's top clip all read correctly on the panel -- the map's own lines
  start right at the separator with nothing bleeding above it. The place
  name lookup also works end to end on real tile data: at the fix rendered
  (z11, 23 places in the loaded tile range), the header showed a real
  fine+coarse pair, not a placeholder string (name withheld here --
  it was the device's persisted GPS fix, close to a real address; see
  the parent CLAUDE.md's screenshot-privacy rule). Icon cluster (BLE X,
  95% battery) unaffected, as expected -- `drawHeaderStatusStrip()` itself
  was not touched.
- **Verified on hardware, 2026-08-11, a second grab**: a Slovak diacritic
  renders correctly in the header font -- a real place name (withheld, same
  reason as above), the ž a clean glyph, not a fallback box. Same grab is
  where the vertical/horizontal offset tuning above (`+2px` left, `+3px`
  top) happened, alongside the scale bar (`docs/map-scale-bar.md`) on the
  same screen. No shot kept in the repo -- privacy, not a rendering concern.
- **Not yet checked**: the truncation path (no name seen so far has been
  close to running into the icon cluster) and every other zoom rung's places
  walk.
