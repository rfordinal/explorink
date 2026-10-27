# Landmarks on the map

The point layer's second kind (`kind = 2`): castles, manors, churches,
museums, viewpoints, peaks, passes, rocks, caves, waterfalls, towers,
guideposts and locality names. Built 2026-10-27 (T-305).

The bytes are `../../docs/point-file-spec.md` in the parent repo; which OSM tag
becomes which category is `../../docs/map-landmarks.md`; the writer is
`mapbuilder/tilegen/landmark.py`. This file is what the device does with them.

## A landmark is a dot and a name, never a square

The mark vocabulary says a square means "this exists in the data"
(`../../docs/map-render-spec.md`, "Point mark vocabulary"), and a safety point
draws one with a 24 px glyph inside it. A landmark draws **a dot**
(`layers.points.landmark_dot_px`) and its **name**, and no square at all.

Maintainer's call 2026-10-27, taken on the first real render over Súľov. There
is no landmark glyph sheet, so every landmark square came out empty, and an
empty square costs a mark's worth of ink to deliver no information: it says
"something is here" on ground where the name beside it is the whole point.

Two things follow:

- **`label_px` is what makes this layer worth drawing.** With it at 0 a
  landmark is an anonymous dot. The name is the information; the dot only pins
  it to its ground.
- **`kLandmarkTextOnly` (MapPointTypes.h) still means something.** It marks
  `locality` and `guidepost`, which stay a dot even on the day a glyph sheet
  exists, because they are names and not symbols. Giving the other categories
  a glyph back is one branch in `drawMarkAt()`.

Never index `kPoiIconByCategory` with a landmark category. It is the safety
vocabulary: category 1 is water there and a castle here, so one table indexed
by the other's id draws a confident lie.

## The rank cutoff, and why it is scoped to one kind

Every landmark record carries a writer-defined `rank`, 0 most important.
`layers.points.landmark_rank_max` draws `rank <= this` and hides the rest, per
rung and per mode through the style's own `when` clauses.

**The test is scoped to `kind == Landmark` and must stay that way.** A safety
record always carries rank 0, where it means "no rank" and never "most
important". Hoisting the test out of the kind branch would let a landmark
cutoff hide a spring, and the zoom that would hide a spring is the zoom a
walker needs it at.

A version-1 shard reads every record back as rank 0 because the byte does not
exist in that format. Harmless for the same reason: such a file holds no
landmarks at all, so the test never sees one from it.

### The ladder was measured, not reasoned

Arithmetic off the regional density (`../../docs/map-landmarks.md`, "The
density problem") predicted about two marks on a rung-0 panel and was a whole
rung too generous everywhere else. Measured on the Súľov panel 2026-10-27:

| rung | `landmark_rank_max` | what the panel held before tightening |
|---|---|---|
| 0 | 3 (everything) | fine |
| 1 | 3 (everything) | fine |
| 2 | 0 | 151 marks at rank 1 |
| 3 and wider | 0 | 527 marks at rank 1 |

Rung 1 carries tier 3 because of the second measurement, against a published
hiking map of the same ground: that sheet names guideposts and localities at
about the rung-1 width (1.4 km), and our own panel holds fourteen named
landmarks there. The mark count is not the only question -- what a map of this
kind is expected to name at a given width is the other.

Peaks and towers around a ridge are far denser than the regional average says,
which is why a count over a big box cannot set a per-panel cutoff.

## Two masks, because Nearby and the style answer different questions

`MapPointSource::Config` carries **one category mask per kind**:

- `safetyCategoryMask` is `Nearby -> Show on map`: a temporary view the rider
  switched on, nothing to do with the style.
- `landmarkCategoryMask` is not a UI toggle at all. **A landmark is on the map
  because this mode's style draws landmarks**, never because somebody asked to
  see nearby points.

They used to be one mask and one gate, so a rider with every Nearby row off
got no castles either (maintainer, 2026-10-27). Which landmarks draw is the
rank cutoff above, applied at draw time.

`kindMask` comes from the style too, through `MapPointMarks::kindMaskFor()`.
A mask that disagrees with the style lies in one of two directions: a kind the
style draws but the mask filters never reaches the canvas, and a kind the mask
allows but the style hides costs a shard read for nothing. The mask is the
cheap test -- a shard whose header holds no wanted kind is skipped without
reading a record -- so it is derived from the style rather than kept beside it.

## Clustering holds one kind

A cluster's `categoryMask` is a bit per category id, and the two kinds number
their categories independently: safety 2 is a shelter, landmark 2 is a manor.
So a point only ever merges into a cluster of its own kind.

**A point that finds no slot is drawn where it stands, never dropped.** With
both kinds on, the walk order decides which kind fills the 40-cluster table
first; dropping the overflow made every landmark on a dense panel invisible
while the log still counted it as drawn. One unclustered mark among clustered
ones is a small inconsistency. A whole kind missing from the panel is not.

## Names ride the place-name placer

`MapLabels` places landmark names and place names in one pass, because a name
has to know where every other name went. Landmarks have their own text size
(`layers.points.label_px`), their own weight (`label_bold`) and their own cap
(`max_labels`) next to the places' two -- a ridge full of peaks must not be
able to spend the villages' budget.

**Two budgets, never one.** `placeMaxLabels` is the places' ceiling and
`pointMaxLabels` is the landmarks'; the placer stops only when both are spent.
They shared one running total at first, and because a rung-1 style lets places
draw four names, the landmarks got whatever was left of those four: measured
over Súľov, a panel holding **fourteen named landmarks drew four labels**, and
the cap that stopped it belonged to the other layer.

Three more mechanics worth knowing:

- **The name is read during the point walk**, which is the only moment a
  point's name is in hand: `MapPointRef::name` points into the source's own
  reusable buffer and is valid until the next `nextMapPoint()`, the same
  contract `MapPlaceRef::name` has.
- **Names are fetched only when the style draws them** (`Config::wantNames`).
  A name is a seek and a read per surviving point, and the mark itself never
  needs one.
- **The label clears the mark it belongs to.** `MapLabelCandidate::markHalfPx`
  is that candidate's own reach, so a 4 px dot and an 18 px square push their
  names by different amounts instead of sharing the place dot's offset.

Only a landmark is named. A safety point's name belongs on the Nearby list,
and its square already says what it is, so naming it on the map would spend
the ink twice.

## What is not built

- **No glyph sheet.** The slot list is the frozen category ids in
  `mapbuilder/tilegen/point_spec.py`; the artwork is the maintainer's, judged
  on a panel render.
- **Nothing has run on hardware.** T-221 -- the `Useful places` menu and the
  POI marks themselves -- has never had a device pass, and landmarks reuse
  that whole path.
