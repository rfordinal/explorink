#include "MapRenderer.h"

#include <cmath>

#include "MapAreaClass.h"
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

// One landuse class, drawn as tone then hatch then outline. Called once per
// class rather than once per record because forest and built-up sit at
// different depths in the draw order and share a single tile layer.
void drawLanduseClass(IMapCanvas& canvas, IMapSource& source, const MapStyle& style, const MapLanduseClass wanted) {
  const uint8_t index = static_cast<uint8_t>(wanted);
  if (style.landuseTone[index] == MapAreaTone::None && style.landuseHatch[index] == MapAreaFill::Pattern::None &&
      style.landuseOutlinePx[index] == 0) {
    return;  // nothing to draw for this class, so do not walk the layer for it
  }
  if (!source.beginLanduse()) return;
  MapWayRef ring;
  while (source.nextLanduse(ring)) {
    if (ring.classId != index) continue;
    MapAreaFill::toneRing(canvas, ring.xs, ring.ys, ring.pointCount, style.landuseTone[index]);
    MapAreaFill::hatchRing(canvas, ring.xs, ring.ys, ring.pointCount, style.landuseHatch[index],
                           style.landuseHatchSpacingPx[index], MapInk::Black);
    MapAreaFill::outlineRing(canvas, ring.xs, ring.ys, ring.pointCount, style.landuseOutlinePx[index], MapInk::Black);
  }
}

// The route: one thick black polyline, with a filled arrowhead at the far end
// (docs/map-render-spec.md item 3). Streamed, never collected -- two points are
// live at a time whatever the route's length (IMapRouteSource.h).
//
// Coordinates are int32_t. In follow mode the far end of a long route is
// millions of pixels off screen, and the canvas clips per segment, so an honest
// off-screen coordinate is cheap while a wrapped int16_t one is a line drawn
// across the middle of the map.
void drawRoute(IMapCanvas& canvas, IMapRouteSource& route, const MapStyle& style) {
  if (style.routeWidthPx == 0) return;
  if (!route.beginRoute()) return;

  int32_t x = 0;
  int32_t y = 0;
  if (!route.nextRoutePoint(x, y)) return;

  int32_t prevX = x;
  int32_t prevY = y;
  // The segment before the last point, kept for the arrowhead's direction. A
  // route is at least two points (the format enforces it), so by the end of the
  // walk this is always a real segment.
  int32_t tailX = x;
  int32_t tailY = y;
  bool drewAny = false;

  while (route.nextRoutePoint(x, y)) {
    canvas.drawLine(static_cast<int>(prevX), static_cast<int>(prevY), static_cast<int>(x), static_cast<int>(y),
                    style.routeWidthPx, MapInk::Black);
    tailX = prevX;
    tailY = prevY;
    prevX = x;
    prevY = y;
    drewAny = true;
  }
  if (!drewAny) return;

  // Arrowhead at the far end, pointing along the last segment. `arrow_len_px`
  // is tip-to-base and `arrow_width_px` is the base, both device pixels: screen
  // decorations stay legible at any map zoom, so they do not scale with mpp
  // (data/mapstyle.json, layers.route).
  const int arrowLen = style.routeArrowLenPx;
  const int arrowWidth = style.routeArrowWidthPx;
  if (arrowLen <= 0 || arrowWidth <= 0) return;

  const double dx = static_cast<double>(prevX - tailX);
  const double dy = static_cast<double>(prevY - tailY);
  const double len = std::sqrt(dx * dx + dy * dy);
  // A zero-length last segment has no direction to point along. Two identical
  // points in a row are deduped by the builder, so this is a corrupt file rather
  // than a normal route -- draw the line, skip the head.
  if (len < 1e-6) return;
  const double ux = dx / len;
  const double uy = dy / len;

  const double baseX = static_cast<double>(prevX) - ux * arrowLen;
  const double baseY = static_cast<double>(prevY) - uy * arrowLen;
  const double halfWidth = arrowWidth / 2.0;
  // Perpendicular, same length scale.
  const double px = -uy * halfWidth;
  const double py = ux * halfWidth;

  const int xs[3] = {static_cast<int>(prevX), static_cast<int>(std::lround(baseX + px)),
                     static_cast<int>(std::lround(baseX - px))};
  const int ys[3] = {static_cast<int>(prevY), static_cast<int>(std::lround(baseY + py)),
                     static_cast<int>(std::lround(baseY - py))};
  canvas.fillPolygon(xs, ys, 3, MapInk::Black);
}

}  // namespace

void MapRenderer::render(IMapCanvas& canvas, IMapSource& source, const MapViewState& state, const MapStyle& style,
                         IMapRouteSource* route, MapRenderTiming* timing) {
  // Instrumentation only. `mark` holds the last stamp; each layer's field gets
  // the delta since it. With no timing, or no clock in it, both of these are a
  // null check and nothing else -- no clock call, no pixel changed.
  const auto stamp = [timing]() -> uint32_t { return (timing && timing->nowMs) ? timing->nowMs() : 0; };
  const auto lap = [&stamp](uint32_t& field, uint32_t& mark) {
    const uint32_t now = stamp();
    field = now - mark;
    mark = now;
  };
  uint32_t mark = stamp();

  // Draw order is fixed and not arbitrary (docs/map-data-spec.md, "A tile is a
  // storage unit, not a render unit"): built-up area, green area, water,
  // buildings, road casings, road fills, then places. Landuse tones are
  // background; everything above has to stay readable over them, which is why a
  // tone is a sparse pattern and not a grey block.
  //
  // A layer the style has off is never read, not read and skipped: buildings
  // alone were 277 KB of the 364 KB a four-tile viewport read
  // (docs/map-data-spec.md, "RAM budget").
  if (style.landuseEnabled) {
    // Two walks over one layer, built-up first: a park inside a housing estate
    // has to land on top of it, and a single walk would draw them in whatever
    // order the tile happens to store.
    drawLanduseClass(canvas, source, style, MapLanduseClass::BuiltUp);
    drawLanduseClass(canvas, source, style, MapLanduseClass::Forest);
  }
  if (timing) lap(timing->landuseMs, mark);

  // Both gates read: the style says whether buildings are drawn at all, the view
  // says whether this rung draws them. Either one false and the layer is not
  // opened -- and not opening it is what saves the card read, not just the
  // drawing (MapStyle::buildingsEnabled, MapViewState::drawBuildings).
  if (style.buildingsEnabled && state.drawBuildings && source.beginBuildings()) {
    MapWayRef ring;
    while (source.nextBuilding(ring)) {
      // Tone or hatch, whichever the style chose, then the outline on top so a
      // building keeps a crisp edge over its own texture.
      MapAreaFill::toneRing(canvas, ring.xs, ring.ys, ring.pointCount, style.buildingTone);
      MapAreaFill::hatchRing(canvas, ring.xs, ring.ys, ring.pointCount, style.buildingHatch,
                             style.buildingHatchSpacingPx, MapInk::Black);
      MapAreaFill::outlineRing(canvas, ring.xs, ring.ys, ring.pointCount, style.buildingOutlinePx, MapInk::Black);
    }
  }
  if (timing) lap(timing->buildingsMs, mark);

  if (style.waterEnabled && source.beginWater()) {
    MapWayRef way;
    while (source.nextWater(way)) {
      // A closed ring is a lake, an open one a waterway. Same layer, same
      // record shape -- the ring is the only thing that tells them apart
      // (IMapSource.h, mapWayIsClosedRing).
      // Width per water class now that tiles carry one: a river is not a
      // ditch. A class byte past the table can only come from a corrupt
      // record.
      const uint8_t waterClass = way.classId < kWaterClassSlots ? way.classId : 0;
      const int lineWidth = style.waterLinePx[waterClass];
      if (mapWayIsClosedRing(way)) {
        MapAreaFill::toneRing(canvas, way.xs, way.ys, way.pointCount, style.waterTone);
        MapAreaFill::hatchRing(canvas, way.xs, way.ys, way.pointCount, style.waterHatch, style.waterHatchSpacingPx,
                               MapInk::Black);
        MapAreaFill::outlineRing(canvas, way.xs, way.ys, way.pointCount, lineWidth, MapInk::Black);
      } else {
        MapAreaFill::outlineRing(canvas, way.xs, way.ys, way.pointCount, lineWidth, MapInk::Black);
      }
    }
  }
  if (timing) lap(timing->waterMs, mark);

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
  if (timing) lap(timing->roadsMs, mark);

  // The route sits over the roads it follows and under the place dots -- the
  // draw order docs/map-data-spec.md fixes ("built-up area, green area, water,
  // buildings, roads, route, junctions, places"). Over the roads because a
  // route hidden under a casing is not a route; under the dots because a dot on
  // the route is exactly what item 4 of the render spec draws.
  if (route != nullptr) drawRoute(canvas, *route, style);
  if (timing) lap(timing->routeMs, mark);

  const int dotDiameter = style.placeDotDiameterPx;
  if (dotDiameter > 0 && source.beginPlaces()) {
    MapPlaceRef place;
    while (source.nextPlace(place)) {
      canvas.fillRoundedRect(place.x - dotDiameter / 2, place.y - dotDiameter / 2, dotDiameter, dotDiameter,
                             dotDiameter / 2, MapInk::Black);
    }
  }
  if (timing) lap(timing->placesMs, mark);

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
