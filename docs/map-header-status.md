# The map screen's header status row

Top-right of the map, above the compass: a clock, a globe, a Bluetooth logo,
four signal bars and the battery block. Drawn by
`MapActivity::drawHeaderStatus()` and `drawHeaderStatusStrip()`
(`src/activities/map/MapActivity.cpp`).

No other screen has one. Battery goes through `GUI.drawHeader()`, the same call
every other activity makes, so its position matches the info-list screens
exactly; everything left of it is this screen's own, because no other activity
has a wireless link worth showing.

## What each part means

| Part | Source | Meaning |
|---|---|---|
| Battery | `GUI.drawHeader()` | same as every other screen |
| Signal bars | `resolveBleBars(ble.rssi())` | link quality to the phone |
| X over the bar slot | `connIntervalMs() == 0` | no central connected |
| Bluetooth logo | always | what the bars are about |
| Globe | `autoSyncPending_ > 0` | tiles are being fetched over the phone's data |
| Clock | `BlePositionServer::localTimeNow()` | local time, from the phone; blank until it has sent one |

The globe is not an internet indicator in the literal sense -- this device has
no radio that reaches the internet. It is about the thing the rider cares about:
the phone is spending mobile data on their behalf right now. See
`missing-tiles.md`, "Autosync".

## Two rects, one source

`headerStatusRect()` gives the strip: the clock slot, the globe slot, the logo,
the bars, plus the opaque backing's padding. Deliberately **excludes** the
battery block, which `GUI.drawHeader()` clears and draws itself.

Everything drawn must be inside it, because it is also what the windowed
repaint refreshes. Three consequences worth stating:

- **The globe's and the clock's slots are in the rect whether or not either is
  drawn.** A rect that shrank when one went away would leave its pixels on the
  panel with nothing to erase them.
- **`updateHeaderStatus()` calls `drawHeaderStatusStrip()`, never
  `drawHeaderStatus()`.** The latter also redraws the battery, which is outside
  the window -- that would put a battery in the framebuffer the panel will not
  show until the next full frame, and if the charge moved in between, the two
  disagree.
- **The minute tick is the one exception**, and it pays for it: it redraws the
  battery *and* widens the window to the right screen edge so both are inside
  what is refreshed. See "The clock" below.

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

## A failed rssi() read must not draw full signal

**Fixed 2026-08-13.** `BlePositionServer::rssi()` returns `0` when
`ble_gap_conn_rssi()` fails (`lib/BlePositionServer/src/BlePositionServer.cpp:601-605`;
`BlePositionServer.h:323-327` documents 0 dBm as never a real reading on a
live link -- `docs/power-management.md` records this failing on this build).
`bleBarsForRssi()`'s thresholds are all negative (`MapActivity.cpp:478`), so a
failed read passed every one of them and drew 4 bars -- a dead RSSI read
looked identical to a perfect link.

`MapActivity::resolveBleBars(int8_t rssi)` (`MapActivity.cpp`, next to
`updateHeaderStatus()`) is the fix: it only updates `lastKnownBleBars_` when
`rssi != 0`, and always returns `lastKnownBleBars_` rather than
`bleBarsForRssi(rssi)` directly. Both call sites that used to call
`bleBarsForRssi(ble.rssi())` -- `updateHeaderStatus()`'s own change check and
`drawHeaderStatusStrip()`'s draw -- now go through it, so the two agree.

`lastKnownBleBars_` starts at, and is reset to, `0` in three places: the
member initializer, `onEnter()`, and the `!connected` branch of
`drawHeaderStatusStrip()`. That last one matters most -- it is what stops a
new connection from inheriting the previous one's signal, since the member is
otherwise held across reconnects within the same activity instance (a rider
who leaves the map screen up while the phone's app drops and reconnects).

Net behaviour:

| Case | Bars shown |
|---|---|
| First poll after connect fails (`rssi()` returns 0) | none (0, same picture as "no signal") |
| Mid-ride poll fails after an earlier good reading | the last good count, held |
| Next poll succeeds | live bars again |
| Disconnect, then a new connection | starts from 0 again, not the old connection's count |

**Read off the code, not measured on hardware.** No device access in this
change (firmware-task rule). `docs/power-management.md`'s open question --
whether `ble_gap_conn_rssi()` fails on every call on this build or only
sometimes -- decides how visible this fix is in practice: if it fails always,
the bars will sit wherever they were at connect time and never move again,
which is a real limitation of holding-last-known and not something this fix
can address without the underlying HCI call working at least once per
connection.

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
`drawnLinkConnected_`, `drawnBleBars_`, `drawnClockMinute_` -- all written by
`drawHeaderStatusStrip()` on every path that draws the row, full frames
included) and refreshes only the strip when they differ.

Four rate limits, each for its own reason:

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
- **The minute rolling over**, with no cap of its own. A minute *is* the cap,
  and it is 30x slower than the bars' floor. It is the only condition that
  fires while nothing else on the device is happening.

Nothing repaints before `viewportDrawn_`: the waiting banner draws no header row
at all, and painting one onto it would leave a floating status row over a screen
with no map.

## The clock (2026-08-15)

The device has no clock of its own. The X4 has no RTC -- `lib/hal/HalClock.h`
wraps a DS3231 that only the X3 carries, and the clock entries in the settings
menu are hidden on hardware without one
(`src/activities/settings/StatusBarSettingsActivity.h:26`). So the time on the
map header is the phone's time, and before the phone has sent one there is no
time to show.

### Where it comes from

Already on the wire, and has been since the packet was revised: `utc` at bytes
8..11 (uint32, unix seconds) and `tz_offset` at bytes 12..13 (int16, minutes
east of UTC) -- `lib/BlePositionServer/include/BlePositionServer.h:20-47`,
`docs/architecture-plan.md`, "Time is mandatory in the payload". The fields were
added ahead of any consumer precisely so adding one later would not need another
wire-format bump. This is that consumer.

### Why it is extrapolated, not read

**The stored `utc` alone would be the wrong number to draw.** The phone sends a
fix when the rider moves; park the bike and the packets stop. A header that
printed `latest_.utc` would freeze at the time of the last movement and keep
presenting it as the current time -- a clock that is confidently wrong, which is
worse than no clock.

So `BlePositionServer` keeps an anchor instead: the last non-zero `utc` and the
`millis()` at which it landed (`clockUtc_`, `clockStampMs_`,
`lib/BlePositionServer/src/BlePositionServer.cpp`, in `onWriteIngest()`).
`localTimeNow()` adds the elapsed milliseconds to the anchor. Between packets
the clock keeps running off the ESP32-C3's own tick; every fresh packet
re-anchors it.

Three details that are load-bearing:

- **The anchor is separate from `latest_`.** `latest_` is overwritten by every
  packet, including one carrying `utc == 0` -- the documented value for a sender
  with no clock. Letting one of those clear the anchor would stop the clock
  dead, so only a non-zero `utc` updates it.
- **Unsigned subtraction across the `millis()` wrap.** `millis() - anchorMs` is
  correct through the 49.7-day rollover; the device would have to be awake that
  long for it to matter, and it still would not break.
- **`localTimeNow()` returns the offset already applied**, so the caller does no
  timezone arithmetic. False means no time is known.

### Current time, not fix time -- a deliberate departure from the spec

`map-render-spec.md` asks for something different, and asked for it first. Its
rule 8 ("Fix time, always") makes the primary value **the time the phone took
the position being drawn**, and says that when fix time and redraw time
diverge, both must be shown, because "a device redrawing happily off a
40-minute-old fix must not look identical to one that is current".

**This clock shows current time instead.** Maintainer's call, 2026-08-15, made
with that trade stated. The spec has been amended to record it -- see its
"Current time in the header" note.

The cost is exactly what the spec warned about and it has not gone away: a
stale position now looks current. Nothing else on the header carries fix age
either, so today the rider has no on-screen signal that the dot is old. If that
turns out to bite on a real ride, the fix is additive rather than a rewrite --
the anchor already knows when the last packet landed, so a staleness marker
beside the clock is a small change. Nobody has ridden with this yet.

### What is drawn

24-hour, `H:MM`, leftmost in the status strip. `SETTINGS.clockFormat` exists
(`src/CrossPointSettings.h:226-231`) and is **not** honoured: its menu entry is
hidden on a device with no RTC, so reading it here would obey a setting the
rider cannot reach.

The slot is reserved whether or not a time is known -- same rule as the globe's
slot, same reason. The text is right-aligned inside it, so the colon does not
walk sideways between `9:05` and `10:05`.

Its width comes from `headerClockSlotWidth()`, which measures the **widest
digit** and takes four of them plus a colon. Measuring `"00:00"` instead would
be a bug: `SMALL_FONT` is proportional, so `23:59` can be wider, and because the
text is right-aligned an over-long string overflows *left* -- past the opaque
backing, outside `headerStatusRect()`, onto pixels the windowed repaint never
refreshes and therefore never erases. The sum-of-parts ignores kerning, which
shortens real strings more often than it lengthens them, so the estimate errs
wide. That is the safe direction.

Vertically it sits on `kHeaderTextTopY`, **not** the icon row. The two are 4px
apart: `drawHeader()` hands `drawBatteryRight()` `rect.y + 5`
(`src/components/themes/BaseTheme.cpp:387`) and that function draws its
percentage string at exactly that y (`:119`), while the +6 it adds is for the
icon box only (`:114`). The clock stands next to the percentage, so it matches
the percentage. `headerStatusRect()` therefore starts at whichever of the two
rows is higher -- text drawn above the rect would land outside the window the
repaint refreshes, which is the same stale-pixel failure the feature exists to
avoid.

**No time yet draws nothing**, not `--:--`. A placeholder reads as a fault; a
blank slot reads as "not a thing this screen has".

### `GUI.drawHeader()` clears the full width, and it cost a hardware bug

**Found on hardware 2026-08-15.** The first cut of the minute tick drew the
strip and *then* called `GUI.drawHeader()` to refresh the battery. On the panel
the clock, the Bluetooth logo and the signal bars vanished a moment after
appearing, and so did the place name -- each reduced to a single row of pixels.

The comment that justified that order was wrong about which theme runs.
`GUI` is whatever `SETTINGS.uiTheme` selects, and the shipped default is Lyra
(`src/CrossPointSettings.h:334`, `uiTheme = LYRA`). `LyraTheme::drawHeader()`
opens with a clear of **the whole rect it is handed**:

```cpp
// src/components/themes/lyra/LyraTheme.cpp:112
renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);
```

Handed `Rect{0, kHeaderMarginTop, screenWidth, kHeaderRowHeight}` that is all
480 columns, rows 6..27. Only `BaseTheme` confines itself to an 80px battery box
(`src/components/themes/BaseTheme.cpp:383`) -- and `BaseTheme` is not what
ships.

The single surviving scanline is the tell, and the arithmetic is exact.
`GfxRenderer::drawText()`'s `y` is the **top of the line box**, not a baseline:
`const int yPos = y + getFontAscenderSize(...)` (`GfxRenderer.cpp:571`).

| element | draw y | ascender | baseline | last ink row |
|---|---|---|---|---|
| place name, `ubuntu_10` | 9 | 20 | 29 | 28 |
| clock, `notosans_8` | 11 | 18 | 29 | 28 |

The clear covers rows 6..27 inclusive. Row 28 is one below its bottom edge, so
row 28 is exactly what survives -- for both, from two different fonts, by
coincidence of a shared baseline.

**The rule this leaves behind: never call `GUI.drawHeader()` after drawing
anything else into the header band.** `drawHeaderStatus()` already has the
order right (clear, battery, strip, place name, separator), so the minute tick
now calls that one function and refreshes the whole band rather than keeping a
second copy of the ordering in step with it.

### The battery rides the minute tick

Charge moves slowly enough that it never earned a repaint of its own, and it had
none: the battery is drawn by `GUI.drawHeader()`, which sits outside
`headerStatusRect()` entirely, so every strip repaint deliberately left it
alone. A minute is a fine rate to notice charge at, so the minute tick now
redraws the whole row via `drawHeaderStatus()` and refreshes the whole band --
480x36 once a minute instead of a strip. The bars-only and structural paths
still redraw and refresh the strip alone, unchanged.

Those partial paths still never call `drawHeaderPlaceName()`. That is fine only
because nothing they draw reaches the name: `headerStatusRect()` is bounded to
`clockLeft..barsRight` and the name is truncated against `clockLeft`. Widen
either and the name has to be redrawn with them.

### Verified vs assumed

- **Measured on hardware 2026-08-15**, from decoded `CMD:SCREENSHOT`
  framebuffer dumps: the clock draws and reads correctly, and **the minute tick
  fires while the device is idle**. Over 75 s with no interaction and no forced
  render, the dumped framebuffer went `23:43` -> `23:45` and the battery 70% ->
  72%. The battery is the load-bearing half of that: the structural and
  bars-only paths deliberately do not touch it, so only the minute tick can
  have moved it.
- **Measured on hardware 2026-08-15**, same method: the full-width clear bug
  above, and its fix.
- **Read off the code, not measured:** everything about which function draws
  what and when, and the font arithmetic in the clear-bug section.
- **Builds clean**: no warnings, RAM 17.8% (58,268 of 327,680 bytes), flash
  59.3%. The added state is 10 bytes in `BlePositionServer` and 2 in
  `MapActivity` -- below the resolution of the build's own RAM figure.
- **Open -- needs hardware.** Three things a longer device run would settle:
  that the clock slot does not collide with the place name at a long
  "fine, coarse" pair; what the minute tick costs when a grayscale plane is
  pending and the window is promoted to a full HALF refresh (~1720 ms, per the
  `GfxRenderer` HAL note); and whether a once-a-minute refresh of the whole
  header band accumulates ghosting worth a periodic full clear.
- **Not verified against a reference clock.** The drift claim is the mechanism
  (crystal ppm between packets, re-anchored on every fix), not a measured
  figure.

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
suburb to its city (`mapbuilder/tilegen/build_config.json`'s `place_ranks` is a flat
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
- **Read off the code, not measured on hardware, 2026-08-13**: the
  `resolveBleBars()` fix above (0-renders-as-4-bars) traced through
  `pio run` and the source, not through a device screenshot -- see "A failed
  rssi() read must not draw full signal". What would settle it: a build with
  a logging shim that forces `ble_gap_conn_rssi()` to fail on demand, watched
  over `CMD:SCREENSHOT` through a connect / forced-fail / recovery cycle.

## The row freezes while the map shows a persisted fix -- found on hardware 2026-08-14

**Measured, not read off the code.** Reported as "BLE indicator is dead" with
the link demonstrably alive, and traced end to end in one session.

What happened, from the device's own serial log and a `CMD:SCREENSHOT` of the
same moment:

```
10:07:16  onEnter: mapHasLastFix=1
10:07:16  onEnter: rendering persisted fix 481305686,171078122
10:07:18  connected: interval 40 units (50 ms)      <- link came up AFTER the frame
10:07:19  command channel subscribed / transfer status subscribed / MTU now 256
```

The panel kept the "no link" X for minutes while `stats` answered
`rssi_dbm=-38` and both indication channels were subscribed. The framebuffer
itself held the X, so this is not a panel-refresh problem -- nothing redrew the
row.

**The mechanism is one flag doing two jobs.** `viewportDrawn_` is set from
`viewportDrawn_ = !showingPersistedFix_` (`src/activities/map/MapActivity.cpp`,
in the render path), which is correct for what that flag was built for: the
comment right above it says the persisted-fix frame "is deliberately not
followable". But `updateHeaderStatus()` opens with `if (!viewportDrawn_) return;`
-- so the same flag also gates the header row. With no live fix the row is
therefore drawn exactly once, by `onEnter`, and never again. The link comes up a
second or two later, so the X `onEnter` correctly drew is what stays.

Whether the phone is connected has nothing to do with whether the frame is
followable. The two uses were never meant to be the same test.

**Why it bites exactly when it matters.** No live fix is the case where a rider
most wants to know whether the phone is still there -- the map is stale
*because* something is wrong, and the one indicator that would say what is
frozen. Indoors, or with GPS not yet locked, this is the normal state, not an
edge case.

**Not caused by the RSSI change above.** T6.6 changed *what* is drawn when the
row repaints; this is about the row not repainting at all. Both were live in the
same build, which is why the first hypothesis on the day was T6.6 and why
`stats` reporting a real `rssi_dbm` was what ruled it out.

**Fixed, read off the code, not yet on the panel.** Split the two jobs: a new
member `headerRowDrawn_` (`src/activities/map/MapActivity.h`, next to
`viewportDrawn_`) is true whenever the frame on the panel carries a header row,
independent of whether that frame is followable. `updateHeaderStatus()`
(`MapActivity.cpp`) now opens with `if (!headerRowDrawn_) return;` instead of
testing `viewportDrawn_`.

`headerRowDrawn_` is set at every place `drawHeaderStatus()` runs or does not,
next to the existing `viewportDrawn_` assignment there:

- `renderViewport()` -- `true` unconditionally (persisted fix or not), because
  `drawHeaderStatus()` in that function runs unconditionally too. This is the
  line that fixes the bug: `viewportDrawn_` stays `!showingPersistedFix_` (its
  followability meaning is untouched), `headerRowDrawn_` does not follow it.
- `renderRouteOverview()`'s success path -- `true`. `drawHeaderStatus()` runs
  there as well, and the same freeze applies to the route overview screen as to
  the persisted-fix map: `viewportDrawn_` is `false` there (an overview has no
  marker to follow), so the header row would have frozen for as long as the
  overview stayed up, on the same mechanism as the bug above. Not separately
  reported by the maintainer -- found by tracing every `viewportDrawn_` reader
  while building this fix, not by a second hardware session.
- `renderWaiting()` and `onEnter()`'s state reset -- `false`. Neither draws a
  header row (`renderWaiting()` draws only the waiting text; `onEnter()` runs
  before any frame exists), so the row must stay ungated-off here exactly as
  before.

Every other `viewportDrawn_` reader was checked and left alone:
`panBy()` (`MapActivity.cpp`, guards on `source_` instead, by its own comment,
because panning cares about a frame existing, not about followability -- the
same distinction this fix draws, already correct there before this change),
the menu-close windowed repaint (restores a saved backdrop, touches neither
flag), and `applyFix()`'s `!viewportDrawn_ || !markerPatchValid_ || !source_`
check (this is the followability test the flag was built for -- untouched).

**Verified on the panel 2026-08-14, and the trap turned out not to exist.**
Flashed and reproduced deliberately: the map entered from a persisted fix
(`onEnter: rendering persisted fix 481306007,171078012`) with the link coming up
half a second later (`connected: interval 24 units` then `MTU now 256`). The row
flipped from the no-link X to signal bars on its own, with nothing touched.
Before this change it held the X for as long as the frame was up.

**The banner the trap was about does not exist.** Chased it while planning the
panel check: `showingPersistedFix_` has no drawing use anywhere
(`MapActivity.cpp` -- reset, set on entry, cleared when a real fix lands,
`fixChanged`, and `viewportDrawn_`, and nothing else), there is no
"last known fix" string in `lib/I18n/translations/english.yaml`, and the only
overlay on that frame is the debug readout, which is `mapDebugInfo`-gated and
sits *below* this row (see "The debug readout sits below this row"). So a
windowed header repaint has nothing to damage, which is why the panel check
found nothing to report.

**That leaves a stale comment, and it is load-bearing.** `MapActivity.cpp`, just
above `viewportDrawn_ = !showingPersistedFix_`, still says "The persisted-fix
frame carries a banner only a full redraw can clear, so it is deliberately not
followable". That banner is the stated justification for the assignment and it
is not in the code. The assignment may still be right for another reason -- a
persisted-fix frame plausibly should not be followable -- but its written reason
cites something that is gone. **Open:** find the real reason and rewrite the
comment, or find that the flag no longer needs to exclude persisted fixes at
all. Do not delete the comment without settling which.
