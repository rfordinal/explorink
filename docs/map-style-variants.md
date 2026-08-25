# Style is per travel mode and per zoom rung

**Built 2026-08-25. Judged on the host renderer, not on a panel** — there is no
device to test on (the X4 was lost 2026-08-22). Host tests pass, the ESP32 build
is clean, and all seven rungs were rendered and compared: see "Judged on the host
renderer" below. What the preview cannot answer is what the panel does with a
5 px road next to a 4 px one, which is exactly the number this change tunes.

## The problem

Every length in `data/mapstyle.json` is a device pixel. A pixel is not a fixed
amount of ground: it is 1 m at the closest zoom rung and 45 m at the coarsest.
So one width per road class means an 11 px motorway that covers 11 m of ground
at rung 0 and **495 m at rung 6**.

That was measured, not guessed: roads were two thirds of all ink at both
overview rungs, 9.1 % of 13.9 % at rung 3 and 9.4 % of 14.4 % at rung 4
(`../../docs/PROGRESS.md`, 2026-08-08). The note there names the fix and says it
was not started: "a per-rung width multiplier, or a width set per rung".

A multiplier is the wrong shape. A motorway must stay visible as it thins, a
residential street should vanish rather than thin, and a railway's tick rhythm
is not a width at all. Three different decisions per class is a table.

The same argument applies to travel mode, where the filter already existed but
the *style* did not: a hiker's footpath and a rider's motorway are not the same
mark, and a mode that only decides "drawn or not" cannot say so.

## The grammar

Any object in `data/mapstyle.json` may carry a `when` list:

```json
{
  "match": { "class": ["primary"] },
  "width": 9, "casing_px": 2, "major": true,
  "when": [
    { "steps": [2],          "width": 7, "casing_px": 2 },
    { "steps": [3, 4],       "width": 5, "casing_px": 1 },
    { "steps": [5],          "width": 4, "casing_px": 1 },
    { "steps": [6],          "width": 3, "casing_px": 0 },
    { "modes": ["hike"], "steps": [6], "hidden": true }
  ]
}
```

- **`modes` omitted means every mode. `steps` omitted means every rung.** An
  entry with neither always applies, which is how a value is overridden without
  repeating the outer field.
- **Entries apply in order, and the last matching one wins.** That is the whole
  precedence rule. There is no specificity scoring to reason about, and the file
  reads top to bottom.
- A patch is a shallow merge; a dict value deep-merges. `match` and a nested
  `when` cannot be patched.
- **`when` is banned under `device`.** The marker anchor becomes a `constexpr`
  the whole viewport arithmetic is built on (`MapViewport.h`, `kAnchorScreenX`),
  so a per-rung anchor would be a per-rung viewport. That is a different
  feature, and the resolver, the generator and a host test each refuse it
  separately.

Everything with a `when` today: `layers.roads.rules` (width and casing per
rung), `layers.buildings` (`enabled`, rung 0 only), the built-up
`layers.landuse` rule (`hidden` at rung 0), and `layers.places`
(`max_labels`, 3 names at rung 0 up to 14 at rung 6).

## The device never sees a rule

This is the part worth being clear about, because "a config with nested
conditions" usually means an interpreter somewhere.

`scripts/mapstyle_variants.py` resolves the rules **at build time**.
`scripts/gen_mapstyle.py` walks the 3 modes x 7 rungs, resolves each, and emits
one `MapStyle` per *distinct* result plus a `[3][7]` index of `uint8_t`. The
firmware does one array lookup:

```cpp
const MapStyle& style = mapStyleFor(mode_, view.zoomStep);   // MapStyleTable.h
```

No parsing, no branching, no JSON, no card read. Identical variants are
deduplicated, so a style file with no `when` in it compiles to exactly one
struct and a 21-byte index.

Measured on this branch's style, which has 7 distinct variants (one per rung;
no mode differs yet): **`sizeof(MapStyle)` is 238 bytes, and the variants plus
the index are 1,687 bytes of flash.** The whole ESP32 build is 3,944,467 bytes,
60.2 % of the partition.

## The filter is per rung too, and it agrees with the style by construction

`modes.<mode>.classes` is the mode's **vocabulary**: which classes it cares
about at all. What `MapTileSource` actually filters with is that intersected
with the classes that rung's style draws:

```
mask[mode][rung] = modes[mode].classes  AND  { classes with width > 0 at that rung }
```

Two things fall out. A class the style hides at a rung is no longer read past
its record header there, instead of being read, bbox-tested, projected and then
drawn at width 0 — the whole cost and none of the picture. And the filter and
the style can no longer disagree, which was a bug that would have looked like a
missing road and read like a corrupt tile.

`gen_mode_masks.py` prints a note when a mode lists a class its style hides at
every rung, because `modes` reads like the list of what a mode draws and is only
half of it. It says this today for `living_street` and `track` in all three
modes, and `steps` in hike — all three are `hidden: true` in the style.

## What moved out of MapViewport::ZoomStep

Three fields lived in the zoom ladder and are now `when` blocks: `buildings`,
`builtUp` and `maxLabels`. They were drawing decisions in a table beside the
style rather than in it — two places to look, two places to edit, and the same
numbers free to drift. `MapViewState` lost `drawBuildings`, `drawBuiltUp` and
`maxLabels` with them and gained one `zoomStep`, which the caller uses to pick
the style and the renderer never reads.

`markerScale8` and `minMovePx` stayed. They are refresh policy — how big the
saved patch box is, and how far the marker must move before a waveform is worth
it — not appearance, and the maintainer's call was to keep them out of the
style.

## Where each piece lives

| file | what |
|---|---|
| `data/mapstyle.json` | the style, `when` lists and all. Written by the webapp, never by hand. |
| `scripts/mapstyle_variants.py` | the resolver. The only implementation; the laptop tooling loads *this* file rather than copying it. |
| `scripts/gen_mapstyle.py` | resolves 3 x 7, deduplicates, emits `MapStyleDefaults.h`. |
| `scripts/gen_mode_masks.py` | resolves 3 x 7, intersects with the drawn classes, emits `MapModeMaskDefaults.h`. |
| `src/activities/map/MapStyleTable.h` | `mapStyleFor(mode, rung)`. The entire runtime cost. |
| `scripts/test_mapstyle_variants.py` | the resolver's tests, run by `ctest` as `MapstyleVariants`. |
| `test/map_style_table/` | what the generators produced, and the invariants the firmware depends on. |

## Judged on the host renderer, 2026-08-25

The same `MapRenderer` builds as a host binary (`test/map_preview`), so a style
can be judged without a panel. All seven rungs, ride mode, anchored at Pezinok
so the view runs from a town centre out to Modra, Svaty Jur and Senec:
`../../docs/device-preview-shots/per-rung-roads-ladder-2026-08-25.png`.

| rung | m/px | ink before | ink after |
|---|---|---|---|
| 0 | 1 | 12.6 % | 12.6 % |
| 1 | 3 | 12.5 % | 12.5 % |
| 2 | 6 | 10.1 % | 9.5 % |
| 3 | 12 | 8.9 % | 8.3 % |
| 4 | 20 | 10.3 % | 9.6 % |
| 5 | 32 | 11.2 % | 11.2 % |
| 6 | 45 | 13.8 % | 13.0 % |

Rungs 0 and 1 are byte-identical, which is the design and also the check that
nothing resolved wrong. Rungs 2 to 4 are the clear win: the main-road network
stops reading as a black web and the place names come forward. Rungs 5 and 6
improve but less, and rung 5 barely moved.

**What it is still missing** is a hierarchy at the far end. At rung 6 the five
main classes are 5/4/4/3/2 px, which is close to flat: a motorway and a
secondary road are one pixel apart. That is the number to tune next, and it
argues for widening the spread rather than thinning everything further.

## The bigger legibility win is not the roads

Rendering the ladder made this obvious and it was not what the work set out to
find. **At rungs 4 to 6 the POI marks are the picture.** A mark is a fixed 18 px
square by design, so at 45 m/px each one covers 810 m of ground, and a few dozen
of them bury the map they are marking.

The mechanism this document describes makes that a one-line style edit:

```json
"points": { "square_px": 18, "when": [ {"steps": [4, 5, 6], "square_px": 0} ] }
```

`square_px == 0` hides the layer, and hidden means the shards are never opened,
so the coarse rungs also stop paying for a file per 39 km.

Rendered as a proposal, **not committed**:
`../../docs/device-preview-shots/per-rung-poi-proposal-2026-08-25.png`. Ink at
rung 4 goes 9.6 % to 7.5 %, at rung 5 11.2 % to 9.6 %, at rung 6 13.0 % to
10.6 % -- three times the change the road widths bought. It is left uncommitted
because which rung should stop drawing POIs is a style decision for the
maintainer, not a mechanism decision, and 4 is a guess.

## A rung says the scale, not the density, and a capital shows it

Second reference view, added the same day at the maintainer's coordinates
(50 04 29.0 N, 14 30 08.2 E, eastern Prague):
`../../docs/device-preview-shots/ref-prague-east-ladder-ride.png`. POI marks off
in both references from now on, also the maintainer's call -- a mark is a fixed
18 px square and at the wide rungs the marks are most of the ink.

Ink along the two ladders, same style, same mode, marks off:

| rung | m/px | Pezinok | Prague east |
|---|---|---|---|
| 0 | 1 | 12.6 % | 11.9 % |
| 2 | 6 | 9.5 % | 16.2 % |
| 4 | 20 | 9.6 % | 19.2 % |
| 5 | 32 | 11.2 % | 23.8 % |
| 6 | 45 | 13.0 % | **25.7 %** |

**They move in opposite directions.** Over Slovak countryside the ink falls as
the rung widens, which is the per-rung widths doing their job. Over Prague it
climbs steadily and ends at twice Pezinok's, and on the panel the city core is a
black mass by rung 5 with no street network readable inside it.

So the per-rung table is not wrong, it is **incomplete**: a rung tells the style
how much ground a pixel covers, and says nothing about how much road is on that
ground. The widths in the file were tuned on the Slovak view and a capital needs
more thinning at the same rung. Nothing in the style can currently express that,
because there is no density input to key a `when` on -- only mode and rung.

Two shapes that could fix it, neither built and neither obviously right:

- **Build-time.** The tile already knows how much it holds. A density figure per
  tile, or a stricter class cull inside a dense z11 tile, keeps the style simple
  and costs a tile rebuild. Same argument as the LOD culls
  (`../../docs/map-data-spec.md`, "Which layers a LOD carries at all").
- **Render-time.** The renderer counts what the viewport is about to draw and
  picks a narrower variant above a threshold. Cheap to try, but it makes the
  picture depend on where you are standing, which is the hysteresis problem the
  zoom ladder was shaped to avoid.

Also visible on the Prague sheet and not a road problem: **place labels collide
and truncate at rungs 5 and 6** ("Kostelec na...", "Libcice nad..."). The
per-rung cap rises to 14 names, which is right for a region of villages and too
many for a metro area, and `max_label_width_px` truncates rather than dropping a
name that will not fit. Same density gap, one layer over.

## Which classes are the mesh, measured

"Those dense lines around Neratovice" could not be identified by eye, which is
the whole problem with tuning by looking. `mapbuilder/tools/class_census.py`
(parent repo) reads the tiles and answers it. Eastern Prague at rung 6, 12 z11
tiles, 24,160 ways, 6,313 km of road geometry:

The `width` and `ink` columns are before this pass; the last two are after it,
measured on the per-class renders rather than computed.

| class | ways | km | % of length | width before | ink before | width now | ink now |
|---|---|---|---|---|---|---|---|
| railway | 2,399 | 1,140 | 18.1 % | 4 px | **26.4 %** | 2 px | 5.2 % |
| tertiary | 8,602 | 2,243 | 35.5 % | 2 px | **26.0 %** | 1 px | 5.1 % |
| motorway | 1,393 | 535 | 8.5 % | 5 px | 15.5 % | 5 px | 2.2 % |
| secondary | 7,393 | 1,287 | 20.4 % | 2 px | 14.9 % | 1 px | 3.6 % |
| trunk | 1,681 | 356 | 5.6 % | 4 px | 8.2 % | 4 px | 2.5 % |
| primary | 1,486 | 235 | 3.7 % | 3 px | 4.1 % | 3 px | 3.2 % |
| unclassified | 1,181 | 462 | 7.3 % | 1 px | 2.7 % | 1 px | 2.4 % |

**Nothing dominates any more.** Every class now costs between 2.2 % and 5.2 % of
the panel, where before two of them cost 26 % each. That matters beyond the
totals: it means the remaining difference between Prague and Pezinok is density
of road on the ground, not one class being drawn wrong, so the next move is the
density question and not another width.

Ink is the line's length in screen pixels times its drawn width, against a
384,000 px panel. They sum past 100 % because ways overlap and run off screen,
so the ranking is the finding and not the total. One class at a time, rendered:
`../../docs/device-preview-shots/per-class-prague-r6-2026-08-25.png`.

Around Neratovice specifically (50.22-50.30 N, 14.46-14.58 E, 1,537 ways,
734 km): tertiary 38.3 % of length, railway 21.5 %, secondary 13.6 %,
unclassified 13.1 %. **The mesh is `tertiary`**, with `secondary` as its second
layer.

Two things fell out that nobody was looking for:

- **`railway` had no `when` at all** and stayed 4 px ticked at 45 m/px, which
  made it the single biggest ink item in the viewport. The first pass tuned
  roads and never looked at it.
- **`unclassified` is already the 1 px hairline** and reads exactly as a paper
  map's minor road should: information when you look closely, silence at a
  glance. That is the evidence that 1 px is the right weight for the mesh, not
  a guess about it.

## A casing spends less ink than a thinner solid line

Found by getting it wrong. The first hairline proposal traded casings away for
smaller widths and **ink went up** at rung 4, 19.2 % to 20.5 % over Prague.

A casing draws the road at its full width in black and then a white stroke
`2 * casing` narrower inside it, so a 4 px road with a 1 px casing inks two
1 px edges: 2 px of black with white between them. A 3 px solid road inks 3 px.
Narrower, more ink, and it also throws away the thing that made it read as a
major road.

So the rule for thinning a class: **keep the casing while the road is one you
steer by, and go to a solid hairline only when it stops being one.** Never the
step in between.

## The hairline shape, applied

Maintainer's call 2026-08-25, in two steps. First secondary kept 2 px at rung 6
with tertiary on the hairline, to hold one step between the connecting network
and the mesh. Looked at again at 1:1: **still too dense, so secondary joins
tertiary at 1 px.**

The step between them is gone and that is accepted. At 1 px on 1-bit there is no
axis left to separate two classes: width is spent, colour does not exist, and
pattern is spoken for (dashed is a watercourse, ticked is a railway). What
carries the hierarchy at that rung is motorway 5, trunk 4, primary 3 against one
hairline texture for everything below, which is what a paper map at 1:400,000
does too.

What the roads do across the ladder now (the compiled table, read off
`MapStyleDefaults.h`):

Width and casing, `width/casing`:

| class | r0 | r1 | r2 | r3 | r4 | r5 | r6 |
|---|---|---|---|---|---|---|---|
| motorway | 11/3 | 11/3 | 9/2 | 7/2 | 7/2 | 7/2 | **7/2** |
| trunk | 10/3 | 10/3 | 8/2 | 6/2 | 6/2 | 5/2 | **5/2** |
| primary | 9/2 | 9/2 | 7/2 | 5/1 | 5/1 | 4/1 | 3/0 |
| secondary | 8/2 | 8/2 | 6/1 | 4/1 | 4/1 | 2/0 | **1/0** |
| tertiary | 7/2 | 7/2 | 5/1 | 3/0 | 2/0 | **1/0** | **1/0** |
| railway | 4/1 | 4/1 | 4/1 | 4/1 | 4/1 | 3/1 | 2/0 |
| unclassified | 1/0 | 1/0 | 1/0 | 1/0 | 1/0 | 1/0 | 1/0 |

**motorway and trunk keep a 2 px casing from rung 3 out**, not 1 px --
maintainer's call, 2026-08-25: a 1 px edge is too faint to read as a road you
steer by once the mesh below it is a hairline. That forced two widths, and both
forcings are worth knowing:

- `casing 2` needs `width > 4`, or `gen_mapstyle.py` drops it for having no
  white core left. So trunk goes 4 to **5** at rung 6.
- With trunk at 5, motorway at 5 would have been the same mark. It goes to
  **7**, whose 3 px white core reads against trunk's 1 px.
- motorway is then 7 at rungs 3 to 6 rather than thinning. Deliberate: it is the
  one class that should not fade at the widest rung, because it is what you find
  at a glance. It also has to be flat rather than 6 then 7, since a class that
  gets *wider* as the rung coarsens is a bug the host test catches.

Rung 6 ends with motorway and trunk as cased double lines, primary as a 3 px
solid, and one hairline texture for everything below.

Ink, points of panel, POI marks off, before this pass and after:

| | r3 | r4 | r5 | r6 |
|---|---|---|---|---|
| Prague east, one width per class | 16.2 % | 19.2 % | 23.8 % | 25.7 % |
| Prague east, hairline mesh, casing 1 | 16.2 % | 17.7 % | 18.6 % | 18.5 % |
| Prague east, hairline mesh, casing 2 | 16.9 % | 18.4 % | 19.9 % | **19.6 %** |
| Pezinok, one width per class | 6.2 % | 7.5 % | 9.6 % | 10.6 % |
| Pezinok, hairline mesh, casing 1 | 6.2 % | 7.3 % | 8.7 % | 8.8 % |
| Pezinok, hairline mesh, casing 2 | 6.2 % | 7.3 % | 8.7 % | **8.9 %** |

The 2 px casing costs about **1.1 points over Prague and nothing over Pezinok**,
which is the shape you would expect: it only touches motorway and trunk, and
those are 535 km and 356 km in the Prague viewport against 2,243 km of tertiary.
It buys the thing the ink number cannot show, which is that the network you
steer by reads as a network rather than as the widest strands of the mesh.

**Rung 5 is now the densest rung on the ladder**, 19.9 % against rung 6's
19.6 % over Prague. It still draws secondary at 2 px and railway at 3 px, and
the same argument that took rung 6 to a hairline applies at 32 m/px, where a
2 px secondary road is a 64 m band. Open.

What the render shows and the numbers do not: at rung 6 the motorway and trunk
spine through Praha becomes the strongest thing on the panel instead of one
strand in a tangle, and Neratovice, Kostelec, Klecany, Roztoky and Libcice are
readable. The reference ladders are re-rendered against this style
(`../../docs/device-preview-shots/ref-prague-east-ladder-ride.png`,
`ref-pezinok-modra-ladder-ride.png`).

**It does not close the density gap**, only narrows it: 19.6 % against
Pezinok's 8.9 % is 2.20x, against 2.42x before any of this. A capital still draws twice the ink
of countryside at the same rung, and the density input discussed above is still
the missing piece.

**The sheets are 1:1 now**, one 480x800 frame per panel, never resampled --
maintainer's standing rule, and it was caught here: the first sheets pasted each
frame at 200 px wide, which turns a 1 px hairline into a grey smudge, so a
decision about whether the minor network reads was being taken against an image
that could not show it. Seven X4 panels side by side is 3,456 px and that is the
correct file. See the parent repo's `CLAUDE.md`, "Every render is pixel perfect".

Still true: nothing here has been on a panel at all.

## What a hardware pass has to check

Nothing here has been on a panel. In order of what would hurt most:

1. **The coarse rungs, 3 to 6.** Do the thinner main roads still read as a
   hierarchy, or did they collapse into "all roads look the same"? The numbers
   in the file are a first proposal and were chosen by eye on a host render of
   Pezinok/Modra (`../../docs/device-preview-shots/per-rung-roads-2026-08-25.png`).
   Ink went 10.1 % to 9.5 % at rung 2, 10.3 % to 9.6 % at rung 4, 13.8 % to
   13.0 % at rung 6 — small, so the numbers probably want to go further.
2. **Rung 0 and 1 are unchanged by design.** If they look different, something
   resolved wrong.
3. **The mode switch.** No mode differs from another yet, so ride, hike and
   cycle must still look exactly as they did.
4. **Buildings at rung 0 and the wash from rung 1 out**, and the label counts
   per rung. Those numbers did not change; only where they are written did. A
   difference there is a migration bug, not a style decision.
