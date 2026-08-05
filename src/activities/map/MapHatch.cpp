#include "MapHatch.h"

#include <cmath>

#include "IMapCanvas.h"
#include "MapProjection.h"
#include "MapTileGrid.h"

namespace MapHatch {

namespace {

// Normalised tile coordinates: u east from the NW corner, v south from it,
// both 0..1. Projected the same way a way's points are, so the hatch turns
// with the viewport instead of staying screen-aligned.
void projectLocal(const MapProjection& proj, int32_t west, int32_t north, int32_t sizeM, double u, double v,
                  int16_t& outX, int16_t& outY) {
  const double mercX = static_cast<double>(west) + u * static_cast<double>(sizeM);
  const double mercY = static_cast<double>(north) - v * static_cast<double>(sizeM);
  proj.projectMerc(mercX, mercY, outX, outY);
}

}  // namespace

void drawTile(IMapCanvas& canvas, const MapProjection& proj, uint8_t z, uint32_t col, uint32_t row) {
  int32_t west = 0, south = 0, east = 0, north = 0;
  MapTileGrid::tileBounds(z, col, row, west, south, east, north);
  const int32_t sizeM = east - west;
  if (sizeM <= 0) return;

  const double tilePx = static_cast<double>(sizeM) / proj.mppMerc();
  if (!(tilePx > 1.0)) return;

  // Lines run at 45 degrees in tile space: constant u+v = s, s in (0,2).
  // Consecutive lines sit kSpacingPx apart on screen, so the step in s is
  // spacing * sqrt(2) / tilePx.
  const double stepS = static_cast<double>(kSpacingPx) * 1.41421356237 / tilePx;
  if (!(stepS > 0.0)) return;

  int drawn = 0;
  for (double s = stepS; s < 2.0 && drawn < kMaxLines; s += stepS, ++drawn) {
    double u0, v0, u1, v1;
    if (s <= 1.0) {
      u0 = s;
      v0 = 0.0;
      u1 = 0.0;
      v1 = s;
    } else {
      u0 = 1.0;
      v0 = s - 1.0;
      u1 = s - 1.0;
      v1 = 1.0;
    }

    int16_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    projectLocal(proj, west, north, sizeM, u0, v0, x0, y0);
    projectLocal(proj, west, north, sizeM, u1, v1, x1, y1);
    canvas.drawLine(x0, y0, x1, y1, 1, MapInk::Black);
  }
}

}  // namespace MapHatch
