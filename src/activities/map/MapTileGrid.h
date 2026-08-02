#pragma once

#include <cstdint>

// Slippy z/x/y tile grid in Web Mercator metres (EPSG:3857) -- the same
// arithmetic as mapbuilder/tiles.py's tile_col_origin_x / tile_row_origin_y /
// latlon_to_tile_colrow, kept bit-for-bit equivalent so a tile's origin_x/
// origin_y (read from its header) always equals what this code computes for
// that tile's own z/x/y. y counts from the north (standard slippy tiles),
// NOT TMS. docs/map-data-spec.md, "Coordinate frame".
namespace MapTileGrid {

constexpr double kMercRadius = 6378137.0;
constexpr double kWorldSizeM = 2.0 * 3.14159265358979323846 * kMercRadius;  // ~40075016.686

// West edge of tile column `col` at zoom z, in Mercator metres, rounded to
// the integer grid -- a pure function of (z, col), so neighbouring tiles
// agree on their shared boundary bit-for-bit.
int32_t tileColOriginX(uint8_t z, uint32_t col);

// North edge of tile row `row` at zoom z (y counts from the north).
int32_t tileRowOriginY(uint8_t z, uint32_t row);

void tileBounds(uint8_t z, uint32_t col, uint32_t row, int32_t& outWest, int32_t& outSouth, int32_t& outEast,
                int32_t& outNorth);

// Which tile column/row a Mercator point falls in at zoom z. Clamped to the
// valid [0, 2^z - 1] range.
void mercToTileColRow(double mercX, double mercY, uint8_t z, uint32_t& outCol, uint32_t& outRow);

}  // namespace MapTileGrid
