#include "MapTileGrid.h"

#include <algorithm>
#include <cmath>

namespace MapTileGrid {

int32_t tileColOriginX(uint8_t z, uint32_t col) {
  const double n = static_cast<double>(1u << z);
  return static_cast<int32_t>(std::lround(col / n * kWorldSizeM - kWorldSizeM / 2.0));
}

int32_t tileRowOriginY(uint8_t z, uint32_t row) {
  const double n = static_cast<double>(1u << z);
  return static_cast<int32_t>(std::lround(kWorldSizeM / 2.0 - row / n * kWorldSizeM));
}

void tileBounds(uint8_t z, uint32_t col, uint32_t row, int32_t& outWest, int32_t& outSouth, int32_t& outEast,
                int32_t& outNorth) {
  outWest = tileColOriginX(z, col);
  outEast = tileColOriginX(z, col + 1);
  outNorth = tileRowOriginY(z, row);
  outSouth = tileRowOriginY(z, row + 1);
}

void mercToTileColRow(double mercX, double mercY, uint8_t z, uint32_t& outCol, uint32_t& outRow) {
  const double n = static_cast<double>(1u << z);
  const int64_t maxIdx = static_cast<int64_t>(n) - 1;
  int64_t col = static_cast<int64_t>((mercX + kWorldSizeM / 2.0) / kWorldSizeM * n);
  int64_t row = static_cast<int64_t>((kWorldSizeM / 2.0 - mercY) / kWorldSizeM * n);
  col = std::clamp<int64_t>(col, 0, maxIdx);
  row = std::clamp<int64_t>(row, 0, maxIdx);
  outCol = static_cast<uint32_t>(col);
  outRow = static_cast<uint32_t>(row);
}

}  // namespace MapTileGrid
