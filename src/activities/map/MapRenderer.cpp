#include "MapRenderer.h"

namespace {

constexpr int ROAD_LINE_WIDTH = 4;
constexpr int VILLAGE_DOT_DIAMETER = 10;
constexpr int MARKER_TIP_LEN = 14;      // center to tip, pixels
constexpr int MARKER_BASE_HALF_W = 7;   // center to each base corner, pixels

// Unit-ish direction vectors for the 8 snapped headings, scaled by 8 (so the
// diagonal 0.7071 factor becomes the integer 6/8) -- avoids any sin/cos call,
// matching the "no per-frame trig" design decision. Index = MapHeading value.
struct Vec2 {
  int dx, dy;
};
constexpr Vec2 kHeadingDir[8] = {
    {0, -8},   // N
    {6, -6},   // NE
    {8, 0},    // E
    {6, 6},    // SE
    {0, 8},    // S
    {-6, 6},   // SW
    {-8, 0},   // W
    {-6, -6},  // NW
};

}  // namespace

void MapRenderer::render(IMapCanvas& canvas, const MapViewState& state) {
  const auto& road = state.roadPolyline;
  for (size_t i = 1; i < road.size(); ++i) {
    canvas.drawLine(road[i - 1].first, road[i - 1].second, road[i].first, road[i].second, ROAD_LINE_WIDTH);
  }

  for (const auto& dot : state.villageDots) {
    canvas.fillRoundedRect(dot.first - VILLAGE_DOT_DIAMETER / 2, dot.second - VILLAGE_DOT_DIAMETER / 2,
                           VILLAGE_DOT_DIAMETER, VILLAGE_DOT_DIAMETER, VILLAGE_DOT_DIAMETER / 2);
  }

  drawMarker(canvas, state.markerX, state.markerY, state.heading);
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
