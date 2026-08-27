#pragma once

#include <cstdint>

#include "MapAreaClass.h"
#include "MapAreaFill.h"
#include "MapClassEnum.h"

// The style numbers MapRenderer draws with. Every length is in device pixels
// against the 480x800 screen, exactly as in data/mapstyle.json (see
// docs/mapstyle.json.md).
//
// data/mapstyle.json is the source; MapStyleDefaults.h holds the one compiled
// instance, generated from it by scripts/gen_mapstyle.py at build time. The
// device reads no style file at runtime -- same arrangement as
// MapModeMaskDefaults.h.
//
// A struct rather than free constants, because the renderer is also linked
// into the laptop-side preview (test/map_preview). One argument makes "which
// style produced this pixel" answerable there; free constants would only be
// answerable by rebuilding and hoping.
// How a line class is broken up.
//
// The two are genuinely different marks, not two lengths of one mark. `Dashed`
// breaks the stroke itself, so the background shows through -- that is a
// watercourse, and it reads as discontinuous. `Ticked` leaves the stroke whole,
// casing and all, and lays short marks across it -- that is a railway, and it
// reads as one continuous line that happens to be ticked. Every modern map
// draws them that way round.
// `None` is not "no pattern chosen", it is **draw no line at all**. Added
// 2026-08-27 because `solid` had come to mean two different things on a water
// class: a solid stroke when there is no tone, and nothing drawn when there is
// one -- the tone being the whole mark. One word with two meanings is the same
// defect as two words for one thing, so the second meaning got its own word.
//
// Only the water pass reads it. The generator refuses it on a road, where a class
// that draws nothing is what `hidden: true` already says.
enum class MapLinePattern : uint8_t { Solid = 0, Dashed, Ticked, None, DashMark, Hachured };

// **`Hachured` is the rock-face mark, and it is not `Ticked`.** Ticked lays its
// marks *across* a whole line -- a railway's sleepers. Hachured leaves the line
// whole and hangs short combs off **one** side of it, which is how every hiking
// map in this part of the world draws a `natural=cliff`: a rock formation that
// ends in a sudden drop, the combs pointing the way down. Maintainer's reference
// 2026-08-27, the Prosiecka dolina sheet.
//
// The side is not a choice. OSM's convention puts the lower ground on the right
// of the way's direction of travel, and the tile keeps the points in OSM order
// exactly so a correct-side comb is possible without refetching
// (docs/map-data-spec.md, "The relief layer's second class"; fetch_osm.py). Draw
// them on the left and the map says the drop is uphill.

// The mark a `DashMark` period carries between its dashes: dash, gap, mark, gap,
// repeating. The classic way a paper map separates route classes without colour,
// and on a 1-bit panel colour is exactly what there is none of -- a waymarked
// trail and a farm track are both a 1 px hairline today, and 28.7 % of the
// pedestrian ways around Vratna are on a waymarked route (measured 2026-08-27).
//
// Three shapes, not a vocabulary. Each one is a thing that has to survive on
// glass at 3 px, and none of them has been looked at on a panel.
//
// **Screen-aligned, not turned to the line, and that is for sharpness rather than
// for cheapness.** Maintainer's call 2026-08-27: a square with one fixed
// orientation reads better. An axis-aligned square lands on the pixel grid
// exactly, so all four edges are hard; rotate it and every edge becomes a
// staircase the panel has no grey to soften. One orientation everywhere also
// makes the mark recognisable as *the same mark* along a winding trail, which a
// turning one is not. It costs nothing either -- no perpendicular, no square root
// per mark -- but that is the smaller reason.
// **Silhouettes only separate from 5 px.** Rasterised and counted: at 3 px a
// circle and a square are the *same nine pixels* -- there is no room for a corner
// to be missing -- and a diamond is a plus sign of 5. At 5 px they are 25 / 21 /
// 13 and the outlines differ; at 7 px, 49 / 37 / 25. So the shape is a choice
// about 5 px and up, and below that only `Cross` and `Diamond` say anything at
// all. Which of them survives on glass nobody has asked yet.
// `U` is the rock-face mark asked for 2026-08-27: a bracket on the line, its floor
// **turned towards the drop**, which is how the reference sheet (Prosiecka dolina)
// draws a rock face. It is the one shape here whose orientation is its meaning --
// a square rotated is the same square, a U rotated is a different statement -- so
// unlike the others it is turned, using the side the tile already carries: OSM
// order puts the lower ground on the right of the way's direction.
//
// **Snapped to four directions, not turned freely.** Free rotation would put the
// U's edges on diagonals, and on 1-bit there is no grey to soften a staircase --
// the maintainer's own reason for keeping a square axis-aligned. Snapping to
// up/down/left/right keeps every edge on the pixel grid and still says which way
// the ground goes, which is all the mark has to say.
// `Comb` is the cartographic one, and the maintainer's final call 2026-08-27: a
// short bar laid along the line with **three** teeth hanging off the drop side,
// where `U` has two. That is the escarpment hachure every topographic sheet uses,
// and the reference (Prosiecka dolina) is full of them. `U` stays because it is
// lighter at small sizes, but `Comb` is the correct symbol.
enum class MapLineMark : uint8_t { None = 0, Dot, Square, Cross, Circle, Diamond, U, Comb };

// A way record's `roughness` byte is three fields, not one number.
//
// Not in MapClassEnum.h, which is generated from tilegen's class_spec.py and
// says "do not hand-edit" -- a constant added there is lost on the next
// regeneration. The layout is docs/map-data-spec.md, "The rest of the flag
// budget is allocated": bits 0-2 are roughness (0 unknown, 1 best .. 7 worst),
// bits 3-5 are `sac_scale` and 6-7 `trail_visibility`.
//
// **The builder fills bits 3-7 since 2026-08-27** (tilegen `roughness.py`,
// `roughness_byte_for_tags`). Until then it wrote zeros there, which is why
// this mask existed before the fields did -- and why it now matters: an
// unmasked compare against `roughness` reads an alpine path
// (`sac_scale=alpine_hiking` -> 0x20) as surface grade 32, i.e. as worse than
// impassable. Measured on the extracts the tiles are built from
// (`mapbuilder/tools/osm_tag_census.py`, 2026-08-27): 13.1 % of High Tatras
// pedestrian ways carry `sac_scale` and 6.1 % `trail_visibility`, against
// 1.2 % and 0.2 % in Mala Fatra. Sparse, and densest exactly where a walker
// needs it.
static constexpr uint8_t kMapRoughnessValueMask = 0x07;

// `sac_scale`, bits 3-5. 0 unknown, 1 hiking, 2 mountain_hiking,
// 3 demanding_mountain_hiking, 4 alpine_hiking, 5 demanding_alpine_hiking,
// 6 difficult_alpine_hiking. 7 is reserved and never written by the builder, so
// a 7 read here is a corrupt byte and must be treated as unknown.
static constexpr uint8_t kMapSacScaleShift = 3;
static constexpr uint8_t kMapSacScaleMask = 0x07;

// `trail_visibility`, bits 6-7. 0 unknown, 1 obvious (OSM excellent/good),
// 2 patchy (intermediate), 3 absent (bad/horrible/no).
//
// 0 is not "fine". On a Slovak forest track a missing tag is the normal case --
// 99.8 % of Mala Fatra pedestrian ways -- so a style that draws state 0 like
// state 1 is asserting something nobody surveyed.
static constexpr uint8_t kMapTrailVisibilityShift = 6;
static constexpr uint8_t kMapTrailVisibilityMask = 0x03;

static constexpr uint8_t mapSacScale(uint8_t roughness) { return (roughness >> kMapSacScaleShift) & kMapSacScaleMask; }

static constexpr uint8_t mapTrailVisibility(uint8_t roughness) {
  return (roughness >> kMapTrailVisibilityShift) & kMapTrailVisibilityMask;
}

// A stroke that replaces a road class's own, for a way whose `flags` or
// `roughness` says so (docs/map-data-spec.md, "Flag bits"; MapWayFlag in
// MapClassEnum.h).
//
// **Why this exists at all.** Every .tib tile ever written carries `flags` and
// `roughness` per way, and until 2026-08-27 the renderer read neither. So a
// track tagged `access=no` was drawn as the same hairline as an open path: the
// map told a hiker a closed track was open. Measured in the local mirror,
// 24,000 ways carry no_motor, 21,342 no_bicycle and 20,264 no_foot -- 5.1 %,
// 4.5 % and 4.3 % of 474,178 road ways, and 6.7 / 6.0 / 5.7 % at the detail LOD
// where a walker is actually reading the map.
//
// **It replaces, it does not patch.** A matched way is drawn with this rule's
// width, casing and pattern instead of its class's -- there is no per-field
// "inherit", because a flag rule spans classes and there is no single class
// width for it to inherit from. Consequence, and it is the trap to remember: a
// rule matching `bridge` flattens a motorway to whatever width it names. Match
// on bits whose classes you actually mean to restyle, or restrict the rule with
// `when` (docs/map-style.md, "Matching a way's flag bits").
//
// **No tone.** `fill: tone` is deliberately outside this grammar: the generator
// validates a tone against its class's casing and interior width, and a rule
// that spans classes has no one interior to check. Open -- add it when a panel
// pass says a shaded flag treatment is wanted.
struct MapRoadFlagRule {
  // Bits that make this rule match, OR'd together -- any one of them set on the
  // way is a match. 0 with `roughnessMin` 0 means the slot is unused.
  uint16_t flagMask = 0;
  // Lowest `roughness & kMapRoughnessValueMask` that matches. 0 means the rule
  // does not look at roughness at all; 1 upward excludes roughness 0, which is
  // "unknown" and not "smooth".
  uint8_t roughnessMin = 0;
  // Full stroke width in device pixels. Never 0 in a used slot that is not
  // `hidden`, because 0 would read as "hidden" and there is already a word for
  // hidden -- but the generator gets there by flooring, not by refusing: a width
  // that rounds to zero becomes 1 (gen_mapstyle.py, `width = max(width, 1)`).
  // What it refuses is a *missing* width on a rule any variant can draw.
  uint8_t widthPx = 0;
  uint8_t casingPx = 0;
  uint8_t dashPx = 0;
  uint8_t gapPx = 0;
  MapLinePattern pattern = MapLinePattern::Solid;
  // Matched and not drawn. Distinct from width 0 on a class, which also keeps
  // the class out of that rung's tile read (gen_mode_masks.py); this one is a
  // draw-time decision, so the bytes are still read.
  bool hidden = false;
};

// How many flag rules one style may carry. Four, and the number is a flash
// budget rather than a taste judgement: data/mapstyle.json compiles to 14
// distinct MapStyle variants plus the base, so each slot costs 15 copies of
// sizeof(MapRoadFlagRule) -- 10 bytes -- i.e. 150 bytes of flash per slot.
// Raise it when a style needs a fifth, and say what it bought.
static constexpr uint8_t kMapRoadFlagRuleSlots = 4;

struct MapStyle {
  // Road line width per class_id -- index with MapClassId, whose slot count
  // this array matches. 0 means the class is not drawn, which is how
  // mapstyle.json's `hidden: true` and a disabled roads layer arrive here.
  //
  // Widths are whole pixels: IMapCanvas::drawLine takes an int line width, so
  // the style's 0.83px unclassified road is 1px on the panel. The generator
  // rounds, and never rounds a visible class down to 0.
  uint8_t roadWidthPx[kClassEnumSlots];

  // Casing thickness per class, layers.roads.rules[].casing_px. Above 0 the
  // road is drawn as a black stroke at the full width with a white stroke
  // `2 * casing` narrower inside it, i.e. two black edges with the road left
  // white between them -- how a main road reads as bigger than a side street
  // with no colour to spend (docs/map-render-spec.md, "What must be drawn").
  // 0 means a solid black line of the full width.
  uint8_t roadCasingPx[kClassEnumSlots];

  // Dash length per class, 0 for a solid stroke; the gap matches the dash.
  // A broken line is the one mark left that says "not a road you drive on",
  // and two classes need it -- a railway (thick, the modern-map convention)
  // and a watercourse. mapstyle.json's `pattern` field finally means
  // something: it was declared for ferry, railway and aerialway from the
  // start and read by nothing at all.
  MapLinePattern roadPattern[kClassEnumSlots];
  uint8_t roadDashPx[kClassEnumSlots];

  // Gap between dashes. Separate from the dash so a railway can be long runs
  // with short ticks (the modern-map look) while a watercourse is short marks
  // with real space -- one number for both would force the same rhythm.
  uint8_t roadGapPx[kClassEnumSlots];
  // The mark and its size for a `DashMark` class. The generator refuses that
  // pattern without a mark rather than quietly drawing a plain dash: a pattern
  // that needs a mark and has none is a typo, not a choice.
  MapLineMark roadMark[kClassEnumSlots];
  uint8_t roadMarkPx[kClassEnumSlots];

  // Dither tone for the inside of a cased road, layers.roads.rules[]'s
  // `fill: "tone"` plus `tone`. `None` leaves the interior white, which is what
  // every cased road did until 2026-08-25.
  //
  // What it is for: a wide road drawn as two black edges with white between
  // them is a heavy mark, and at the coarse rungs a lot of them is most of the
  // ink. Paper maps draw a motorway as a thin outline with a *shaded* middle
  // instead, so it reads as a wide ribbon rather than a black band. On 1-bit
  // the shading is the same screen-anchored dither an area fill uses
  // (MapAreaTone), so a road and a built-up area cannot disagree about what
  // grey means.
  //
  // Needs `casing > 0` and an interior at least 2 px wide: a 1 px interior
  // cannot carry a period-2 pattern, let alone the period-3 stipple.
  MapAreaTone roadFillTone[kClassEnumSlots];

  // Flag/roughness overrides, applied in file order with the **first match
  // winning** -- so an earlier rule in data/mapstyle.json has priority, and a
  // way matched by one rule is never also restyled by the next.
  //
  // Empty in every style shipped today (an all-zero slot is unused), so the
  // renderer's flag path is dead code until a style file asks for it and the
  // render is byte-identical to the one before this field existed. That is
  // deliberate: a mark on the map is judged on the panel, and there is no
  // device to judge it on yet.
  //
  // A rule is not consulted at all for a class the style hides
  // (roadWidthPx == 0). A hidden class is intersected out of the rung's tile
  // class mask (gen_mode_masks.py), so its ways never reach the renderer -- a
  // flag rule that appeared to un-hide it would draw nothing and read as a bug
  // in the rule.
  MapRoadFlagRule roadFlagRules[kMapRoadFlagRuleSlots];

  // layers.buildings. A ring is drawn as an optional outline plus a hatch --
  // never a solid fill, which on 1-bit swallows the roads around it
  // (docs/map-render-spec.md).
  //
  // `buildingsEnabled` false means the layer is never even read. That is a
  // performance decision, not a cosmetic one: buildings were 277 KB of the
  // 364 KB a four-tile viewport read (docs/map-data-spec.md, "RAM budget"), so
  // "read it and skip drawing" would pay the whole SD cost for nothing.
  bool buildingsEnabled;
  uint8_t buildingOutlinePx;
  // A tone and a hatch are alternatives, not a stack: the style picks one by
  // its `fill`. A tone is what a built-up area wants (MapAreaTone.h); the hatch
  // is kept because a big single area -- a lake, a forest -- can carry lines.
  MapAreaTone buildingTone;
  MapAreaFill::Pattern buildingHatch;
  uint8_t buildingHatchSpacingPx;

  // layers.water. Lines (waterways) and areas (lakes) come from the same layer;
  // an area is a closed ring (IMapSource.h).
  //
  // Width per water class (MapWaterClass: unknown, river, stream, lake), so a
  // river can be wider than a ditch. That became possible on 2026-08-05, when
  // the builder started writing a water class byte; before that every water
  // record was class 0 and mapstyle.json's river/stream rules could not reach
  // the device at all. 0 means that class is not drawn.
  //
  // `waterEnabled` gates the read, not the draw -- see buildings above.
  bool waterEnabled;
  uint8_t waterLinePx[kWaterClassSlots];
  // Border thickness for a water *area* -- a lake's ring -- per class.
  //
  // Separate from waterLinePx since 2026-08-26. The two were one number, so a
  // lake's border and a river's stroke width could not be set apart: widen the
  // river and the lake outline widened with it. Defaults to the class's line
  // width when a style says nothing, so an existing style draws the same.
  uint8_t waterOutlinePx[kWaterClassSlots];
  // Dash length per water class, 0 for a solid stroke. Same reason as
  // roadDashPx: a solid black line is a road, and a stream drawn as one sends
  // the eye down the river instead of the road beside it.
  MapLinePattern waterPattern[kWaterClassSlots];
  uint8_t waterDashPx[kWaterClassSlots];
  uint8_t waterGapPx[kWaterClassSlots];

  // Casing per water class, exactly a road's: above 0 the stroke is drawn black
  // at the full width with a white stroke `2 * casing` narrower inside it. Added
  // 2026-08-27 so a *line* can carry a surface.
  //
  // **Why a stream wanted one.** The Danube reads as an obstacle because it
  // arrives as a closed ring and the ring branch tones it. A stream arrives as an
  // open way -- `mapWayIsClosedRing` is the only thing that tells the two apart --
  // so it could only ever be a stroke, and `fill: tone` on a stream rule drew
  // nothing at all: `toneRing` is called in the ring branch and nowhere else. A
  // walker crossing a stream cares as much as a rider crossing the Danube, and
  // the mark has to say so.
  uint8_t waterCasingPx[kWaterClassSlots];

  // Tone per water class rather than one for the layer. It was a single
  // `waterTone` until 2026-08-27, which meant a lake and a river had to agree
  // about what water looks like, and a *toned stroke* had no tone of its own to
  // read at all. The ring branch and the stroke branch both index this.
  MapAreaTone waterTone[kWaterClassSlots];
  MapAreaFill::Pattern waterHatch;
  uint8_t waterHatchSpacingPx;
  // White waves on a dark surface is the whole point of the water fill: a tone
  // dense enough to read as water at a glance, with the pattern knocked out of
  // it rather than added on top. Every other layer's hatch is black on white,
  // so this is a per-layer choice, not a global one.
  bool waterHatchWhite;

  // layers.landuse. Forest and built-up areas share one tile layer and are
  // drawn at different depths -- built-up under everything, forest above it --
  // so the renderer walks the layer once per class (docs/map-data-spec.md,
  // "A tile is a storage unit, not a render unit").
  //
  // Indexed by MapLanduseClass. Class 0 is unused: the builder only writes a
  // record whose tags mapped to a real class.
  bool landuseEnabled;
  uint8_t landuseOutlinePx[kLanduseClassSlots];
  MapAreaTone landuseTone[kLanduseClassSlots];
  MapAreaFill::Pattern landuseHatch[kLanduseClassSlots];
  uint8_t landuseHatchSpacingPx[kLanduseClassSlots];
  // A dashed boundary, per landuse class. 0 in either field means solid.
  //
  // Dashes on an area outline exist for one reason: a forest edge on foot is a
  // real thing to navigate by, and a dot tone ending is not an edge -- nothing
  // says whether the wood stops there or the mapping does. A boundary line
  // settles it (docs/map-style.md, "Borders").
  uint8_t landuseOutlineDashPx[kLanduseClassSlots];
  uint8_t landuseOutlineGapPx[kLanduseClassSlots];

  // layers.contours. Stroke width per contour class (MapContourClass: minor,
  // index), 0 for a class this rung does not draw -- which is how rung 1 shows
  // only the 100 m lines and rungs 5-6 only the 500 m ones
  // (docs/contours-plan.md, "Interval per LOD").
  //
  // No tone, no hatch and no dash: a contour is a line, and "a 1 px line under
  // a checkerboard becomes a dashed line" (docs/map-render-spec.md, "1-bit
  // rules"), which reads as a footpath. Weight is the only separator there is.
  //
  // `contoursEnabled` gates the read, not the draw -- see buildings above. Ride
  // and cycle resolve it false, so those modes never open the layer at all.
  bool contoursEnabled;
  uint8_t contourWidthPx[kContourClassSlots];

  // Per relief class, so a cliff is not the same line as an index contour. It was
  // width alone until 2026-08-27, which made them identical marks at 2 px -- and
  // on a hike map those two must never be confused: one says "a hundred metres of
  // height", the other says "you fall here".
  //
  // `contourTickPx` is how far a `Hachured` comb reaches off the line and
  // `contourGapPx` how often one is hung; both are ignored by every other
  // pattern.
  MapLinePattern contourPattern[kContourClassSlots];
  uint8_t contourTickPx[kContourClassSlots];
  uint8_t contourGapPx[kContourClassSlots];
  // A `DashMark` relief class needs a real dash length. It briefly borrowed the
  // gap, which is a number nobody chose for it.
  uint8_t contourDashPx[kContourClassSlots];
  MapLineMark contourMark[kContourClassSlots];
  uint8_t contourMarkPx[kContourClassSlots];

  // Height numbers on the index contours. Two or three on a frame is the whole
  // design: the rest of the ladder is countable from them, and a number per
  // line would bury the map it is describing.
  //
  // 0 px switches them off. The number is drawn into a white knockout box
  // rather than a halo, because a gap in the line is what a paper contour map
  // does and it reads better than letters floating on the line they belong to.
  // Not rotated to follow the contour -- the canvas has no rotated text, so
  // this is a known deviation from paper practice (docs/contours-plan.md).
  uint8_t contourLabelPx;
  bool contourLabelBold;
  // White outline around the digits, in pixels. 0 draws none, which on a dotted
  // area fill makes a number hard work: the dots sit inside the counters.
  uint8_t contourLabelHaloPx;
  // How many may land on one frame. Held small on purpose.
  uint8_t contourLabelMax;
  // Minimum spacing between two numbers, so they spread instead of clustering
  // wherever the stream happened to offer straight contour first.
  uint8_t contourLabelMinGapPx;

  // Village/town dot, layers.places.dot_radius_px doubled. 0 means places are
  // not drawn.
  uint8_t placeDotDiameterPx;

  // layers.places label fields (docs/place-labels.md). Two tiers, because a
  // city and a hamlet are not the same news: `Label` applies to rank <= 1
  // (city/town), `LabelMinor` to everything above it
  // (village/suburb/hamlet/farm -- mapbuilder/tilegen/build_config.json's
  // place_ranks). Weight and size carry the hierarchy; there is no colour to
  // carry it with, and all-caps was rejected as harder to read at a glance.
  //
  // Either size at 0 hides that tier's labels; both at 0 means dots only, which
  // is what the firmware did before labels existed.
  uint8_t placeLabelPx;
  bool placeLabelBold;
  uint8_t placeLabelMinorPx;
  bool placeLabelMinorBold;

  // Gap between the dot and its label box, device pixels.
  uint8_t placeLabelOffsetPx;

  // Two ways to keep a name readable over a road network, and they are
  // alternatives rather than a stack: an opaque white box under the text
  // (`placeLabelBg`, the original spec), or a white halo drawn around the
  // glyphs themselves (`placeLabelHaloPx`). The halo is the compact one -- it
  // knocks out only the pixels the letters need instead of a rectangle of map --
  // so it is the default and the box is the fallback for a style that wants the
  // stronger contrast. With the box on, the halo is not drawn: the box already
  // separates the text from everything under it, and the halo would only eat
  // into the box's own white.
  bool placeLabelBg;
  uint8_t placeLabelBgPadPx;
  uint8_t placeLabelBgBorderPx;
  uint8_t placeLabelHaloPx;

  // Declutter, all device pixels except the last two.
  //
  // `placeMaxLabels` is a render-cost backstop, not the declutter mechanism:
  // labels are placed rank-first and a candidate that collides is dropped, so
  // the picture is already thinned before this cap is reached
  // (docs/place-labels.md, "Greedy, not a zoom table").
  //
  // `placeLabelGapPx` inflates a label's box before it is tested against the
  // labels already placed, so two names cannot end up touching.
  //
  // `placeLabelRouteOverlapPct` is how much of a label's box may sit over the
  // route -- 0 forbids any overlap at all. The route is the one thing on screen
  // a label must not obscure, and forbidding it outright loses names along a
  // road the rider is following, which is where they are needed most. So a few
  // per cent are allowed and a name laid across the line is not.
  //
  // `placeLabelMaxWidthPx` caps a label's text width; a longer name is
  // truncated with an ellipsis. 0 means no cap.
  uint8_t placeMaxLabels;
  // Per-tier caps, `max_labels_major` and `max_labels_minor`. Major is city and
  // town (place rank <= 1), minor is everything smaller.
  //
  // Why a cap per tier and not just the total: the total is a cost backstop and
  // says nothing about what a frame should look like. "Six towns and two
  // villages" and "two towns and six villages" are the same total and different
  // maps, and which one is right changes with the rung -- at 45 m/px the towns
  // are the skeleton and the villages are texture, at 6 m/px it is the other
  // way round.
  //
  // Absent from the style file means "the total, i.e. no extra restriction", so
  // a style that never mentions them behaves exactly as before. 0 means none of
  // that tier, which is a real thing to ask for: a rung that names towns only.
  // `placeMaxLabels` still applies on top of both.
  uint8_t placeMaxLabelsMajor;
  uint8_t placeMaxLabelsMinor;
  uint8_t placeLabelGapPx;
  uint8_t placeLabelRouteOverlapPct;
  uint16_t placeLabelMaxWidthPx;

  // layers.points -- the POI marks from the .tip point layer
  // (../../../docs/point-file-spec.md). A square outline with a category glyph
  // inside it and one corner flag when the point carries a condition
  // (docs/map-render-spec.md, "Point mark vocabulary"; MapPointMarks.h for why
  // the glyphs are primitives and not icons).
  //
  // Device pixels and they do not scale with mpp: like the route arrow, a mark
  // is a screen decoration and has to stay legible at every zoom rung. A square
  // that shrank with the zoom would be a dot at the coarse rungs, which is
  // already what a place is.
  //
  // `pointSquarePx == 0` hides the layer, and hidden means the shards are never
  // opened -- the same performance decision as `buildingsEnabled`, and it
  // matters more here because a shard is a separate file per 39 km.
  //
  // The two `*Enabled` flags are per kind rather than per category, which is
  // exactly what the `kind` byte was for: "safety without landmarks" is a byte
  // test and not a category allowlist (docs/map-data-spec.md, "Two kinds of
  // point, one file"). Category filtering is a render-time mask the UI sets
  // (MapPointSource::Config::categoryMask), not a style field -- `Nearby ->
  // Show on map` is a temporary view, not a style edit.
  bool pointsSafetyEnabled;
  bool pointsLandmarkEnabled;
  uint8_t pointSquarePx;
  uint8_t pointBorderPx;
  uint8_t pointGlyphPx;
  // Leg of the corner triangle. 0 draws no flag at all, which makes a flagged
  // point look confirmed -- so it is a debug setting, never a style choice.
  uint8_t pointFlagPx;

  // Clustering (docs/map-render-spec.md, "POI clustering"): points whose
  // screen positions land within pointClusterRadiusPx of each other merge
  // into one tile before drawing. A tile with more than one distinct category
  // lays each out in a pointClusterCellPx square grid (ceil(sqrt(n)) side),
  // same-category points collapsing to one mark. Screen pixels, so the same
  // two points that merge at a coarse zoom pull apart at a fine one with no
  // per-rung tuning. 0 disables clustering: every point draws its own mark.
  uint8_t pointClusterRadiusPx;
  uint8_t pointClusterCellPx;

  // layers.route. The route is distinguished from the roads by width alone --
  // there is no colour on 1-bit e-ink -- so this is deliberately wider than any
  // road class. 0 means the route is not drawn even when one is loaded.
  //
  // The arrow is the filled head at the far end (docs/map-render-spec.md item
  // 3): without it the line does not say which way round the route runs.
  // `routeArrowLenPx` is tip-to-base, `routeArrowWidthPx` is the base. Both are
  // device pixels and do not scale with mpp: a screen decoration has to stay
  // legible at every zoom rung. Either at 0 draws the line and no head.
  uint8_t routeWidthPx;
  uint8_t routeArrowLenPx;
  uint8_t routeArrowWidthPx;

  // device.marker_x_px / marker_y_px -- the viewport anchor, which is also
  // where the marker itself sits (MapViewport::kAnchorScreenX/Y).
  int16_t markerXPx;
  int16_t markerYPx;

  // Position puck, layers.position: a white disc of this radius under a black
  // ring of `puckRingPx`, with a black heading arrow `puckArrowPx` long inside
  // it. Radius 0 draws the bare arrow and no disc -- a bare triangle vanishes
  // into a thick route, so that is a debug shape rather than the spec's.
  uint8_t puckRadiusPx;
  uint8_t puckRingPx;
  uint8_t puckArrowPx;
};

// The widest stroke this style can draw around a way's own geometry, in device
// pixels.
//
// This is the margin an off-screen test has to allow: a way whose points all
// sit just off the panel can still put ink on it, because a stroke is drawn
// centred on the line. MapTileSource uses it to decide what cannot possibly be
// visible (Config::rejectMarginPx).
//
// The **full** width, not half of it: half is what a centred stroke actually
// extends, and doubling that is one cheap pixel of paranoia against a rounding
// step or an off-centre stack (MapStroke::stackFor's `first` biases odd counts
// by half a pixel).
//
// Area fills are not in here on purpose. A tone or a hatch is clipped to the
// ring that carries it (MapAreaFill), so it never reaches past the geometry;
// only outlines and line strokes do.
inline uint8_t mapStyleMaxStrokePx(const MapStyle& style) {
  uint8_t widest = 0;
  const auto take = [&widest](uint8_t candidate) {
    if (candidate > widest) widest = candidate;
  };
  for (uint8_t i = 0; i < kClassEnumSlots; ++i) take(style.roadWidthPx[i]);
  for (uint8_t i = 0; i < kWaterClassSlots; ++i) take(style.waterLinePx[i]);
  for (uint8_t i = 0; i < kLanduseClassSlots; ++i) take(style.landuseOutlinePx[i]);
  take(style.buildingOutlinePx);
  take(style.routeWidthPx);
  // A flag rule can name a width no class has, and the reject margin has to
  // allow for it or a flag-widened way just off the panel loses its ink.
  for (uint8_t i = 0; i < kMapRoadFlagRuleSlots; ++i) take(style.roadFlagRules[i].widthPx);
  return widest;
}
