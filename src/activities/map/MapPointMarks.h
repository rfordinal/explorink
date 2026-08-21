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
namespace MapPointMarks {

// The mark's half-width, for a source's off-screen margin: a square is drawn
// centred on its point, so this is how far past the panel a point can still put
// ink on it.
inline int16_t reachPx(const MapStyle& style) { return static_cast<int16_t>(style.pointSquarePx / 2 + 1); }

// One mark, centred on (x, y) in screen pixels. Draws nothing when the style
// hides the layer (pointSquarePx == 0) or the kind is filtered out.
void draw(IMapCanvas& canvas, const MapPointRef& point, const MapStyle& style);

}  // namespace MapPointMarks
