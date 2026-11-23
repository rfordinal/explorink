# The satellite wait: a screen in front of the map

**Status: on hardware 2026-09-10, seven flashes and thirteen panel reads**,
which is where most of the design below comes from. The plot has drawn satellites
and been judged: the mark sizes, the white halo where a mark lands in the ridge,
and sixteen marks at once without crowding.

**Against a synthetic sky** (`GnssFakeSky.h`), because a real one never arrived:
indoors the receiver locates nothing, and the day it went outside the weather
gave nothing stable. **A real acquisition is the one thing still open** -- see the
bottom.

On a board with a receiver, opening the map used to mean opening the map and
finding out. This puts a screen in between: the sky as the receiver sees it,
with two ways out.

## Why it exists

The map already had a waiting state, `STR_MAP_WAITING_GNSS` over an empty panel.
It answers none of the questions a rider actually has while it is up.

The numbers are the argument. A ride on 2026-09-01 took **526 s to first fix**
(`gnss.md`, "The map reads it"). A 15-minute walk on 2026-09-04 got **no fix at
all** while the receiver tracked one satellite the whole time. A banner cannot
tell those two apart, and they call for opposite actions: wait, or go and stand
somewhere with sky.

So the screen shows what the receiver hears, and the rider decides. That is the
thesis position too -- the device answers *where am I and what is around me*, so
"why do I not know where I am yet" is a question it owes an answer to, and
"searching..." is not one.

## What is on it

Top to bottom:

- **The title and its subtitle**, and nothing else at the top. No logo and no
  wordmark: the device does not need to introduce itself on a screen the rider
  reached by pressing Explore on it, and every pixel spent on branding is a pixel
  of sky.

  It carried the home screen's header art for one round and that was a mistake
  worth writing down. **The art has mountains in it, so the panel showed a
  mountain range above the sky and another below it** -- the sky read as being
  underground. The horizon has to be the lowest thing in the picture.
- **The elapsed wait**, directly under the subtitle and in the second-largest
  type on the screen. It is the number the rider is actually watching; it spent
  one round buried at the bottom of the readout.
- **The countdown**, when the wait has a limit: `Opens the map on its own in
  4:12`.
- **The sky**, as a panorama: azimuth left to right, elevation up from the
  horizon. South at both ends, north in the middle. One mark per satellite the
  receiver has located, filled when it is being heard and an outline when it is
  not.
- **The horizon**: the mountain line art, running off both edges and cut off at
  the bottom.
- **Cardinal labels with a tick between each pair** -- S | W | N | E | S -- so
  "which way do I move" has an answer and the row reads as a scale rather than as
  five loose letters. Bare letters, not translated, the same choice the map's
  compass makes for its `N`.
- **The readout**, three lines and three sizes: the state in one sentence, the
  best signal with a five-slot meter, and one line of advice.
- **Two action rows**, and a Back that goes home.

### Why a panorama and not a skyplot

A GNSS skyplot is conventionally a circle seen from above. That is the right
picture for checking geometry and the wrong one for a rider, because the thing
in front of them is a horizon. **North-up-on-a-circle answers "which
satellites"; a panorama answers "which way is the sky open", which is the only
action available to someone waiting for a fix.**

It is also cheaper: two divisions per satellite, no trigonometry
(`src/activities/map/GnssSkyView.h`).

### The horizon is the asset, and so is the geometry

The ridge is `src/images/mountains.svg` baked by `scripts/gen_mountains.py`, and
it states the physical fact behind a slow fix: a satellite low in the sky is
behind terrain. A mark that sits inside the ridge is one the rider should not
expect help from, and it is drawn with a **white halo** rubbing out the ground
behind it -- the art is line work, so a white mark would vanish in its white
interior and a black one would read as another ridge line.

**The generator emits the silhouette's own top edge alongside the bitmap**
(`MountainsTop`, one entry per column), and `GnssSkyView::ridgeHeight()` reads
that. The first version drew this ridge from eleven hand-typed numbers while the
art came from somewhere else: a drawing and a claim that can disagree, on the one
screen whose whole job is to say which way the sky is open.

Three geometry decisions, all from the panel:

- **Wider than the screen.** The asset is 700 px against a 540 px panel, so the
  ridge runs off both edges. A silhouette that ends inside the frame reads as a
  picture of mountains; one that leaves it reads as terrain the rider is standing
  in.
- **Seated 60 px below the horizon** (`GnssSkyView::kRidgeCrop`), so its bottom
  is cut off. Terrain does not end tidily above a caption.
- **Clipped by our own blit.** `GfxRenderer::drawMono1bpp()` goes through
  `drawPixel()`, which LOG_ERRs every out-of-range pixel rather than dropping it,
  so blitting an over-wide asset straight would be tens of thousands of serial
  lines per frame. `drawRidgeClipped()` is the two loops that avoid that.
- **A horizon line under it, edge to edge.** Not needed to carry the ridge across
  a wider panel any more, but it is what makes the crop read as ground rather
  than as art that ran out of pixels.

The art is drawn 1:1 and never scaled, which is also why `ridgeHeight()` returns
the asset's own pixels rather than a fraction of the box height (parent repo's
CLAUDE.md, "Map rendering").

### The signal ladder is the header's, not this screen's

Both the meter next to the readout and the size of each satellite's mark read
`MapGnssBars`' calibrated C/N0 rungs -- 26/31/36/40 dB-Hz of the best satellite
(`map-header-status.md`, the maintainer's numbers against real readings from
this L76K, 2026-09-10).

The meter is **five slots for four rungs**, one lit per rung passed. The fifth
is not a spare: it is the "heard, below the first rung" state, the same one
`GnssSkyView::snrBucket()` draws as its smallest mark. So an empty meter means
nothing worth hearing, one lit slot is a satellite that cannot read its own
ephemeris off the air, and full is open sky.

Slots are filled solid rather than part-height. A two-thirds bar inside a box at
this size reads as a rendering fault, and the number beside it already carries
the value. Empty slots stay outlined: four 2 px outlines read as four
missing-glyph boxes on the panel (seen 2026-09-10), and this screen stands for
minutes with nothing to show, so its instrument has to look like one while
empty.

**Deliberately the same instrument on both screens.** This is where a rider
first meets it, with minutes to look at it, and a wait screen that scored the
sky on its own invented ladder would teach them to read the header wrongly. The
earlier version of this screen had exactly that: thresholds of 18/24/34 picked
by eye. `test/gnss_sky_view` now asserts the two agree against the constants
rather than against copies of them.

Two differences, both stated in code:

- **No hysteresis here.** `MapGnssBars::resolve()` takes a default `State`, so
  no slack is applied. The header damps because the map repaints per fix; this
  screen redraws at most once every five seconds and has nothing to damp.
- **Empty slots stay as outlines.** The header draws nothing below the first
  rung. Here the screen is up for minutes with nothing to show, and an
  instrument that disappears reads as a broken one.

The per-satellite mark folds the top rung into the one below it: the radius
ladder is 3/4/5/6 px and a fifth step would need a 14 px wide mark, which is too
big for a plot holding sixteen of them.

## The two ways out, and what they really choose

One position source per map session (`MapActivity`'s `bleInUse_`, maintainer's
call 2026-09-03), so the rows are not "wait" versus "do not wait". They pick a
source:

| row | receiver | map session runs | what it costs |
|---|---|---|---|
| **Open the map now** | keeps searching, handed to the map | GNSS, no BLE | nothing; the header glyph goes Seeking to Fixed when the fix lands |
| **Take position from the phone** | powered down here | BLE | the sky for this session; buys tile sync, the command channel and the phone's stabilised heading |

And the screen leaves on its own the moment a usable fix arrives -- same
acceptance test as the map's (`valid` latches on the first solution, `quality`
is what says the receiver still has satellites; 0 is no fix and 6 is dead
reckoning with nothing behind it).

**Back goes home, not forwards.** Same as the trip picker: a Back that
continued into the map would mean something different here than everywhere else.

## Ownership of the rail: the one trap

Whoever starts the receiver has to be the one to stop it, and this screen starts
it (`gnssStart()`, which also seeds it with the persisted last fix -- see
`gnss.md`). The map's own rule is that a receiver it finds already running
belongs to somebody else, so it declines to own it and leaves the rail up on
exit. That rule is right for a host `CMD:GNSS ON` session and wrong for a
handover.

Hence `MapActivity`'s `adoptRunningGnss` constructor flag: set only on the
handover from this screen, and it makes the map's `onExit()` drop the rail.
Without it the wait screen would leak a powered rail -- which also feeds the
LoRa radio -- every time a rider went to the map and then home.

The phone row drops the rail here instead, before the map opens.

## Where it is in the flow, and where it deliberately is not

`ActivityManager::goToGnssAcquire()` is the front door, and it falls straight
through to `goToMap()` unless there is something to wait for:

- the build has a receiver (`ENABLE_GNSS_CMD`), and
- `SETTINGS.mapGnssPosition` is on, and
- the receiver does not already have a usable fix.

So a second entry into the map inside one session shows nothing, and a device
with the setting off behaves exactly as before.

Two callers route through it: the Home screen's Explore row and the trip
picker's rows (which carry the chosen route through the wait untouched).

Three paths deliberately do not:

- **`CMD:GOTO_MAP`** -- host tooling that must land on the map itself. Every
  screenshot recipe in the parent repo depends on it.
- **The wake-into-map path** -- a resume, not a departure.
- **The trip picker's OOM fallback** -- an allocation has already failed there,
  and the wait screen is another one.

## The wait has a limit, and the screen says so while it runs

`Wait for the sky` in Settings (category Map, `mapGnssWaitLimit`): **No limit /
2 min / 5 min / 10 min, five by default**. When it runs out the screen opens the
map by itself, exactly as the first action row does -- receiver handed over, still
searching behind the map frame.

The default matters more than the value. A ride took 526 s to first fix and a
walk never got one, so a wait with no end is a screen that can hold a rider out
of their own map indefinitely -- and the map is useful without a fix, because it
draws from the persisted last position. The wait is a courtesy, not a gate.

"No limit" stays offered because somebody parked and watching the sky fill is
exactly who this screen was built for, and a timeout would cut them off
mid-observation.

**The countdown is on the panel the whole time it runs.** A screen that jumps to
the map on its own without having said it would is a screen that took a decision
away from the rider.

## The type ladder, and the 813 kB it did not spend

Four steps, biggest first: the title (12 pt bold), the subtitle (10 pt bold),
the clock and the readout's first line (12 pt), the signal line (10 pt), the
countdown and the advice (8 pt).

The mockup asks for a title around 18 pt, and **the UI font family stops at
12**. Registering NotoSans 14/16/18 would fix that and costs **813 kB of flash**
as full families, or **251 kB** as only the four rezes actually drawn (both
measured on t5s3pro, 2026-09-10, against a 3,896,147-byte baseline).
Maintainer's call: not for a title. The ladder carries the hierarchy with weight
and spacing instead.

NotoSerif 14 is linked in every build and is the one genuinely larger face
available for free. Deliberately unused: every other screen here is sans, and a
serif title would read as a different device.

## The refresh budget, which is the reason the clock is coarse

A windowed refresh on the T5 S3 Pro costs **1,081 ms measured**
(`t5s3-partial-refresh.md`), the same as a whole panel today. A screen that
redrew per fix would hold the panel busy for a third of every second of a
ten-minute wait, for a picture that changed by one satellite.

So: **never more than one redraw per 5 s**, and only when the picture would
actually differ (satellite count, satellites heard, best signal, or the clock's
own 5-second step). The clock is deliberately coarse for exactly that reason --
a per-second timer would force a redraw with nothing new in it and read as a
device that is busy rather than one that is waiting.

A selection move refreshes only the two action rows, which is a much smaller
rectangle than the sky.

## Input, and why the rows are drawn as buttons

`MapActivity`-style logical buttons do not exist on every board. On the T5 S3
Pro the physical inputs are a side switch (tap = Confirm) and the capacitive
home key (tap = Confirm); Back is a touch gesture, and there is no Up or Down at
all (`lilygo-t5s3-bringup.md`). In the default touch mode no hint boxes are
drawn either (`touch-modes.md`).

So the screen accepts three things at once, and any one of them is enough:

- **Up / Down / Left / Right** move the highlight (X4, X4 Pro).
- **Confirm** activates the highlighted row (every board).
- **A tap on a row itself** activates it directly (any board with a digitizer,
  in any touch mode).

## What was added

- `lib/Gnss` -- a per-satellite snapshot: `GnssSatellite` (talker, PRN,
  elevation, azimuth, C/N0, and whether the position fields were present),
  `satelliteCount()` and `satellite(i)`. GSV already parsed those four fields
  per satellite and threw three of them away.

  `sizeof(GnssSatellite)` is 8 bytes by layout, so the array is 256 bytes plus
  the stale mask and the count -- **derived, not measured**: no build was taken
  with and without it to price it on its own.

  It updates **in place per talker** and sweeps at the end of each constellation's
  GSV cycle: an entry disappears only when a *complete* cycle stopped listing it.
  A lost GSV sentence is ordinary on a 9600 baud line, and dropping four
  satellites out of the plot every time one goes missing would read as the sky
  emptying. Costs 32 entries of internal RAM, fixed, never grown.
- `src/activities/map/GnssSkyView.h` -- the projection, the signal buckets, the
  ridge geometry and the inset plot area. Pure arithmetic, no renderer,
  host-tested in `test/gnss_sky_view` (19 tests: north centring, azimuth wrap,
  out-of-range clamping, the crop, the asset offset, the inset, a degenerate
  box).
- `scripts/gen_mountains.py` and `src/images/Mountains.h` -- the horizon asset
  and its per-column top edge.
- `CrossPointSettings::mapGnssWaitLimit` plus its `Wait for the sky` row.
- `src/activities/map/GnssAcquireActivity.{h,cpp}` -- the screen.
- `ActivityManager::goToGnssAcquire()`, plus two new `goToMap()` arguments
  (`adoptRunningGnss`, `forcePhonePosition`).
- `MapActivity`: `bleInUse_` now also honours `forcePhonePosition_`, and
  `gnssHeaderState()` / `pollGnssFix()` read `bleInUse_` instead of the setting
  -- so a session the rider sent to the phone neither draws a receiver glyph nor
  accepts a fix from a receiver something else is running.
- Ten `STR_GNSS_ACQ_*` strings in `lib/I18n/translations/english.yaml`. The
  build strips unused keys under SCons (`scripts/gen_i18n.py`), so a string added
  to the yaml and not yet drawn does not compile.

## The synthetic sky, and why the plot needed one

`CMD:GNSS SKY 16` fills the plot with a deterministic pattern; `CMD:GNSS SKY OFF`
clears it. Devel only, behind `ENABLE_GNSS_CMD`, which is set in `env:t5s3pro`
and in no release env -- a screen that claims satellites the device cannot see is
a lie a shipped build must not be able to tell.

**It exists because the screen's whole subject was unverifiable by waiting.**
Five passes drew no mark at all. So the plot's placement, its mark sizes, the
halo over the ridge and whether a dozen marks read at all could not be judged,
on the one screen where they are the entire content.

The numbers are chosen to exercise the drawing rather than to look like a sky
(`src/GnssFakeSky.h`): elevations from 3 to 80 degrees, so the low ones land
inside the ridge, which is where the halo either works or does not; C/N0 across
all four calibrated rungs plus some zeros, so every mark size appears next to an
outline; and two satellites heard but not located, which is the state that leaves
a count with no mark and puts "N not located yet" on the readout. Azimuths are
offset so nothing hides under a cardinal label.

**It substitutes at five reads and nowhere else** (`skyCount()`,
`skySatellite()`, `skyInView()`, `skyHeard()`, `skyBestSnr()`), so the drawing
cannot tell the two skies apart. That is the only thing that makes a screenshot
taken with it say anything about the real one. It never produces a position and
never touches `Gnss`, so the map is unaffected.

**No host test can replace it, and that is a property of the problem.** The
drawn ridge and the geometry that decides "this satellite is behind terrain"
both come from the same generator, so a test comparing them is a tautology
against the same data. A mark drawn on the ridge is the only instrument there
is, and this is what provides one.

## What a hardware pass has to check

Nothing here has been on a panel. In rough order of what would embarrass us:

Settled on the panel, 2026-09-10, on a T5 S3 Pro:

- the layout fits 540x960, and the readout, rows and hints all land
- the redraw cadence works -- the clock was read at 0:05 and again at 2:10
- the countdown runs and the texts render, middle dot included
- the meter's empty state is legible now that its slots are full height

Still open, and the first one is the big one:

1. **Nothing has ever drawn a satellite.** Every pass was indoors: the receiver
   heard one or two, located none, and the sky stayed empty. Untested therefore:
   the panorama's placement, the mark sizes, the white halo over the ridge, and
   whether the plot reads at all with a dozen marks on it. This needs a walk
   outside.
2. **Elevation and azimuth from this receiver.** The snapshot is read off GSV
   fields nothing here has ever used. Confirm with `CMD:GNSS RAW ON` that the
   marks land where the sentences say.
3. **Does the handover actually keep the fix?** `Gnss::begin()` treats a second
   call as a no-op by design, so the map must not restart the receiver -- and the
   rail must be **down** after the map exits, which is the `adoptRunningGnss`
   flag's whole job. Read it back with `CMD:GNSS STATUS` after leaving the map.
4. **Does the phone row leave BLE working**, with the setting still on.
5. **Does the screen leave by itself** when the fix lands, and how long after.
6. **Does the limit fire** at the minute it promises, and does the map that
   follows carry the receiver.
