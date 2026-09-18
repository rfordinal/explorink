# The marker says how much it knows

The position marker answers two questions, and until 2026-09-05 it answered
both with total confidence whatever the fix was worth:

- **Where am I** -- the ring.
- **Which way do I face** -- the centre glyph.

Now each question has its own visible states, and neither borrows the other's
channel.

| | drawn |
|---|---|
| position, trusted | whole ring |
| position, loose | ring broken into 8 arcs |
| heading, good | today's hand (hike) or arrow (cycle/ride) |
| heading, coarse | outlined wedge, one heading step either side. Device receiver only -- the phone cannot honestly produce this state, see below |
| heading, unknown | nothing |

The two are independent. A loose position with a good heading draws a broken
ring with a sharp arrow in it, and that combination is real: they come from
different measurements.

## The bug this started from

`drawPositionMarker()` always drew a heading, and an unknown heading fell
through to step 0. `anchorHeading_` starts at 0. **A device that had never
been told a heading drew a hand pointing north**, and a rider standing still
with a receiver reporting noise got a confident arrow.

That is not a missing feature, it is a false statement. The sleep marker has
made the same argument since 2026-08-19: it drops the heading on purpose,
because a heading with nothing behind it is "a claim about the past dressed as
the present" (`MapMarkerMetrics.h`).

## Why not a thickness, and why not a circle in metres

Two obvious designs were rejected, both for reasons worth keeping written down.

**Accuracy as ring thickness.** A reader can see *whether* a ring is broken
with nothing to compare against. A reader cannot read a *value* off a stroke
width without a second stroke beside it, and there is no second ring on the
panel. Thickness is also already spoken for: `markerMetricsFor()` uses it for
the zoom rung, so a thick ring would mean two different things at once.

**Accuracy as a circle in ground metres**, the way a phone map draws it. This
is the semantically correct answer and it is blocked on one hard constraint:
the marker's patch box is fixed (`kMarkerBoxSize`, 64 px, buffer
`kMarkerPatchBytes` = 720 B). Anything drawn outside that box is not restored
when the marker moves, and it smears a trail across the map -- a correctness
bug, not a cosmetic one. At the finest rung a pixel is 1 m of ground, so a 100 m
accuracy would want a 200 px circle. Left **`[open]`**: it needs a decision
about the patch buffer, and that is its own pass.

The question "how far out could I be" is answered today by the scale bar plus
a ring that has stopped pretending. Not by a second circle.

## Where the numbers come from

**25 m, the position line.** The ring's radius is 27 px
(half of `layers.marker.ring_px` in `data/mapstyle.json`, 54 -- moved out of a
`kMarkerRingDiameter` constant on 2026-09-19, see `MapMarkerMetrics.h`) and the
finest rung draws 1 m per pixel
(`MapViewport::kZoomLadder[0]`). So a 27 m error is exactly the error that
still fits *inside the drawn marker* at the closest the device ever zooms --
the true position is somewhere under the glyph, and drawing the ring whole is
not a lie. Rounded down to 25.

The marker cannot express a precision finer than itself. That is the only
non-arbitrary place to put the line.

**The 1 m/px is ground metres, checked rather than assumed.**
`MapViewport::kZoomLadder` carries ground metres and `mppMercFor()` is where the
conversion to Mercator is paid -- at 48.5N one Mercator metre is 0.66 ground
metres (`MapViewport.h`, the comment above `mppMercFor`). Reading the ladder as
Mercator would have put this line at 17 m instead of 25, so the check is worth
keeping written down: the next reader gets the same doubt.

This makes `Trusted` generous for a phone, which reports 5 to 15 m under open
sky. Intended: **the broken ring is an alarm, not a quality meter.** It should
be quiet on a normal ride and fire in a street canyon or a tunnel mouth.

**18 m, coming back.** 7 m of hysteresis. Without it a fix hovering at the line
repaints the marker on every packet, and a repaint is a windowed refresh, which
costs the same ~500 ms as a full one whatever its area (measured on the X4
2026-08-05, `map-follow.md`). **On the T5 S3 Pro, today's reference board, the
same windowed refresh is ~1,081 ms** (`refresh-modes.md`, measured 2026-09-05),
so every argument in this doc about the cost of a repaint is stated at its
cheapest. That is a panel refresh per fix with the rider standing still.

**22.5 degrees, the heading line.** The render has 16 heading steps and nothing
finer (`MapHeading.h`). A glyph aimed at exactly one step therefore already
claims +-11.25 degrees, whether or not anybody decided to make that claim. So:

- agreed to within one step to the sharp glyph is honest
- within about three steps to only a neighbourhood is honest, draw the wedge
- wider to there is nothing to draw

**The wedge is +-22.5 degrees, not +-11.25.** A wedge one step wide would be
saying the same thing the sharp arrow says, only more vaguely, which is worse
than either. Its two edges use `kMarkerHeadingDir` at `step +- 1`, so both tips
land on the same circle the hike hand's tip does and the existing
`markerHandFitsAtEveryRung()` assert already bounds it.

## How it is drawn

**Broken ring:** draw the ring whole, then punch eight white squares on it.
`GfxRenderer::drawArc` only draws axis-aligned quarter circles, so a real
dashed circle would need chord sampling and per-frame trig; the punch needs
neither. The halo under the ring is already white, so a white square restores
background rather than cutting a hole into the map.

The gap positions are every other entry of `kMarkerHeadingDir` -- the same
16-step table the heading glyph indexes, so there is no trig at all. The gap
count is fixed at 8 and the gap *size* scales with the stroke
(`MarkerMetrics::ringGap`): fixing the count keeps the shape reading as a
broken ring at every rung, where fixing a pixel length would leave the coarse
rungs looking like a whole ring with a nick in it.

**Wedge:** two lines from the centre to the two step-neighbour directions, plus
the chord that closes them. Outlined, never filled -- the panel is 1-bit with no
alpha, and a filled wedge is a solid black fan over the map exactly where the
rider is trying to read what is around them.

**No heading:** hike keeps its dot, because the dot is position and not
direction. Cycle and Ride have no separate position glyph, so for them the ring
and halo are the whole marker.

## The seam

`MapFixTrust.h` is a source-neutral layer, the same shape and for the same
reason as `MapFollow::decide()` and `MapGnssHeading::stepFor()`: pure
arithmetic, host-tested, and reproducible by a second client (`CLAUDE.md`, "The
phone app must stay portable to iOS").

```
source                       MapFixTrust                 renderer
------                       -----------                 --------
BLE: accuracyM, flags 2-3 -> Trust{Pos,Dir} -> styleFor -> MarkerStyle
GNSS: hdop, quality, sats                                 {ringBroken, head}
console: pos ... acc dirq
```

`drawPositionMarker()` reads `MarkerStyle` and nothing else. It never sees
metres, an HDOP or a GGA quality digit, which is what keeps receiver vocabulary
out of the draw path.

`MapActivity` holds the `Trust` and the hysteresis `State`; both BLE ingest and
the console's `pos` resolve theirs before calling `applyFix()`, so a redraw
triggered by the fix already draws that fix's own claim.

## `Unstated` is a real state, and it must stay one

Four states in the model, three on the screen. **`Unstated` draws as today's
marker**: whole ring, sharp glyph.

This is the whole back-compatibility argument. A phone built before these bits
existed writes zeros, and zero decodes to `Unstated`. If zero meant "bad", every
phone still on an old build would suddenly make the device draw an alarm.

Said the other way: **a client that knows nothing about quality is not a client
reporting a fault.** Collapsing `Unstated` into `Unknown` would strip the arrow
off every existing phone; collapsing it into `Good` would be a claim nobody
made. It stays separate, and it draws the picture that was there before.

`accuracy` follows the same rule: 0 metres is physically impossible and the
encoder has always written 0 for a NaN, so 0 is the sentinel. An unstated
accuracy also **does not touch the hysteresis latch**, so a source that reports
accuracy on some fixes and not others does not drag the ring back and forth.

## The wire

The BLE packet is a fixed 21 bytes and a write of any other length is dropped
(`BlePositionServer.h`), so a new byte would break every client at once, in both
directions. Heading quality goes in two spare flag bits instead:

```
[16] flags  bit0 = off-route, bit1 = altitude present
            bits 2-3 = 0 unstated, 1 good, 2 coarse, 3 unknown
[17] accuracy  metres, saturating; 0 = no figure
```

`accuracy` was already crossing the link and being thrown away after a log line.
It is now the first real consumer of that byte.

## The phone sends two states, the device's own receiver sends three

**The phone never sends `coarse`, and that is correct rather than a gap.**

Its heading is not a reading, it is a **conclusion**. `HeadingTrend` returns one
only when a window of recent fixes covered real ground and its legs agreed with
the overall trend; anything less returns nothing. So a heading that exists on
the phone has already passed the phone's own test and has earned the sharp
arrow. There is no half-believed heading for it to report, and sending `coarse`
would be inventing a doubt the app does not hold.

An earlier cut of this had the phone grade its own trend by spread -- tight
means `good`, loose means `coarse`. That was wrong twice over: the spread is
already gated at 45 degrees before a trend is returned at all, so grading it
again second-guesses a decision the app has made, and it put a state on the
wire that the source cannot honestly distinguish.

`coarse` belongs to the **device's own receiver**, where a course is an
instantaneous NMEA reading rather than a conclusion and can genuinely be
half-trusted.

Android's `Location.getBearingAccuracyDegrees()` is also unused here, for a
different reason: the heading the phone sends is not the fix's bearing at all
(the phone rides in a backpack), so that figure describes a number the app does
not send.

### Staleness is the phone's only real question

When the window stops being a confident trend the app keeps the last bearing
rather than snapping to north. Right while the rider is briefly stopped, wrong
once they have been standing a while -- at that point there is effectively no
heading. `STALE_HEADING_MS` is where one becomes the other, currently **90 s**,
a first cut that has **not been judged on a ride**. It sits between an arrow
still pointing somewhere long after the rider parked and an arrow that vanishes
at every traffic light, each change costing a refresh the device pays in full --
~500 ms on an X4, ~1,081 ms on a T5 S3 Pro. The trend itself
disappears within about 5 s of stopping, so the timer runs from "stopped
moving", not from "stopped sending".

**The decay needs its own send reason to work at all.** Every other trigger in
the phone's send policy is driven by movement, and a parked phone sends nothing
until the hourly keepalive -- so the device would hold the last arrow for up to
an hour after the timer expired. `SendPolicy.Reason.HEADING_LOST` sends that one
packet. It fires at most once per stop, waits out the send floor like everything
else, and loses to `moved`, which carries the same new state anyway.

## GNSS: what this can and cannot catch

The device's own receiver fills the same struct, and the two halves are not
equally easy.

**Heading is already solved.** `MapGnssHeading::State::moving` is exactly
`Unknown` -- the speed gate is the thing that knows the course is noise.

**Accuracy is not, and HDOP alone will lie.** Measured indoors 2026-09-01 on
the T5 S3 Pro: quality 1, 8 satellites, HDOP 1.4 -- nominally about 7 m -- while
the reported course swung 206 to 252 to 21 to 148 degrees across a minute and the
speed reached 23.7 km/h on a device that was sitting on a desk. A rule of
`hdop × UERE` would have called that fix good.

So the GNSS mapping must take `quality` (GGA field 6 -- value 6 is dead
reckoning, with no satellites behind it) and `satsUsed` as well as `hdop`, and
even then it will be optimistic. **`[open]` -- it needs a ride under real sky.**

Which means the honest summary for a bad indoor fix is: the broken ring will
miss it, and the missing heading will catch it.

## A quality change alone has to repaint, and it did not

Found in the simulator 2026-09-05, before any of this reached a panel.

`MapFollow::decide()` answers `Skip` for a fix that lands under the move floor,
and `Skip` does not touch the panel. So a rider standing still -- whose heading
has gone stale, or whose fix has just degraded in a street canyon -- produced
fixes that updated `trust_` and changed nothing on screen. The marker kept
claiming what it claimed before, for as long as they stood there. Which is
exactly the case this whole feature exists for.

`Request::markerStyleChanged` fixes it: checked **inside** the move-floor
branch, so a fix that moves far enough is untouched (it already repaints and
picks up the new shape on the way) and a re-anchor is never downgraded to a
marker move. Only the fix that would have changed nothing is rescued, and it
gets its own reason, `TrustChanged`, so the log says why the panel moved.

`markerStyleDrawn_` records what the marker on the panel is claiming, recorded
where it is painted -- the same pattern as `markerBoxDrawn_` and for the same
reason: the answer has to come from the frame, not from live state that may
have moved on since.

This is the device-side twin of the phone's `HEADING_LOST` send reason. Both
are the same mistake caught twice: **the feature is about a rider who is not
moving, and every existing path is driven by movement.**

## What the simulator showed

`docs/images/marker-fix-trust/` holds the frames, 1:1, 80x80 crops of the
marker out of real 480x800 renders. Hike mode, Trnava, the local CDN mirror.

- `states-rung2-1to1.png` -- rung 2, ring 54 px. Five states left to right:
  whole+hand, broken+hand, broken+wedge, broken+none, whole+none. **All five
  are distinct and the broken ring reads as broken.**
- `states-rung6-1to1.png` -- rung 6, ring 33 px. whole+hand, broken+hand,
  broken+wedge, broken+none.
- `trust-change-repaint-1to1.png` -- two real BLE packets at the **same
  position**, the second with heading quality dropped to unknown. The second
  would have been a `Skip` before the fix above; the arrow is gone in the
  second frame.

Ink measured between those frames, which is what pins down that each shape is
actually being drawn rather than merely believed:

| difference | rung 2 | rung 6 |
|---|---|---|
| eight ring gaps | 157 px | 109 px |
| the hike hand | 73 px | 44 px |
| the wedge, against no heading at all | 43 px | **25 px** |

**The wedge at rung 6 is the weak one.** Its reach there is 14 px against a
dot radius of 5, so nine pixels of line per edge, and 25 px of ink total. It is
present and it differs, and whether it *reads* as a region rather than as a
smudge is a question for the glass.

An earlier run appeared to show the wedge not drawing at all -- `broken+wedge`
came out pixel-identical to `broken+none`. It was a measurement artifact, not a
bug: a console `pos` reply that nobody is listening for stalls the redraw for
3 seconds (`[BLEPOS] reply unconfirmed after 3000 ms`), so every timed
screenshot had captured the *previous* command's state. One simulator run per
state removes the coupling. Worth knowing before trusting any timed capture on
this path.

## What is verified and what is not

- **Verified on the host:** the arithmetic and every state transition,
  `test/map_fix_trust` (10 tests), plus the packing on the phone side.
- **Verified by compiler:** the wedge and the ring gaps stay inside the patch
  box (`markerHandFitsAtEveryRung`, `static_assert`).
- **Verified in the simulator 2026-09-05:** every state renders and all of them
  are distinct, at rung 2 and rung 6; a trust change on a stationary fix
  repaints. See the section above.
- **No host preview can show any of this.** `test/map_preview` and the webapp's
  `firmware` panel draw `MapRenderer::drawMarker()`, a deliberately mode-less
  puck for callers with no hike/cycle/ride distinction -- `MapActivity` does not
  call it, and `drawPositionMarker()` is not reachable from any host tool. So
  the shapes need the simulator or the device; there is no two-second preview
  loop for them the way there is for the map style.
- **Not verified on a panel:** how the shapes *read*. The simulator proves they
  are drawn and distinct; it cannot answer whether the eight gaps read as a
  broken ring rather than a damaged one, whether the wedge reads as a region at
  rung 6 (25 px of ink), or whether a hike marker with no heading is confusable
  with the sleep marker -- same ring-plus-dot shape, half the size. E-ink
  contrast and viewing distance decide all three, and an SDL window decides
  none of them.
- **Not verified on a ride:** the phone's 90 s staleness timer, and the whole
  GNSS accuracy mapping, which does not exist yet.

## Bench recipe

`pos` carries both claims, so the shapes can be walked through without a phone
and without a bad fix:

```
pos 48.1486 17.1077 heading 4 acc 5 dirq 1     whole ring, sharp arrow
pos 48.1486 17.1077 heading 4 acc 40 dirq 2    broken ring, wedge
pos 48.1486 17.1077 heading 4 acc 40 dirq 3    broken ring, no heading
pos 48.1486 17.1077 heading 4 acc 5            back to whole; dirq is sticky
```

Both are sticky like `heading` is, so a bench run sets a state once and then
walks a track through it, and `info` reports `acc_m` and `dirq` when they have
been set. The hysteresis latch is live on this path too, which is the part a
hand-drawn mock could not show.

**Security note:** `acc` and `dirq` add nothing an attacker did not already
have. `pos` injects a position over the same unauthenticated grammar on both
serial and BLE, and `info` already replies with the rider's latitude and
longitude -- see T-222. These two fields let someone claim a fix is better or
worse than it is, which is strictly less than being able to claim it is
somewhere else.
