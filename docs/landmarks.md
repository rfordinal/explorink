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

## Tourist draws them too, capped at rank 2

Added 2026-10-28. A castle is rank 0 and a church rank 2, so 2 is the lowest
cutoff that lets both through, and it brings manors, museums, viewpoints and
towers with it -- which is the set a city walker wants. Rank is a tier of
importance and not a per-category switch, which is the whole reason this is one
number rather than a mask.

It stops short of tier 3 deliberately. A guidepost belongs on a trail, and
4,680 locality names in one Sulov-sized build would bury an old town.

The ladder mirrors hike's, capped: rungs 0-1 at rank 2, 2-3 at rank 1, wider at
rank 0 only.

**Seen on a device 2026-10-28**, X4 Pro, after a Bratislava old-town build went
to the card over Wi-Fi. Twelve landmark names on one rung-0 panel over the old
town: Michalska veza, kaplnka sv. Jakuba, Mariansky stlp, Kaplnka Kristovho
tela, Rybarska brana, Kostol Nanebovzatia Panny Marie, the galleries.

**Three of those twelve are statues** -- Anton Petrak, Pavol Orszagh
Hviezdoslav, Tomas Garrigue Masaryk. A quarter of the names on a city panel are
busts drawn with no symbol, and a name with no symbol reads as a place. The
host render had shown two of eleven and that read as a footnote; the panel says
it is not one. Museums and memorials are 28 of the 117 landmarks in that box,
the largest single category. This is the strongest argument for the glyph sheet
(T-2036) that this layer has produced, and it is an argument for the symbol
rather than against the category.

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

| rung | `landmark_rank_max` | names drawn on the Súľov panel |
|---|---|---|
| 0 | 3 (everything) | 3 |
| 1 | 3 (everything) | 10 |
| 2 | 1 | 8 (3 at rank 0) |
| 3 | 1 | 14 (9 at rank 0) |
| 4 and wider | 0 | 13 (11 at rank 1, and 7 dropped) |

**The ladder was measured three times and the first two measurements are
dead.** The first counted marks and set every rung a step tighter: rank 1 at
rung 2 put 151 marks on one panel and at rung 3 put 527. The second compared
against a published hiking map at the same width and moved rung 1 back up to
tier 3, because that sheet names guideposts and localities at about 1.4 km
across.

The third is the one that holds, and it only became possible once **a landmark
stopped drawing an 18 px square and became a 4 px dot plus a name**. 151 dots
cost a fraction of 151 squares, and the name count is capped by
`pointMaxLabels` regardless, so the crowding the first measurement found no
longer exists. Rungs 2 and 3 carry rank 1 now, which is what puts summit names
on a ridge: rung 2 goes from 3 names to 8 for +0.35 points of panel ink, rung 3
from 9 to 14 for +0.20.

**Rung 4 is where it turns, and it turns the other way.** Rank 1 there drops
the count from 13 names to 11 and raises the dropped-for-no-room count from 3
to 7: peak names crowd out the town names, which are what a 7.7 km wide panel
is for. So 4 and wider keep rank 0 only.

**A measurement taken against one drawing decision does not survive the next
one.** That is the lesson worth more than the numbers.

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

### A summit is set the way a paper map sets one

Name above, the triangle on the point, the height below, all three centred.
The label block **straddles** the mark rather than sitting beside it: the two
lines are held apart by the mark's own height (`FittedLabel::gapPx`), and the
whole block is centred on the point.

The straddle is tried **first and on its own**, before the eight positions and
without competing on ink. When it is legal it is the right answer for a
summit, and a position that puts the name beside the mark instead of around it
is not a cheaper version of the same thing.

**The height is set smaller than the name** (`label_ele_px`, 12 -- ubuntu_5,
the face the contour numbers use). The number qualifies the summit; the name
is what the eye is hunting for, and at the name's own size the two read as
equals. 0 means "the name's size", so a style that never thought about it
keeps the old behaviour. Each line carries its own size and line height in
`FittedLabel`, which is what makes a mixed-size block measurable at all.

Two rules the layout gives way on, both because the name is worth more than
the number:

- **Only when the name fits on one line.** A name that already wrapped has no
  room for a third line, so it keeps the wrap and loses the height.
- **Only when the straddle box is legal.** If there is no room around the
  mark, the label falls back to the ordinary eight positions with the name
  alone -- a stacked name-over-number beside the mark would read as two
  labels.

The mark is drawn by the mark pass and the label by the label pass, so a
summit whose label is dropped still shows its triangle. That is why the
triangle is not drawn by the label.

**Two budgets, never one.** `placeMaxLabels` is the places' ceiling and
`pointMaxLabels` is the landmarks'; the placer stops only when both are spent.
They shared one running total at first, and because a rung-1 style lets places
draw four names, the landmarks got whatever was left of those four: measured
over Súľov, a panel holding **fourteen named landmarks drew four labels**, and
the cap that stopped it belonged to the other layer.

**`label_px` is a line-height cap, not a type size, and that cost a day.** The
canvas picks the largest registered face whose line height fits under the
number and **falls back to the smallest face when nothing does**. The faces
are 12 px (ubuntu_5, built for contour numbers), 23 (notosans_8), 24
(ubuntu_10), 29 (ubuntu_12) and 34 (notosans_12, dropped by `OMIT_FONTS` in
the shipped build). So `label_px: 8` drew the 12 px contour face, and every
value from 0 to 22 renders **byte-identically** -- measured 2026-10-27 by
sweeping 8, 10, 12, 14 and 16 and diffing the frames.

The shipped value is **23**, which is notosans_8. 24 (ubuntu_10) was tried
first and read as too big on the panel: the two line heights differ by one
pixel, but Noto Sans at that ppem is a narrower, lighter cut, so it reads a
size smaller and two long names stop needing an ellipsis. What 24 has that 23
does not is a bold cut -- `smallFontFamily` is regular only -- so a tier of
landmark names set in bold would have to move back up.

Landmarks also carry their own `label_max_width_px` (230), separate from the
places' 170, because a landmark name runs longer: "Kostol sv. Michala
archanjela" against "Súľov-Hradná".

**The halo is 3 px and every map label gets one.** `MapLabels::draw()` paints
the string in white eight times per ring radius before the black pass lands,
so only the pixels the letters need are knocked out and the map keeps showing
through between them. It was 2, which held up over white but thinned over the
dither a field or a forest is drawn with -- and that is exactly the ground a
locality name sits on. 3 costs one more dropped label per rung-1 panel,
because the knockout box each name reserves grows with it. The contour numbers
carry the same number through a different path: `drawTextRotated` grows the
glyphs by `contourLabelHaloPx` in the opposite ink, one pass rather than
eight, because a rotated label needs it most where it sits on the line it
names.

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
