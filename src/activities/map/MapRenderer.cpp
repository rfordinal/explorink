#include "MapRenderer.h"

#include "MapAreaFill.h"

namespace {

// Unit-ish direction vectors for the 16 snapped headings, scaled by 8 (so a
// 22.5 degree component like sin(22.5) becomes the integer 3/8, and the
// diagonal 0.7071 factor becomes 6/8) -- avoids any sin/cos call, matching
// the "no per-frame trig" design decision. Index = MapHeading value.
struct Vec2 {
  int dx, dy;
};
constexpr Vec2 kHeadingDir[16] = {
    {0, -8},   // N
    {3, -7},   // NNE
    {6, -6},   // NE
    {7, -3},   // ENE
    {8, 0},    // E
    {7, 3},    // ESE
    {6, 6},    // SE
    {3, 7},    // SSE
    {0, 8},    // S
    {-3, 7},   // SSW
    {-6, 6},   // SW
    {-7, 3},   // WSW
    {-8, 0},   // W
    {-7, -3},  // WNW
    {-6, -6},  // NW
    {-3, -7},  // NNW
};

// One way's worth of line drawing at a given width and ink.
void strokeWay(IMapCanvas& canvas, const MapWayRef& way, int lineWidth, MapInk ink) {
  if (lineWidth <= 0) return;
  for (uint16_t i = 1; i < way.pointCount; ++i) {
    canvas.drawLine(way.xs[i - 1], way.ys[i - 1], way.xs[i], way.ys[i], lineWidth, ink);
  }
}

// Width the style draws this class at, or 0 for "do not draw". A class id past
// the enum's slots can only come from a corrupt tile; reserved slots carry
// width 0 anyway, so this guard is about the array bound, not the style.
int roadWidthFor(const MapStyle& style, const MapWayRef& way) {
  if (way.classId >= kClassEnumSlots) return 0;
  return style.roadWidthPx[way.classId];
}

}  // namespace

void MapRenderer::render(IMapCanvas& canvas, IMapSource& source, const MapViewState& state, const MapStyle& style) {
  // Draw order is fixed by docs/map-render-spec.md, "What must be drawn":
  // buildings, then water, then roads, then places. Buildings and water go
  // under the road network on purpose -- they are context, and a road crossing
  // them has to stay readable.
  //
  // Both layers are skipped entirely when the style has them off, not read and
  // then not drawn: buildings alone were 277 KB of the 364 KB a four-tile
  // viewport read (docs/map-data-spec.md, "RAM budget").
  if (style.buildingsEnabled && source.beginBuildings()) {
    MapWayRef ring;
    while (source.nextBuilding(ring)) {
      MapAreaFill::hatchRing(canvas, ring.xs, ring.ys, ring.pointCount, style.buildingHatch,
                             style.buildingHatchSpacingPx, MapInk::Black);
      MapAreaFill::outlineRing(canvas, ring.xs, ring.ys, ring.pointCount, style.buildingOutlinePx, MapInk::Black);
    }
  }

  if (style.waterEnabled && source.beginWater()) {
    MapWayRef way;
    while (source.nextWater(way)) {
      // A closed ring is a lake, an open one a waterway. Same layer, same
      // record shape -- the ring is the only thing that tells them apart
      // (IMapSource.h, mapWayIsClosedRing).
      if (mapWayIsClosedRing(way)) {
        MapAreaFill::hatchRing(canvas, way.xs, way.ys, way.pointCount, style.waterHatch, style.waterHatchSpacingPx,
                               MapInk::Black);
        MapAreaFill::outlineRing(canvas, way.xs, way.ys, way.pointCount, style.waterLinePx, MapInk::Black);
      } else {
        MapAreaFill::outlineRing(canvas, way.xs, way.ys, way.pointCount, style.waterLinePx, MapInk::Black);
      }
    }
  }

  // One pass per layer, each pass pulled straight from the source and
  // forgotten. Nothing is collected first -- see IMapSource.h.
  //
  // Roads are two passes over the same layer, and the order is a correctness
  // requirement, not a preference: **all black strokes first, then all white
  // fills**. Finishing one road before starting the next leaves a broken
  // casing at every junction, because the later road's white fill punches
  // through the earlier road's black edge (docs/map-render-spec.md, "What must
  // be drawn"). beginWays() is rewindable exactly so this can be two walks
  // instead of one buffered pass (IMapSource.h:22-27).
  if (source.beginWays()) {
    MapWayRef way;
    while (source.nextWay(way)) {
      // Width 0 is the style hiding this class outright (mapstyle.json's
      // `hidden`). Distinct from the mode mask, which drops the way earlier,
      // in the source, and per travel mode rather than for every mode.
      strokeWay(canvas, way, roadWidthFor(style, way), MapInk::Black);
    }
  }

  // Second walk, white: the inside of every cased road. A class with
  // casing 0 is not touched here, so it stays the solid black line the first
  // pass drew.
  if (source.beginWays()) {
    MapWayRef way;
    while (source.nextWay(way)) {
      const int lineWidth = roadWidthFor(style, way);
      if (lineWidth == 0) continue;
      const int casing = style.roadCasingPx[way.classId];
      if (casing == 0) continue;
      // The generator guarantees 2 * casing < width, so this is at least 1.
      strokeWay(canvas, way, lineWidth - 2 * casing, MapInk::White);
    }
  }

  const int dotDiameter = style.placeDotDiameterPx;
  if (dotDiameter > 0 && source.beginPlaces()) {
    MapPlaceRef place;
    while (source.nextPlace(place)) {
      canvas.fillRoundedRect(place.x - dotDiameter / 2, place.y - dotDiameter / 2, dotDiameter, dotDiameter,
                             dotDiameter / 2, MapInk::Black);
    }
  }

  // No marker draw here -- MapActivity draws its own mode-specific one (ring +
  // dot/arrow, sized per hike/cycle/ride) after this call returns. Drawing the
  // generic puck here as well left it peeking out from under the real marker
  // whenever the real one was smaller, e.g. hike's dot.
  //
  // The reason that marker bypasses IMapCanvas no longer holds: it needed a
  // white halo fill, and IMapCanvas can paint white now (MapInk). Moving it
  // behind the canvas is what would make the laptop preview show the marker
  // the device actually draws -- until then the preview shows the style's puck
  // (drawMarker below) and the device shows the mode marker. See
  // docs/map-style.md.
  (void)state;
}

void MapRenderer::drawMarker(IMapCanvas& canvas, int16_t cx, int16_t cy, MapHeading heading, const MapStyle& style) {
  // Puck under the arrow: a black disc, then a white one inside it, which
  // leaves a ring of `puckRingPx`. Radius 0 skips both and draws the bare
  // arrow -- see MapStyle.h.
  const int radius = style.puckRadiusPx;
  if (radius > 0) {
    const int diameter = 2 * radius;
    canvas.fillRoundedRect(cx - radius, cy - radius, diameter, diameter, radius, MapInk::Black);
    const int inner = radius - style.puckRingPx;
    if (inner > 0) {
      canvas.fillRoundedRect(cx - inner, cy - inner, 2 * inner, 2 * inner, inner, MapInk::White);
    }
  }

  // Arrow proportions off one style number: `arrow_px` is the tip-to-tail
  // length, the tail sits a quarter of it behind the centre, and the base is
  // half of it wide. At the style's 28 px that reproduces the triangle this
  // drew before the puck existed (14 forward, 7 back, 7 to each side).
  const int arrow = style.puckArrowPx;
  if (arrow <= 0) return;
  const int tipLen = arrow / 2;
  const int tailLen = arrow / 4;
  const int halfWidth = arrow / 4;

  const Vec2& dir = kHeadingDir[static_cast<uint8_t>(heading)];
  const Vec2 perp{-dir.dy, dir.dx};  // rotate 90 degrees, same /8 scale

  const int tipX = cx + dir.dx * tipLen / 8;
  const int tipY = cy + dir.dy * tipLen / 8;
  const int baseCx = cx - dir.dx * tailLen / 8;
  const int baseCy = cy - dir.dy * tailLen / 8;
  const int baseLeftX = baseCx + perp.dx * halfWidth / 8;
  const int baseLeftY = baseCy + perp.dy * halfWidth / 8;
  const int baseRightX = baseCx - perp.dx * halfWidth / 8;
  const int baseRightY = baseCy - perp.dy * halfWidth / 8;

  const int xs[3] = {tipX, baseLeftX, baseRightX};
  const int ys[3] = {tipY, baseLeftY, baseRightY};
  canvas.fillPolygon(xs, ys, 3, MapInk::Black);
}
