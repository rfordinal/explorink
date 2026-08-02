#include "MapViewport.h"

#include <algorithm>
#include <cmath>

#include "MapTileGrid.h"

namespace MapViewport {

namespace {
constexpr double kDegToRad = 3.14159265358979323846 / 180.0;
}  // namespace

double mppMercFor(int zoomStep, double anchorLat) {
  const int step = std::clamp(zoomStep, 0, kZoomStepCount - 1);
  return kZoomLadder[step].mpp / std::cos(anchorLat * kDegToRad);
}

TileRange tileRangeFor(const MapProjection& proj, uint8_t z, int screenWidth, int screenHeight) {
  const int corners[4][2] = {{0, 0}, {screenWidth, 0}, {0, screenHeight}, {screenWidth, screenHeight}};

  double minX = 0, minY = 0, maxX = 0, maxY = 0;
  for (int i = 0; i < 4; ++i) {
    double mx = 0, my = 0;
    proj.screenToMerc(static_cast<int16_t>(corners[i][0]), static_cast<int16_t>(corners[i][1]), mx, my);
    if (i == 0) {
      minX = maxX = mx;
      minY = maxY = my;
    } else {
      minX = std::min(minX, mx);
      maxX = std::max(maxX, mx);
      minY = std::min(minY, my);
      maxY = std::max(maxY, my);
    }
  }

  const double marginMerc = kMarginPx * proj.mppMerc();
  minX -= marginMerc;
  maxX += marginMerc;
  minY -= marginMerc;
  maxY += marginMerc;

  uint32_t cA = 0, rA = 0, cB = 0, rB = 0;
  MapTileGrid::mercToTileColRow(minX, maxY, z, cA, rA);  // NW corner
  MapTileGrid::mercToTileColRow(maxX, minY, z, cB, rB);  // SE corner

  TileRange range;
  range.z = z;
  range.col0 = std::min(cA, cB);
  range.col1 = std::max(cA, cB);
  range.row0 = std::min(rA, rB);
  range.row1 = std::max(rA, rB);
  return range;
}

}  // namespace MapViewport
