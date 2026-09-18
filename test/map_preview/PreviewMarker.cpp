#include "PreviewMarker.h"

#include "MapMarkerShape.generated.h"

namespace PreviewMarker {

namespace {

// Same 16-step direction table as MapRenderer.cpp's kHeadingDir and
// MapActivity.cpp's own copy (dx/dy unit vectors scaled by 8, avoiding any
// per-frame trig). A third copy rather than a shared one for the same reason
// MapActivity.cpp gives for its own: MapHeading's ordering is what pins the
// index, so keep all three in step if it ever changes.
struct HeadingVec {
  int dx, dy;
};
constexpr HeadingVec kMarkerHeadingDir[16] = {
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

// Mirrors MapActivity::drawPositionMarker() (src/activities/map/
// MapActivity.cpp) minus the patch-box bookkeeping a live, moving marker
// needs and this preview does not: no markerBoxDrawn_/markerStyleDrawn_,
// because there is nothing here to erase later.
void draw(IMapCanvas& canvas, int cx, int cy, uint8_t headingStep, MapRideMode mode,
          const MapFixTrust::MarkerStyle& style, const MarkerMetrics& m) {
  const int radius = m.ring / 2;
  // White halo first, same reason as the device: the ring is a thin stroke,
  // and without a white backing the map lines it sits over would show
  // straight through its interior.
  const int haloRadius = radius + m.haloMargin;
  canvas.fillRoundedRect(cx - haloRadius, cy - haloRadius, haloRadius * 2, haloRadius * 2, haloRadius, MapInk::White);
  // The ring as two fills rather than a stroke: IMapCanvas has no stroke
  // primitive (MapRenderer's own puck below draws its ring the same way).
  // A black disc out to `radius`, then a white disc inside it out to
  // `radius - ringWidth`, leaves exactly a ringWidth-thick black band at the
  // outer edge -- the same "border inside the shape" GfxRenderer::
  // drawRoundedRect(..., state) draws, and the interior was already white
  // from the halo fill above either way.
  canvas.fillRoundedRect(cx - radius, cy - radius, m.ring, m.ring, radius, MapInk::Black);
  const int innerRadius = radius - m.ringWidth;
  if (innerRadius > 0) {
    canvas.fillRoundedRect(cx - innerRadius, cy - innerRadius, innerRadius * 2, innerRadius * 2, innerRadius,
                           MapInk::White);
  }

  if (style.ringBroken) {
    // Punch white squares through the ring at every other heading step --
    // eight gaps, no trig, same table the heading glyph indexes.
    const int gap = m.ringGap;
    for (int i = 0; i < 16; i += 16 / kMarkerRingGapCount) {
      const HeadingVec& at = kMarkerHeadingDir[i];
      const int gx = cx + at.dx * radius / 8;
      const int gy = cy + at.dy * radius / 8;
      canvas.fillRoundedRect(gx - gap / 2, gy - gap / 2, gap, gap, 0, MapInk::White);
    }
  }

  if (style.head == MapFixTrust::MarkerStyle::Head::None) {
    if (mode == MapRideMode::Hike) {
      canvas.fillRoundedRect(cx - m.hikeDot / 2, cy - m.hikeDot / 2, m.hikeDot, m.hikeDot, m.hikeDot / 2,
                             MapInk::Black);
    }
    return;
  }

  if (style.head == MapFixTrust::MarkerStyle::Head::Wedge) {
    const int stepIdx = headingStep < 16 ? headingStep : 0;
    const HeadingVec& left = kMarkerHeadingDir[(stepIdx + 15) % 16];
    const HeadingVec& right = kMarkerHeadingDir[(stepIdx + 1) % 16];
    const int reach = m.hikeHandReach;
    const int lx = cx + left.dx * reach / 8;
    const int ly = cy + left.dy * reach / 8;
    const int rx = cx + right.dx * reach / 8;
    const int ry = cy + right.dy * reach / 8;
    canvas.drawLine(cx, cy, lx, ly, 1, MapInk::Black);
    canvas.drawLine(cx, cy, rx, ry, 1, MapInk::Black);
    canvas.drawLine(lx, ly, rx, ry, 1, MapInk::Black);
    if (mode == MapRideMode::Hike) {
      canvas.fillRoundedRect(cx - m.hikeDot / 2, cy - m.hikeDot / 2, m.hikeDot, m.hikeDot, m.hikeDot / 2,
                             MapInk::Black);
    }
    return;
  }

  if (mode == MapRideMode::Hike) {
    const HeadingVec& hand = kMarkerHeadingDir[headingStep < 16 ? headingStep : 0];
    const HeadingVec handPerp{-hand.dy, hand.dx};
    const int tipX = cx + hand.dx * m.hikeHandReach / 8;
    const int tipY = cy + hand.dy * m.hikeHandReach / 8;
    const int hx[4] = {
        cx + handPerp.dx * m.hikeHandHalfW / 8,
        tipX + handPerp.dx * m.hikeHandHalfW / 8,
        tipX - handPerp.dx * m.hikeHandHalfW / 8,
        cx - handPerp.dx * m.hikeHandHalfW / 8,
    };
    const int hy[4] = {
        cy + handPerp.dy * m.hikeHandHalfW / 8,
        tipY + handPerp.dy * m.hikeHandHalfW / 8,
        tipY - handPerp.dy * m.hikeHandHalfW / 8,
        cy - handPerp.dy * m.hikeHandHalfW / 8,
    };
    canvas.fillPolygon(hx, hy, 4, MapInk::Black);
    canvas.fillRoundedRect(cx - m.hikeDot / 2, cy - m.hikeDot / 2, m.hikeDot, m.hikeDot, m.hikeDot / 2, MapInk::Black);
    return;
  }

  // Cycle and ride both point at the real incoming heading.
  const int tipLen = mode == MapRideMode::Ride ? m.rideTipLen : m.cycleTipLen;
  const HeadingVec& dir = kMarkerHeadingDir[headingStep < 16 ? headingStep : 0];
  const HeadingVec perp{-dir.dy, dir.dx};
  int xs[kMarkerArrowVertexCount];
  int ys[kMarkerArrowVertexCount];
  for (int i = 0; i < kMarkerArrowVertexCount; ++i) {
    const int forward = tipLen * kMarkerArrowForwardPermille[i] / 1000;
    const int right = tipLen * kMarkerArrowRightPermille[i] / 1000;
    xs[i] = cx + dir.dx * forward / 8 + perp.dx * right / 8;
    ys[i] = cy + dir.dy * forward / 8 + perp.dy * right / 8;
  }
  canvas.fillPolygon(xs, ys, kMarkerArrowVertexCount, MapInk::Black);
}

}  // namespace PreviewMarker
