# The map screen's scale bar

Bottom-left of the map, above `GUI.drawButtonHints()`'s band: a five-segment
alternating black/white bar with tick marks and a rounded ground distance.
Drawn by `MapActivity::drawMapScale()` (`src/activities/map/MapActivity.cpp`).

Called from both `renderViewport()` and `renderRouteOverview()`, right after
`drawHeaderStatus()` -- same "screen furniture, drawn last, on top of the map"
rule the compass and header follow (`MapActivity.cpp`, the comment above
`drawCompass()`'s call sites).

## Geometry

Read off `MapViewport::kZoomLadder[zoomStep()].mpp` (ground metres per pixel
for the current zoom rung). Bottom edge is pinned to the same clearance line
`kBusyMarginBottom` uses (`MapActivity.cpp`'s `kScaleMarginBottom = 50`) so
both the busy badge (bottom-right) and the scale bar (bottom-left) clear
`GUI.drawButtonHints()`'s 40px band by the same 10px margin. Stacked
bottom-up from that line: label text, then a 2px gap, then the ticks (which
also span the bar), then the bar itself on top.

## The nice-number rounding

`niceScaleValue(raw)` picks the largest value of the form `{1, 2, 5} x 10^k`
that does not exceed `raw` -- the same 1-2-5 sequence every printed map scale
uses, so the bar's marks are numbers a rider can subtract in their head
(`MapActivity.cpp`). `raw` is `kScaleTargetPx * mpp`: the ground distance the
bar would span if it filled its full target width (130 px) exactly. The bar
usually ends up narrower than that target, never wider -- it shows the
nearest round number under the cap, not the cap itself.

**Why 5 segments, not 4.** Divided by 5, every nice value from the ladder's
practical range (raw >= 100m at the tightest zoom rung, `mpp=1.0` from
`MapViewport::kZoomLadder`) lands on a whole number: `100/5=20`, `500/5=100`,
`1000/5=200`, and so on through the ladder's loosest rung (`mpp=20.0`,
raw up to a few km). Divided by 4 it does not: `500/4=125`. Verified by
hand for every nice value the zoom ladder's mpp range and the 130px target
can produce; not verified for a target width or ladder outside today's five
rungs.

**Unit choice.** Meters below 1000, kilometres from 1000 up -- `useKm =
niceMeters >= 1000.0`. Because the nice-value ladder's km-range entries
(1000, 2000, 5000, 10000, 20000) divide by 5 into whole kilometres except the
two smallest (1 km -> 0.2 km steps, 2 km -> 0.4 km steps), `formatScaleMark()`
prints one decimal place only when the mark is not a whole number.

## Label collision -- found on hardware, 2026-08-11

First flash, `CMD:SCREENSHOT` at a loose zoom rung (mpp=20, tile z11): the
five interior marks rendered as one unreadable smear -- adjacent numbers
wider than the gap between their ticks, something no host-side arithmetic
check caught because it never measured actual glyph widths against actual
segment spacing at every zoom rung.

Fixed by building every mark's text and measuring its width *before* drawing
any of them: 0 and the final mark (the two numbers that matter) always draw;
each interior mark draws only if it clears both its already-drawn left
neighbour and the final label's own left edge, by `kLabelGap` (3px) on each
side. A mark that would collide keeps its tick but drops its number, rather
than overlapping the next one.

**Verified on hardware, 2026-08-11**, `CMD:SCREENSHOT` via
`tools/screenshot_gate.py`: at mpp=20 (the exact rung that broke), the bar
now shows a clean "0 ... 1 km" with the three interior marks correctly
dropped -- no overlap, ticks intact. Not yet checked at every other zoom
rung; the collision math is general (it does not special-case mpp=20), but
only one rung has actually been looked at on the glass.

The grab that verified this rendered the device's persisted GPS fix, which
named a real neighbourhood -- removed from the repo rather than kept as a
reference shot (privacy, not a rendering concern).

## Read-off-the-code vs measured on hardware

Geometry and rounding: read off `MapActivity.cpp` and a clean `pio run`
(RAM 17.4%, Flash 59.0%, 2026-08-11). Label collision handling and the bar's
general on-panel legibility: measured on hardware, 2026-08-11, at one zoom
rung (see above). **Not yet checked**: every other rung on the ladder, and
whether the tick lines read cleanly against the alternating segments at 1x
in bright daylight rather than an indoor screenshot.
