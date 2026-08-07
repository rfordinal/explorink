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
