#include "MapRenderer.h"

namespace {

constexpr int ROAD_LINE_WIDTH = 4;
constexpr int VILLAGE_DOT_DIAMETER = 10;
constexpr int MARKER_TIP_LEN = 14;      // center to tip, pixels
constexpr int MARKER_BASE_HALF_W = 7;   // center to each base corner, pixels

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

}  // namespace

void MapRenderer::render(IMapCanvas& canvas, IMapSource& source, const MapViewState& state) {
  // One pass per layer, each pass pulled straight from the source and
  // forgotten. Nothing is collected first -- see IMapSource.h. The road
  // casing pass is a second beginWays() walk and lands with the casings
  // themselves; today there is one road pass and it draws fills.
  if (source.beginWays()) {
    MapWayRef way;
    while (source.nextWay(way)) {
      for (uint16_t i = 1; i < way.pointCount; ++i) {
        canvas.drawLine(way.xs[i - 1], way.ys[i - 1], way.xs[i], way.ys[i], ROAD_LINE_WIDTH);
      }
    }
  }

  if (source.beginPlaces()) {
    MapPlaceRef place;
    while (source.nextPlace(place)) {
      canvas.fillRoundedRect(place.x - VILLAGE_DOT_DIAMETER / 2, place.y - VILLAGE_DOT_DIAMETER / 2,
                             VILLAGE_DOT_DIAMETER, VILLAGE_DOT_DIAMETER, VILLAGE_DOT_DIAMETER / 2);
    }
  }

  // No marker draw here anymore -- MapActivity draws its own mode-specific
  // marker (ring + dot/arrow, sized per hike/cycle/ride) straight through
  // GfxRenderer after this call returns, since that needs a white halo fill
  // IMapCanvas cannot express (its fillRoundedRect is hardcoded black -- see
  // GfxRendererCanvas.h). Drawing this generic triangle here too left it
  // peeking out from under the real marker whenever the real one was
  // smaller (hike's dot, cycle's small arrow). Callers that still want the
  // plain triangle (test/map_preview) call drawMarker() directly.
}

void MapRenderer::drawMarker(IMapCanvas& canvas, int16_t cx, int16_t cy, MapHeading heading) {
  const Vec2& dir = kHeadingDir[static_cast<uint8_t>(heading)];
  const Vec2 perp{-dir.dy, dir.dx};  // rotate 90 degrees, same /8 scale

  const int tipX = cx + dir.dx * MARKER_TIP_LEN / 8;
  const int tipY = cy + dir.dy * MARKER_TIP_LEN / 8;
  const int baseCx = cx - dir.dx * (MARKER_TIP_LEN / 2) / 8;
  const int baseCy = cy - dir.dy * (MARKER_TIP_LEN / 2) / 8;
  const int baseLeftX = baseCx + perp.dx * MARKER_BASE_HALF_W / 8;
  const int baseLeftY = baseCy + perp.dy * MARKER_BASE_HALF_W / 8;
  const int baseRightX = baseCx - perp.dx * MARKER_BASE_HALF_W / 8;
  const int baseRightY = baseCy - perp.dy * MARKER_BASE_HALF_W / 8;

  const int xs[3] = {tipX, baseLeftX, baseRightX};
  const int ys[3] = {tipY, baseLeftY, baseRightY};
  canvas.fillPolygon(xs, ys, 3);
}
