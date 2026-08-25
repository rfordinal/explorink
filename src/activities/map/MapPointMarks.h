#pragma once

#include <cstdint>

#include "IMapCanvas.h"
#include "IMapPointSource.h"
#include "MapStyle.h"

// Draws one POI mark: a square outline, a category glyph inside it, and one
// corner flag when the point carries a condition.
//
// The vocabulary is fixed by ../../../docs/map-render-spec.md, "Point mark
// vocabulary": a filled circle is a settlement, a square is "this exists in the
// data", a balloon with a tip is "I marked this place myself". So a POI is a
// square, and `Set destination` turning it into the destination pin's balloon is
// visible without reading a word.
//
// ## The glyphs are primitives, not icons -- decided on a render
//
// map-render-spec.md left one thing open for T-221: draw each glyph from the
// existing IMapCanvas primitives, or grow the interface a drawIcon fed by the
// Lucide pipeline the pins use. Settled 2026-08-21 by rendering ten glyphs at
// real size both ways (mapbuilder/tools/poi_glyph_probe.py, the contact sheet in
// docs/design-shots/poi-glyphs.png):
//
// - **Lucide at 9 px is unreadable.** Those SVGs are a 24 px design with a
//   1.5 px stroke. Thresholded to 1 bpp at 9 px the strokes break into dots:
//   the droplet, the fuel pump, the cross, the pill, the life buoy and the
//   handset all came out as noise. At 11 px, cropped back to 9, they were
//   fragments rather than shapes.
// - **Primitives read at 9 px.** Every one of the ten is legible as a shape: a
//   filled drop, a tent, a house, a bed, a pump, a cross, a capsule, a ring
//   with spokes, an exclamation mark, a bus.
//
// So no new primitive on two canvases, and no icon table in flash. It also
// keeps the mark one drawing decision: the flag is drawn in the same pass, over
// the same square, and can be nudged if a glyph and a flag ever collide.
//
// **Superseded, 2026-08-25.** The comparison above judged Lucide against
// primitives at 9px; it never judged a bitmap *designed for* a bigger mark.
// The maintainer supplied a hand-drawn 24px set built for cartographic
// symbology rather than UI icons (poi_icons.h, scripts/gen_poi_icons.py,
// src/components/icons/poi_icons_24.png -- edit the PNG, rerun the script),
// verified on a real screen (explorink-simulator, Samsung S10, 2026-08-24) at
// 1:1 pixel mode: legible, clearly better than a primitive squeezed into
// 11px. `drawMarkAt()` below draws it for every category that has one and
// falls back to the primitives otherwise -- today that fallback is dead code
// (all ten categories have an icon), kept for whatever category shows up
// next without one yet. So there *is* an icon table in flash, contrary to
// the line above it: read that paragraph as history, not as current
// behaviour. Two categories' artwork (lodging, rescue) reads ambiguously at
// this size and are the maintainer's to redraw in poi_icons_24.png -- that is
// a content fix, not a reason to reconsider the icon-table decision itself.
//
// Two further notes worth keeping:
//
// - The emergency-phone glyph is an **exclamation mark**, not a handset. A
//   handset silhouette at 9 px came out a diagonal slash in the probe. The
//   square means "call for help from here", which is what the point is.
// - `gen_icons.py` needs `rsvg-convert`, which is not installed on the build
//   machine (2026-08-21); the probe used inkscape instead. That is a second,
//   weaker reason -- the pixels are the reason.
//
// ## Honesty: one flag, and not for not_potable
//
// A point whose flags carry unverified, restricted, seasonal or fee draws one
// small filled triangle in the top-right corner
// (kPointFlaggedOnMapMask, MapPointTypes.h). One mark for all four: at this size
// a rider can tell "there is a condition" from "there is none" and nothing
// finer, and the exact condition belongs on the detail screen.
//
// `not_potable` is not in that mask and never reaches this code: the writer
// drops such a point from water entirely (../../../docs/point-file-spec.md), so
// a square with a water glyph always means candidate for drinking water.
// ## POI clustering, added 2026-08-25
//
// At a coarse zoom, points that are metres apart on the ground land on top of
// each other in screen pixels -- an area with a dozen shelters would draw a
// dozen overlapping squares, unreadable and no more informative than one.
// `drawAll()` merges points whose screen positions land within
// style.pointClusterRadiusPx of each other into one cluster before drawing:
//
// - **One category present**: draws exactly the mark `draw()` used to, at
//   the centroid of the merged points -- an isolated point looks identical to
//   before clustering existed.
// - **More than one category present**: lays each distinct category out in a
//   style.pointClusterCellPx square grid, `ceil(sqrt(n))` to a side, centred
//   on the centroid. Two points of the *same* category collapse to one tile
//   slot -- the honesty rule is "there is a shelter here", not "there are
//   three".
//
// Merging is in screen pixels, not on the ground, which is what makes zoom
// work with no per-rung tuning: the same two points are farther apart on
// screen at a finer mpp, so they pull apart on their own as the rider zooms
// in, and re-merge zooming out. `style.pointClusterRadiusPx == 0` disables
// this entirely and restores one-mark-per-point.
//
// A cluster's category mask assumes every point in it is the *same kind*
// (MapPointKind) -- true today because landmarks are off by default
// (pointsLandmarkEnabled) and unbuilt (T-305). If landmarks start sharing the
// safety categories' id space while both are on, a cluster mixing kinds would
// misread as one category when it is really two; revisit this the day T-305
// lands.
namespace MapPointMarks {

// The mark's half-width, for a source's off-screen margin: a square is drawn
// centred on its point, so this is how far past the panel a point can still put
// ink on it. Accounts for the worst-case cluster tile (every category present,
// clustered from as far as pointClusterRadiusPx away) when clustering is on.
int16_t reachPx(const MapStyle& style);

// One mark, centred on (x, y) in screen pixels. Draws nothing when the style
// hides the layer (pointSquarePx == 0) or the kind is filtered out.
void draw(IMapCanvas& canvas, const MapPointRef& point, const MapStyle& style);

// Draws every point `source` yields, clustering first per style.
// pointClusterRadiusPx (see above). Same kind/enable filtering as draw().
void drawAll(IMapCanvas& canvas, IMapPointSource& source, const MapStyle& style);

}  // namespace MapPointMarks
