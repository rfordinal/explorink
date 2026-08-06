# Riding a route: the frame is the problem, not the zoom

What the map screen does while a rider follows a planned route, why a
switchback road breaks it, and what fixes it. The route file and how it is drawn
are `route-layer.md`; the follow-or-redraw decision is `map-follow.md`. This file
is the layer above both: **which frame should be on the panel while a route is
being ridden, and how often it may change.**

Written 2026-08-06, corrected the same day against hardware, and **implemented and
verified on the panel 2026-08-07** (see the next section). Frame costs, re-anchor
counts and the overview's own choices are all measured on the device now. The
laptop simulation is kept where it predicts something no ride has covered, and says
so. One thing remains genuinely unmeasured -- panel ghosting -- and it cannot be
measured from a host at all; see the open list.

Three things in the first draft were wrong and are corrected in place:

- **A full reset does not cost 8.9 s on a rural pass.** It costs 0.8 to 2.3 s
  depending on the rung, so the argument for a per-leg frame is about picture
  stability, not about time. See "What a reset actually costs".
- **The serpentine's geometry was extracted with the tile y axis inverted**, so
  it sat 2.3 km north of the real road, in empty forest. The shape survived --
  see "Where the serpentine came from".
- Overwriting a `.tir` while the map holds it open silently stops the route
  drawing. Recorded in `route-layer.md`, "Do not overwrite a route the map has
  open".

> The question that started it: a 126 km loop opened as a route overview did not
> show the whole route, and the guess was that the coarsest zoom rung is too
> fine. The rung is indeed too fine for that route -- see "Nothing fits" in
> `route-layer.md` -- but that turned out to be the smaller half of the problem.

## There is no route mode

`MapActivity::applyFix()` (`MapActivity.cpp:977`) does not look at `route_` even
once. Grep it: zero references in the whole function. After the opening overview
frame the device is in exactly the same free-ride follow loop it uses with no
route at all, with the route drawn on top as one more layer
(`MapRenderer::render(..., route_.get(), ...)`, `MapActivity.cpp:1171`).

So the heading that orients the frame is the **fix's own heading**, handed
straight to `MapFollow::decide()` (`MapActivity.cpp:1014`, `:1017`). The planned
route is on the card, its shape is known in advance, and none of that reaches the
decision.

`MapFollow::decide()` re-anchors on three things (`MapFollow.h`):

| check | constant | value |
|---|---|---|
| heading drift | `kMaxHeadingDriftSteps` (`:34`) | 4 steps, 90 degrees |
| marker near the edge | `kKeepInMarginPx` (`:27`) | 80 px |
| ghosting budget | `kMaxPartialMoves` (`:45`) | 12 moves |

and skips a fix that moves the marker under `kMinMovePx` (`:40`), 8 px.

## Built and verified on the panel, 2026-08-07

The decision below is **implemented and measured on the X4**. What landed:

- `MapFollow::Request::routeHoldsFrame` turns the heading-drift check off, so with
  a route loaded only keep-in and the budget can re-anchor (`MapFollow.cpp`).
- `MapActivity::frameHeadingFor()` makes the frame's "up" the route's direction, so
  a re-anchor that does happen hands back the **same** orientation. Everything that
  must agree about up -- the projection, `MapViewState::heading`, the compass and
  `anchorHeading_` -- goes through that one answer.
- The overview's rung is kept as well as its heading. It used to be thrown away by
  the first fix: measured beforehand, a 3 m/px overview became 1 m/px on the next
  fix, one bend of the pass on screen.
- `kRouteFramePartialMoves` (160) is the budget while a route holds the frame.
- `CMD:GOTO_MAP <path>` takes an optional route path, which is what makes any of
  this testable from a laptop -- see `route-layer.md`.

**The gate**, three runs of the same 136 positions along the 3.4 km Baba leg at
25 m spacing, same firmware, on hardware:

| run | full redraws | of those, caused by heading | distinct frame orientations |
|---|---|---|---|
| route loaded, rung the fit chose | **0** | **0** | **1** |
| route loaded, forced to 1 m/px | 6 (all keep-in) | **0** | **1** |
| **no route** (the control) | 11 (8 budget, 3 heading) | 3 | 9 |

The route tangent went through 11 of the 16 heading steps in every run, so the
rider really was turning. Heap drift across a whole ride: **0 bytes**.

The middle row is the one worth reading twice. Forced to a rung the leg does not
fit, the marker runs out of frame six times and gets six new frames -- and every
one of them is the same way up. The decision is not "never redraw"; it is "never
redraw because the rider turned, and never hand back a differently-oriented map".

Panel shots of a whole traverse, one frame with the marker walking across it, are
`build/analysis/D9-po-zmene-jeden-frame.png` in the parent repo (gitignored,
regenerate it).

## The decision: while a leg is on screen, it does not get redrawn

**Decided by the maintainer 2026-08-06**, after watching the traverse on the panel:

> In route mode the frame is not redrawn at all while the leg is on screen. A
> heading change is not a reason to change what the rider is looking at. The rider
> is following the route; the only reason to draw a new frame is that they are
> about to run out of the one they have.

So the rule is **keep-in only**. Heading drift stops being a re-anchor reason the
moment a route is loaded, and the ghosting budget has to be made big enough to
cover a leg rather than being allowed to interrupt one.

This is safe because the heading is already shown somewhere better.
`drawPositionMarker()` gets `MapFollow::relativeHeadingStep(headingStep,
anchorHeading_)` (`MapActivity.cpp:937`), so in ride and cycle mode the marker is an
arrow drawn **relative to the frame** -- a rider who turns 90 degrees gets an arrow
90 degrees off up, inside a map that has not moved. That mechanism exists, is wired,
and today only gets to work until the drift limit fires and rotates the whole
picture out from under it. Removing the drift re-anchor is what lets it do the job
it was built for. (Hike mode draws a plain dot with no arrow, deliberately --
`MapActivity.cpp:187`. Hike is not the case this rule is aimed at, but it means the
frozen frame carries no heading at all there.)

### What that costs, and the one number it turns on

**Simulated**, drift disabled, fixes every 25 m as the phone sends them, over the
3.4 km Baba leg. "Extra" is redraws after the first frame -- the number the decision
wants at zero:

| rung | budget 12 | budget 30 | budget 67 |
|---|---|---|---|
| 1 (3 m/px) | +6 | +2 | +1 |
| **2 (6 m/px)** | +4 | +1 | **0** |
| 3 (12 m/px) | +2 | **0** | **0** |

Keep-in fires exactly once across that whole table, at rung 1 on the shipped
budget. It is not the binding constraint, because a per-leg fit picks a rung the leg
already fits inside -- which is the same reason the decision can afford to keep
keep-in as the only trigger.

**Everything therefore turns on the ghosting budget, and the number needed is
knowable in advance.** A leg costs one marker move per `kMinMovePx` of screen
travel, so:

```
moves per leg ~= leg length / (kMinMovePx * metres-per-pixel)
```

3.4 km at 6 m/px is 3400 / (8 x 6) = 71 moves; at 12 m/px it is 35. The simulated
budgets that reach zero -- 67 and 30 -- sit just under those, because the marker's
path across the frame is not a straight line up it.

That formula is the design rule this decision needs: **the rung and the ghosting
budget are one choice, not two.** If the panel turns out to tolerate only 30
windowed refreshes before ghosting makes it unreadable, then a 3.4 km leg has to be
drawn at 12 m/px, not 6, and the fit can work that out before it draws anything. So
the open measurement is no longer "is 67 all right" but "what is the largest budget
the panel holds" -- and whatever it answers, the fit can pick a rung to match.

## A switchback road turns further than the drift limit, and no tangent fixes it

**Measured off the tiles**, `mapbuilder/tools/route_follow_sim.py --curvature`
against the 503 over Pezinská Baba, pulled straight out of the road layer
(`CLASS_SECONDARY`, 3.40 km of road, 1554 m of net displacement, tortuosity
2.19, vertices 28 m apart):

| window | max bearing change | mean | windows past the 90 degree drift limit |
|---|---|---|---|
| 100 m | 158.0 | 36.5 | 6.8 % |
| 200 m | **179.8** | 57.1 | 27.3 % |
| 300 m | 176.1 | 68.9 | 26.6 % |
| 500 m | 179.7 | 81.1 | 41.4 % |
| 1000 m | 179.6 | 99.6 | 54.2 % |

Read the mean column downward: **it grows with the window.** A longer horizon
does not smooth a switchback road out, because the road genuinely reverses. So
"take the route's tangent further ahead" has a floor set by the road, not by the
parameter.

### The measurement that looked fine and was wrong

A first pass at this counted turns sharper than 60 degrees **between adjacent
polyline vertices** and concluded that no paved road near Baba has a sharp turn
at all -- 61 roads scanned, zero hits. That was an artefact of vertex spacing, not
a fact about roads. At 28 m spacing a 180 degree hairpin is spread over seven
vertices at about 26 degrees each, and every per-vertex threshold misses it.

**Curvature has to be measured over a distance window, never per vertex.** The
route file's vertex spacing is whatever the builder happened to emit -- 28 m
straight off OSM, 89 m after `build_route.py`'s default 5 m simplification, 103 m
on the Záhorie loop -- so any per-vertex metric measures the builder instead of
the road. `Route.heading_change_over()` in the sim tool is the shape that does
not have this bug.

## What the device draws on that serpentine today

**Simulated**, `route_follow_sim.py`, rung 4 (20 m/px), the route's own tangent
300 m ahead as the heading, `kMaxPartialMoves` at its shipped 12:

| # | km | reason | heading step |
|---|---|---|---|
| 00 | 0.00 | start | 12 |
| 01 | 0.80 | drift | 0 |
| 02 | 1.55 | drift | 4 |
| 03 | 2.05 | drift | 0 |
| 04 | 2.15 | drift | **12** |

Two things about that table are worse than the count:

- **Frame 00 and frame 04 are the same orientation.** The rider paid four full
  redraws over 2.15 km to arrive back at the picture they started with. The map
  turns 90 degrees, turns back, turns again.
- **Frame 03 to frame 04 is 100 metres**, about 9 seconds at 40 km/h. Every one of
  those redraws is a whole-panel waveform: the screen blanks and rebuilds. Four of
  them inside two kilometres, two of them nine seconds apart -- and the heading
  goes 12, 0, 4, 0, 12, so three of those four are the map swinging back to an
  orientation it already had.

Rungs 0 to 3 are worse, not better: 12, 8, 6 and 5 frames over the same 3.4 km.

**The complaint here is picture stability, not time.** The first draft of this
file argued the redraws eat the ride, on the 8.9 s reset from `map-follow.md`.
They do not -- see the next section, where the same reset measures around a
second. What they do is take a map that was correct and readable and replace it,
twice a minute, with the same map rotated 90 degrees. That is worth avoiding
because of what the rider sees, not because of what it costs.

## What a reset actually costs, and it is not 8.9 seconds

**Measured on the X4, 2026-08-06**, standing on this pass with the route loaded.
`framebuffer ready in N ms` off the device's own log, two runs per rung agreeing
within 2 %. The waveform is a fixed ~500 ms whatever the frame
(`map-follow.md`, "The refresh"), so the third column is the honest full-reset
cost:

| rung | ways | bytes read | framebuffer | full reset |
|---|---|---|---|---|
| 0 (1 m/px) | -- | -- | 313 ms | ~0.81 s |
| 1 (3 m/px) | 186 | 113 kB | 536 ms | ~1.04 s |
| **2 (6 m/px)** | 563 | 51 kB | **400 ms** | **~0.90 s** |
| 3 (12 m/px) | 2 083 | 309 kB | 1 133 ms | ~1.63 s |
| 4 (20 m/px) | 4 443 | 433 kB | 1 750 ms | ~2.25 s |

Rung 1 costs more than rung 2 despite drawing a third of the ways: rung 1 reads the
z13 detail LOD, 113 kB of it, while rung 2 reads z12 where the same ground is
already simplified into 51 kB. Bytes off the card, not ways drawn, is what the
finer rungs pay for.

`map-follow.md`'s 8.9 s is a **Bratislava** number -- 22 904 ways and 1.57 MB at
rung 2. On a forested pass the same rung reads 51 kB and 563 ways, and the reset
is **ten times cheaper**. Cost per reset is a property of the terrain, not of the
device, and quoting one number for it was the first draft's mistake.

Two consequences, and both of them matter more than the correction itself:

- **The waveform is now the bulk of the cost**, 500 ms of 900. Nothing in the
  framebuffer path can be optimised into a cheaper redraw here; the only way to
  pay less is to redraw less often. That is the same conclusion as before, reached
  for the opposite reason.
- **A coarser rung buys fewer redraws, but only down to a floor, and the total
  turns round before it gets there.** Zooming out genuinely does reduce the count:
  the marker crosses fewer pixels per metre, so the ghosting budget lasts longer.
  But rung 4's frame costs 4.4x rung 2's, because a coarser rung covers more ground
  and therefore more geometry, so the two effects fight. Where they settle is the
  next section.

### Where zooming out stops helping, and why it is not the keep-in margin

Frame counts simulated, cost per frame measured, so the total is the product of the
two. Track-up with a 300 m tangent, the shipped budget, 3.4 km of serpentine:

| rung | frames | drift | keep-in | ghost | per frame | **total** |
|---|---|---|---|---|---|---|
| 0 (1 m/px) | 12 | 4 | 0 | 7 | 0.81 s | 9.8 s |
| 1 (3 m/px) | 8 | 2 | 0 | 5 | 1.04 s | 8.3 s |
| **2 (6 m/px)** | 6 | 4 | 0 | 1 | 0.90 s | **5.4 s** |
| 3 (12 m/px) | 5 | 4 | 0 | 0 | 1.63 s | 8.2 s |
| 4 (20 m/px) | 5 | 4 | 0 | 0 | 2.25 s | 11.2 s |

**The count really does fall as the rung coarsens** -- 12, 8, 6, 5, 5 -- and the
reason is exactly the one to expect: a coarser rung moves the marker fewer pixels
per metre, fewer moves accumulate, and the ghosting budget lasts longer. Ghost
redraws go 7, 5, 1, 0, 0. That mechanism works.

**It stops at a floor of four drift redraws that zoom cannot touch.** The road turns
90 degrees whatever the scale; drift is scale-invariant. So past rung 3 the count is
flat while the cost per frame keeps climbing, and the total has a minimum in the
middle. Rung 4 -- the rung the follow loop would sit on, and the one "zoom out to
refresh less" argues for -- is the **worst** of the five, twice rung 2's total.

**And keep-in never fires. Not once, at any rung.** That matters more than the
arithmetic, because "one expensive frame, then the marker just travels for a long
time" is a description of the keep-in mechanism, and on this road the frame is never
ended by the marker reaching the edge. It is ended by the road turning or by the
ghosting budget. Zooming out to give the marker more room to travel is buying
headroom in the one constraint that is not binding -- the same thing the Bratislava
ride found ("The keep-in frame never fired", `map-follow.md`), now on a road with a
completely different shape.

## Why the heading has to be frozen and not merely smoothed

The decision above says the frame does not turn. This section is why nothing short
of that works, which is worth keeping because "look further ahead" is the obvious
first idea and it is wrong.

**Simulated**, so read the frame counts and ignore any timing: same serpentine,
25 m fix spacing, the route's tangent as the heading unless stated.

| frame orientation | frames over 3.4 km |
|---|---|
| tangent 150 m ahead | 7 |
| tangent 300 m ahead | 5 |
| tangent 1200 m ahead | 3 |
| tangent 2000 m ahead | 2 |
| frozen to the leg's net direction | 2 |
| frozen, and `kMaxPartialMoves` at 67 | **1** |

At rung 2's measured ~0.90 s that is 0.9 s of redrawing for the whole traverse
against 4.5, and one whole-panel flash against five.

A 2000 m tangent already equals the frozen frame, which is the useful part:
**a serpentine does not have to be detected.** A long enough horizon handles it
with one parameter and no mode.

The second row of the fix is the ghosting budget. 12 moves at 8 px is 96 px, and
the marker has 540 px of room in front of it (`kMarkerLadder` step 2 puts it at
y=600, `MapViewport.h:102`; `kKeepInMarginPx` stops it at y=80). **The shipped
budget spends 18 % of the screen the marker was given.** Letting it cross the
whole screen needs 67 moves. Whether the panel tolerates 67 windowed refreshes
before ghosting makes it unreadable is **not known** -- `MapFollow.h:42-45` and
`map-follow.md` both already say the number is untuned, and this is the
measurement that settles it.

## And the zoom goes the other way

The premise this started from -- zoom out further -- is backwards for a pass.

Frozen to its own net direction, the whole serpentine occupies 650 x 1554 m, so
it fits inside the usable 424 x 744 px at **every rung on the ladder**:

| rung | serpentine on screen | between direction reversals |
|---|---|---|
| 1 m/px | 650 x 1554 px (does not fit) | 610 px |
| 3 m/px | 217 x 518 px | 203 px |
| **6 m/px** | **108 x 259 px** | **102 px** |
| 12 m/px | 54 x 129 px | 51 px |
| 20 m/px | 32 x 78 px | 31 px |

Reversal spacing measured along the road: 6 reversals, median 610 m apart (min
190, max 1030).

**6 m/px wins every count at once**, which is the part worth keeping. It is the
cheapest frame measured (400 ms against rung 4's 1 750), the cheapest *total* over
the traverse (5.4 s against rung 4's 11.2, see the table above), a frame the whole
traverse fits inside, and one where the switchbacks are 102 px apart and plainly
legible. At 20 m/px the same traverse is a 32 px smudge in a screenful of stream
texture, for twice the total time -- and 20 m/px is what the ordinary follow frame
would be sitting on.

That four separate measures -- frame cost, total cost, fit, legibility -- all point
at the same rung is worth more than any one of them. It also means the rung does
not have to be argued over: **the leg's own extent picks it**, and on this leg the
extent picks 3 m/px, one finer still, with 203 px between switchbacks.

Renders of both, through the firmware's own `MapRenderer` compiled for the host,
are what this was judged on. They are device output for what that renderer
covers; `../../docs/device-preview.md` in the parent repo lists what it does not
(status text, compass, the mode-aware marker).

### The overview already picks the right frame, on its own

**Verified on the panel, 2026-08-06.** The route was picked from the device's own
picker and the overview frame photographed off the X4
(`build/analysis/D6-zariadenie-serpentina-opravena.png` in the parent repo, which
is gitignored -- regenerate it rather than looking for it in a commit).

`MapRouteFit` chose **heading 13 and zoom step 1** for this leg. Heading 13 is the
leg's net direction to the step, the same value the laptop computes as "frozen to
the leg", and step 1 is a rung the whole thing fits on. It also drew the route
sitting exactly on the 503's own casing, which is how the mirrored-geometry bug
below was caught.

So the frame this file argues for is **not a new mechanism**. `renderRouteOverview()`
already produces it; what is missing is that riding throws it away and returns to
the follow loop on whatever rung was persisted -- measured here as step 0, one bend
of the pass on screen.

## What building it would need

The shape of the fix is **one frame per leg**, oriented and scaled to that leg,
with the marker crossing it. **Confirmed with the maintainer 2026-08-06** as the
intended model, in those words: one picture for the whole traverse instead of five,
and the rung chosen so the leg's extent -- switchbacks included -- fits inside it.

Three parts:

1. **Leg boundaries.** A leg ends where the rider has to decide something -- a
   junction where the route leaves one road for another. A serpentine has none,
   so it is one leg. `.tir` carries no such thing: the format is magic, version,
   name, count, bbox, epoch, two crcs, then bare points
   (`../../docs/route-file-spec.md`). This is a v2 field computed by mapbuilder,
   which has the OSM data to find junctions. `MapRouteReader::kFormatVersion`
   (`MapRouteReader.h:66`) is checked for exact equality, so the reader is bumped
   with it.
2. **The frame itself is already written.** `renderRouteOverview()`
   (`MapActivity.cpp:1041`) anchors at the screen centre rather than the marker
   ladder, picks its own rung and heading from `MapRouteFit`, and draws no
   marker. A per-leg overview is that function with the fit run over one leg
   instead of the whole route, plus a marker. Not a new renderer.

   **The rung has to come from the leg's own extent, and measuring that extent as
   a north-up bbox costs a rung.** For this serpentine, all three ways of choosing
   the frame:

   | how the extent is measured | extent | rung it needs |
   |---|---|---|
   | axis-aligned bbox, north up | 1455 x 812 m | 6 m/px |
   | point set, best of the 16 headings | 1151 x 1133 m | 3 m/px, heading 3 |
   | point set, the leg's net direction | 650 x 1554 m | 3 m/px, heading 13 |

   The panel is 480x800 -- tall and narrow. A pass oriented along its own
   direction of travel is tall and narrow too and drops into that shape; the same
   shape held north-up is wide and short, fights the panel, and has to zoom out one
   rung to fit. This is `MapRouteFit.h`'s "Why the bbox is the wrong thing to
   measure", and a leg is where it bites hardest, because a leg is far more likely
   than a whole route to be one long diagonal strip.

   Rows two and three land on the **same rung**, and that is the other half of the
   existing design earning its keep: a rung is a rung, so the tilt row two picks by
   arithmetic buys nothing, and `MapRouteFit` already breaks that tie on the
   direction of travel (`route-layer.md`, "The tie-break is the direction of travel").
   Row three is what the device actually chose on the panel.
3. **Matching the rider to the leg.** This is the part with a real cost, and it
   collides with the reason follow mode exists. Matching has to run on **every**
   fix, and the cheap paths deliberately touch no file at all -- `applyFix()`
   does no route I/O, and `MoveMarker` never renders. `MapRouteReader` has one
   seek cursor and one 1 KB buffer (`MapRouteReader.h:132-137`) already shared
   with the render pass, and `MapRouteSource.h:30-32` says outright that one
   cursor cannot serve two readers. So it is a second reader and a second open
   file, or a fixed RAM window of upcoming points with a refill policy. The
   window keeps the O(1)-in-route-length rule and is the cheaper of the two;
   it also brings a new failure mode, a cursor that has lost the rider.

Match to a **segment**, not to a vertex, whichever is built. Spacing runs 5 m to
890 m on a routed file, and at the wide end the nearest vertex means nothing.

## Nothing on the map says which village this is

Found while looking at the frames, and **left for later by the maintainer** --
recorded here because the frames make it look like a rendering bug and it is not.

The tile carries place names and the source reads them: `MapPlaceRef::name`
(`IMapSource.h:47`), filled by `MapTileSource.cpp:412` and `:425` through
`readPlaceName()`. `MapRenderer` then draws a dot and drops the name
(`MapRenderer.cpp:259-266`) -- it contains no `drawText` call at all. Every
`drawText` in `MapActivity` is the compass letter, the debug line, a banner or
the route's name (`MapActivity.cpp:342`, `:382`, `:864`, `:877`, `:884`).

A village is therefore a grey built-up wash with no label at any rung. The parent
repo's `docs/map-data-spec.md` planned exactly this ("a separate trip-overview
mode that draws only the route and place names") and budgets RAM for label
layout; it was never built.

## Open, and what would settle it

- **What the largest usable ghosting budget is. `kRouteFramePartialMoves` is 160
  on no evidence at all.** It is enough to cover the Baba leg at 3 m/px, which is
  why the gate shows zero redraws; whether 160 windowed refreshes in a row leave a
  readable panel is unknown.

  **And it cannot be measured from a host.** `CMD:SCREENSHOT` dumps the
  framebuffer, which is clean by construction -- ghosting is what the panel does to
  a frame, not what the frame contains, so every screenshot of a badly ghosted
  panel looks perfect. The only instrument is a person looking at the device, or a
  camera pointed at it. That is worth knowing before anyone plans to gate this in
  CI.

  What to do: ride a leg with the value at 12, 40, 80 and 160 (one constant,
  `MapFollow.h`), and **look at the panel at the end of each** -- the quantity is
  accumulated ghosting after N windowed refreshes with no clean frame between them,
  so a glance part way through measures nothing. If 160 smears, lower it; the
  moves-per-leg formula then says which rung a leg of a given length has to be
  drawn at, and the mechanism does not change.
- **A leg longer than the budget can hold has no good answer yet.** The formula
  gives a coarser rung, and coarsening has a floor: past some length the leg does not
  fit the panel at any rung (`route-layer.md`, "When nothing fits") and the frame has
  to break mid-leg after all. Where that boundary lands, and whether the break should
  happen at a chosen place rather than wherever the budget runs out, is undecided.
- **Every re-anchor count here is simulated, and the simulation is optimistic.**
  It walks the route exactly, one fix every 25 m. Real GPS sits off the
  polyline, which adds drift and keep-in redraws the sim never sees. The frame
  *costs* are now measured on the panel; the *counts* are not, and no ride has
  been replayed against a device with a route loaded.
- **The measured costs are one pass, standing still.** Four rungs, two runs each,
  one place on one forested pass. That is enough to retire the single 8.9 s figure
  and to show the cost is terrain-shaped, and not enough to predict any other
  route. Anything that turns on cost per reset should be measured where it will
  run.
- **The sim anchors on the marker ladder, not the leg's centre.** So its frozen
  runs still show keep-in redraws that a real per-leg overview would not have --
  one at 3 m/px. The frozen numbers are pessimistic by that much.
- **Whether a rider wants a whole leg or the road ahead.** A per-leg frame is an
  overview: it shows where the pass goes, not what is 200 m ahead. That is the
  right trade in a serpentine with no junctions and probably the wrong one on a
  fast road with turnoffs. Unresolved, and it is a judgement about riding rather
  than something a measurement answers.
- **Leg boundaries are unspecified.** "A junction where the route leaves one
  road for another" is a sentence, not a rule. Whether a leg splits at every such
  junction, or only where a road changes name or class, decides how often the
  frame changes on ordinary roads -- where, unlike a pass, most junctions are
  passed straight through.

## Reproducing any of this

`mapbuilder/tools/route_follow_sim.py` in the parent repo, which carries the
constants above as copies -- there is no Python binding to `MapFollow.h`, so a
change here silently invalidates it.

```
python3 mapbuilder/tools/route_follow_sim.py build/trips/baba-serp.tir --curvature
python3 mapbuilder/tools/route_follow_sim.py build/trips/baba-serp.tir --sweep --speed 40
python3 mapbuilder/tools/route_follow_sim.py build/trips/baba-serp.tir --zoom 2 --freeze --budget 67
python3 mapbuilder/tools/route_follow_sim.py build/trips/baba-serp.tir --zoom 4 --render \
        --tiles build/tiles --out build/analysis
```

`--render` shells out to the host-built `map_preview`, so the frames are the
firmware's own renderer. Building it regenerates `MapStyleDefaults.h` from
`data/mapstyle.json`, which means an invalid style in the working tree stops the
build -- build in a clean worktree when that happens rather than touching a style
someone else is editing.

## Where the serpentine came from, and the axis that got it wrong

The route file was not planned by hand. It is the most switchbacked
`CLASS_SECONDARY` way in the tiles around Baba, lifted out of the road layer with
`tile_reader.py` and packed with `route_file.py`. That was deliberate: a route
asked for through `build_route.py --via` went round the serpentine instead of over
it, and one simplified at the default 5 m tolerance had lost enough curvature to
change the answer. **Take the geometry from the tiles when the road's shape is the
thing under test.**

Doing that has one trap, and the first version of this file fell into it.

**Tile-local `y` grows south.** A record's coordinates are offsets from the tile
origin, and the vertical one is inverted relative to Mercator, so the way back is
`origin_y - y`:

```python
# right -- render_from_tiles.py:35-39 spells out why
pts = [(tile.origin_x + x, tile.origin_y - y) for x, y in way.points]
```

With `+ y` the geometry comes out **reflected about the tile's origin latitude**.
The extracted serpentine landed 2.3 km north of the real 503, at 48.3788-48.3861
instead of 48.3557-48.3629 -- in empty forest, on no road at all. It took the
device to catch it: the panel drew the route with nothing underneath, and "why is
the rest of the road not rendered" has no answer except that there is no road
there.

**What survived the mirror, and why.** A reflection is an isometry, so every shape
metric this file rests on is unchanged: 3 397 m against 3 399 m of road,
tortuosity 2.19 both ways, and identical bearing-change magnitudes -- a mirror
flips the sign of a turn, not its size. So the curvature table, the re-anchor
counts and the frozen-frame conclusion all stand as statements about a real road's
shape. What was wrong was the claim that the shape sat on the 503 at those
coordinates, and only that.

The lesson is narrower than "check your axes": **a geometry bug that preserves
shape is invisible to every shape-based check.** Length, tortuosity and curvature
all agreed with the road. Only putting it on the map next to the road layer showed
it, which is the argument for rendering a thing rather than tabulating it.
