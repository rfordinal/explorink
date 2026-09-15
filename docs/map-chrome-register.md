# The chrome register: what the map screen may not draw on

The map screen draws two kinds of thing. The map is one. The other is what this
doc calls **chrome**: the furniture the screen puts over the map and keeps in
one place -- the header band, the compass, the scale bar, the debug window, the
four front button boxes, the two side button boxes.

Chrome is drawn last, on top. That is correct for a road: a line running under
the button row still reads as a road leaving the panel. It is wrong for anything
that has to be *found*. A pin under the scale bar is a pin the rider never sees.
A place name with half its letters painted over reads as a different name.

The register is the list of rectangles chrome owns this frame, so that anything
placed into the map can ask before it draws.

`src/activities/map/MapChrome.h` holds it. `MapActivity::buildChromeRegister()`
fills it. `GfxRendererCanvas::areaReserved()` is how the renderer asks.

## What is in it

One entry per piece, never per band:

| Tag | Where it comes from |
|---|---|
| `Header` | `MapActivity::headerRect()`, which is `mapContentTop()` tall |
| `Compass` | `MapActivity::compassRect()` -- the glyph plus its white halo |
| `ScaleBar` | `MapActivity::mapScaleRect()` -- bar, ticks and numbers, plus the numbers' 1 px halo |
| `DebugWindow` | `MapDebugOverlay::currentRect()`, only when `mapDebugInfo` is on |
| `Button` x4 | `BaseTheme::frontHintBox(i, ...)` |
| `SideButton` x2 | `BaseTheme::sideHintBox(i, ...)` |

**Every rectangle comes from whoever draws it.** No constant is restated in the
register. A number written down twice is a number free to drift, and the drift
is invisible until it lands on the glass of a panel nobody has on the desk --
which is how the scale bar came to draw on the place label on an X3
(parent `docs/TODO.md`, T-296).

### Buttons are registered one box at a time

`BaseTheme::buttonHintsRect()` answers with the whole bottom band, because that
is what a caller repainting the panel needs. The register does not use it. The
band is mostly empty: on an X4 the four 80 px boxes start at x=58 and end at
x=422 (`src/components/themes/lyra/LyraTheme.cpp`, `kX4FrontPositions`), so a
full-width rectangle throws away a 58 px corner at each end -- and a pin balloon
is 42 px wide, so it fits in one.

The gaps *between* boxes are 8 px and 28 px, which nothing fits in. They are
free in the register anyway, because the register describes the screen rather
than guessing what will be placed on it.

`frontHintBox()` / `sideHintBox()` answer `false` for a box that is not drawn --
a touch panel, a locked panel, a button with no label on this screen. So the
band frees itself wherever the boxes are absent, with no test for it here.

The theme answers in **portrait logical coordinates**, the same pair
`MappedInputManager::hintBoxAt()` uses for the hit test. The map screen is
portrait, so those are the coordinates everything else on it is drawn in too.

## Who asks, and what each one does about it

The policy is per placer. There is no single rule, because the same collision
means different things.

| Thing being placed | On a collision |
|---|---|
| Place name (`MapLabels`) | try the next of its eight positions; dropped only if all eight are taken |
| POI mark (`MapPointMarks`) | not drawn |
| Pin balloon (`MapActivity::drawPins`) | treated as off the panel: an edge marker if those are on, and counted either way |
| Pin edge marker | unchanged -- clipped to `pinEdgeArea()` as before |

`MapActivity::drawViewedNearbyPoint()` -- the ring around the POI the rider
opened from the Nearby list -- deliberately does **not** ask. It is not a mark
the map placed: the rider asked for that one specifically, and neither dropping
it nor moving it is right. It already refuses and logs when it would fall
outside the map content area. Left as it is, 2026-09-15.

### Why a POI mark is dropped rather than moved

A square means "this exists **here**" (`docs/map-render-spec.md` in the parent
repo, "Point mark vocabulary"). A square nudged to a clear spot says something
false. Maintainer's call, 2026-09-15.

One tile of a cluster can drop while the rest of the cluster draws. That is
right: only the covered tile is invisible.

### Why a pin balloon is not flipped

The first design flipped the balloon to hang below its tip when the body hit
chrome, keeping the tip on its coordinate. Rejected by the maintainer,
2026-09-15: **the shape's orientation already means something.**
`drawPinBalloon()` takes a `step` that turns the pin so its point aims at where
an off-panel pin actually is (`src/components/icons/pins_shape.h`,
`kPinShapeFrames`), so a body hanging the other way would read as a direction
that was never given.

So a hidden pin takes the off-panel path instead, which already exists and
already merges crowded markers. With off-screen markers turned off
(`SETTINGS.mapPinsOffscreen`, off by default) that leaves no marker at all, so
the count is logged regardless -- an unseen pin with no trace is exactly the
failure the pin feature exists to prevent.

### Why the renderer does not know what a compass is

`MapRenderer` and everything it draws through is compiled on the host too: the
native preview binary (`test/map_preview`) and the webapp's firmware preview
panel run the same code with no screen furniture over it at all. So the question
reaching it is not "where is the compass" but `IMapCanvas::areaReserved(x, y, w,
h)`, which defaults to `false`. The device's `GfxRendererCanvas` overrides it
with the register; every other canvas keeps the default and behaves exactly as
it did before the register existed.

This is the same asymmetry `GfxRendererCanvas`'s `bottomReservedPx` already
documented, generalised: geometry may run under chrome, decisions may not.

## Cost

The register is 16 entries of 10 bytes plus two `size_t` counters: **168 bytes
on the C3** (`sizeof` is 176 on a 64-bit host, where `size_t` is 8 rather than
4). It lives inside `MapActivity`, which is heap-allocated, so it is 168 bytes
of the map screen's heap and **nothing at all of static RAM**.

**Read, not measured.** `sizeof` on a 64-bit host is 176; 168 is that adjusted
for the C3's 4-byte `size_t`. The static-RAM figures below are measured and show
zero, which is consistent with it living in the heap-allocated activity, but
nobody has read the map screen's free heap on both builds. `CMD:INFO`'s `heap=`
on the map screen, before and after, would settle it.

Measured 2026-09-15, both sides `pio run -e default` on the same machine within
the hour, the merge being the only difference -- `develop` at `ddc980dd` against
the merge `3b5814c7`:

| | before | after |
|---|---|---|
| RAM (static) | 59,140 B | 59,140 B |
| Flash | 4,063,665 B | 4,065,767 B |

So: static RAM unchanged, flash up 2,102 B for all four commits together.

An earlier revision of this table said +1,314 B against `develop` at `623040fe`.
Both halves of that were wrong by the time it was committed: the base was five
`develop` merges old, and the figure covered only the register commit, not the
scale bar halo or the edge-marker slide that followed it. **A cost measured
against a base the session then rebases past is dead** -- re-measure against the
merge's own first parent.

A query is a linear walk of about ten rectangles with four compares each. The
first draft reused `MapOccupancyGrid` (`src/activities/map/MapLabels.h`), which
is the right instrument for label-against-label placement and the wrong one
here: the furniture never moves within a frame, there are ten rectangles rather
than hundreds, and a grid answers to the nearest 8 px cell where this answers to
the pixel. It would also have cost a 1,250-byte clear per frame.

## What is verified and what is not

**Verified on the host** (`test/map_chrome`, 10 tests):

- the register's own arithmetic, including that touching edges do not count as
  overlapping, that an empty rectangle is not stored, and that an overflow is
  counted rather than silent
- a place name moving to its other side when its first position is reserved, and
  being dropped when all eight are
- a POI mark not drawing under a reserved rectangle, and still drawing beside one

Each of the three behavioural tests was confirmed to fail with the check
disabled, so none of them is a test that cannot fail.

**Verified on an X4 Pro panel, 2026-09-15.** Build
`docs/firmware-builds/x4pro-3b5814c7-good-chrome-register-verified.bin`
in the parent repo. Six pins written over the serial console onto computed
pixels: position pinned at 48.2889/17.2669 heading 0, rung 6 (45 m/px), and the
pin anchor measured at (230, 584) with a calibration pin placed at the device's
own coordinate, because the marker ladder moves the anchor off the style's
`marker_y_px`.

The x is exact: the changed-pixel box was 42 px wide, which is the shape's own
width, so the tip sits 21 px in from its left edge. **The y is inferred** from
the bottom of that box, and the box covers changed pixels rather than the shape,
so it can be a pixel or two high. It did not matter -- every one of the six
targets landed where it was aimed -- but a test needing better than a couple of
pixels must measure it another way.

| pin | tip aimed at | expected | seen |
|---|---|---|---|
| `#1` | the rider's marker | draws | draws |
| `camp` | open map | draws | draws |
| `#2` | under button box 2 | refused, edge marker | edge marker above the row |
| `#4` | under the compass | refused, edge marker | edge marker below the halo |
| `#5` | under the scale bar | refused, edge marker | edge marker, **on the bar** -- see below |
| `#3` | bottom-right corner past box 4 | **draws in place** | draws in place |

`#3` is the one that pays for registering buttons one at a time. No place name
touched the scale bar or a side box on any frame of the pass.

The build that was flashed was commit `70b7c340`, which two later rebases
removed from every branch. It is still the code that merged: `git diff 70b7c340
3b5814c7` touches **four docs files and nothing under `src/`, `lib/`, `data/` or
`platformio.ini`**, checked 2026-09-15. So the archived binary is the bytes of
what is on `develop`, and it is filed under the merge's SHA rather than the dead
one.

**Two defects the host could not have found, both fixed in the same pass:**

1. The scale bar's numbers had no halo. They are drawn straight onto the map,
   and the map under them was a built-up stipple with roads through it -- one
   through the `0`, a thick one through `km`. They carry a 2 px halo now, the
   radius `data/mapstyle.json` already uses for place names, contour heights and
   route junction dots (`docs/map-scale-bar.md`).
2. The edge marker for `#5` landed on the scale bar. `pinEdgeArea()` clears the
   two button bands by shrinking one rectangle, and a corner cannot be excluded
   that way without giving up a whole edge. Markers now slide in 8 px steps
   until their box clears the register (`slideEdgeMarkClear()`).

**Verified on an X3 panel, 2026-09-15.** Build
`docs/firmware-builds/x3-882516cf-good-chrome-register-verified.bin` in the
parent repo. Same method, different everything else: a 528x792 panel, rung 5
(32 m/px) rather than 6, marker rung 3, Ride rather than Hike. The anchor was
**read off the code** this time rather than measured -- `anchorScreenX(528)` is
230 * 528 / 480 = 253 and `markerYForStep(3)` is 690 (`MapViewport.h`) -- and
the calibration pin's box then agreed with it to the pixel.

Seven pins, and every prediction held with nothing corrected:

| pin | tip aimed at | expected | seen |
|---|---|---|---|
| `meet` | open map | draws | draws |
| `#1` | the rider's marker | draws | draws |
| `#2` | under button box 2 | hidden | hidden |
| `#5` | under the compass | hidden | hidden |
| `camp` | under the scale bar | hidden | hidden |
| `#3` | **the 54 px gap between boxes 2 and 3** | **draws** | draws, touching neither box |
| `#4` | the 65 px corner past box 4 | draws | draws |

Two things only this board could say.

**The gap between two buttons is usable here.** The X4's gaps are 8 px and
28 px and a 42 px balloon fits in neither. The X3's middle gap is **54 px**
(`kX3FrontPositions` = 65, 157, 291, 383, boxes 80 wide) and `#3` sits in it
cleanly. So per-box registration pays twice on this panel: the gap and both
65 px corners, all three of which a full-width band throws away.

**The `mapPinsOffscreen`-off branch.** This board has the setting off, so the
three hidden pins got no edge marker at all and the only trace is the count.
Read off a serial capture through one `redraw`:

```
[DBG] [MAP] 4 pin mark(s) drawn
[DBG] [MAP] 3 pin(s) hidden by screen furniture
```

No `, shown as edge markers` suffix, which is that branch exactly, and 4 + 3 is
the split the targets predicted.

**T-296's first half, on the board it was measured on.** The scale bar reads
`0 2 km` with its halo and `Limbach` does not touch it. Honestly: this is a
different view from the 2026-09-09 shot where `Bratislava` and the bar shared
pixels, so it corroborates rather than proves -- the proof that a name cannot be
placed there is structural and host-tested. **The second half cannot be tested
on this board at all**: the X3 draws no side hint boxes on the map screen, zoom
being on the front buttons (`Up`/`Down` in the bottom row), so the north arrow
has nothing to collide with.

**Still not verified:**

- **The X4.** Same 480x800 layout as the X4 Pro, so a regression check rather
  than a new one -- but a C3, a different binary and a third of the heap.
- **Every rung but 5 and 6.** The scale bar's width is per rung, so
  `mapScaleRect()` changes with it. Rung 6 was seen on the X4 Pro and rung 5 on
  the X3; 0 to 4 are unlooked at.
- **POI marks.** Not one square drew on either board, and the layer was not the
  reason: `data/mapstyle.json` `layers.points` carries `square_px: 18`,
  `safety_enabled: true` and **no `when` block**, so it is on at every rung. By
  elimination neither card held a `.tip` shard for the test area -- **that is a
  deduction, not an observation.** Nobody looked at either card or at a
  `placesEmitted` count. Only the host test covers this path.
- `pinEdgeArea()`'s top edge moved down by 3 px, because it now takes the
  compass box (centre + glyph + halo = 39 px) rather than centre + glyph = 36 px.
  The halo is drawn, so the new number is the right one; nobody has looked at
  the 3 px.

**The one thing no test can fail on.** `MapActivity::buildChromeRegister()` has
no host test: it needs a theme, a panel and a `GfxRenderer`. The 10 tests cover
the register's arithmetic and the two placers that query it, so a rectangle
registered in the *wrong place* passes all 519 of them and is caught only by a
thumb on the glass. That is why the pin table above is the evidence for this
feature and the host suite is not.

**Known and not fixed:** an edge marker's distance label and a place name can
draw on the same pixels -- `9.5 km` and `10 km` across `Svaty Jur` in this same
pass. It is a different pair from what the register covers: both are map things,
placed by two passes that cannot see each other. Parent `docs/TODO.md`, T-2016.

## Adding a piece of chrome

1. Give it a rectangle accessor next to the code that draws it, and make the
   drawing read the same accessor. Two copies of the geometry is the defect this
   file exists to prevent.
2. Register it in `MapActivity::buildChromeRegister()` with its own tag.
3. If it is conditional, register it only when it is drawn. Reserving a box
   nothing paints costs the map that area for nothing -- which is why the debug
   window is registered only when `SETTINGS.mapDebugInfo` is on.
4. If the register runs out of room, `buildChromeRegister()` logs it. Raise
   `MapChromeRegister::kMaxEntries`; do not let the log stand, because a dropped
   entry means every placer believes that furniture is not there.
