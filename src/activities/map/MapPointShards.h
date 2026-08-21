#pragma once

#include <cstdint>
#include <cstdio>

#include "MapTileGrid.h"

// The z10 shard grid the point layer is stored on, and the paths it lives at.
//
// Header-only: this is arithmetic and one snprintf, wanted by the render
// source, by the Nearby query and by the native tests without linking either.
//
// **The grid is sharding, not a coordinate.** A record carries absolute
// Mercator metres, so which file it sits in never enters its position
// (../../../docs/point-file-spec.md, "Why z10, and why the grid is not a
// coordinate"). Nothing here may be used to reconstruct a point's location.
namespace MapPointShards {

// z10: 39.1 km of Mercator, 26.2 km on the ground at 48 degrees north. Chosen
// so the Nearby menu's 25 km radius search opens 3x3 files worst case instead
// of z11's 5x5 -- an SD read storm for one button press.
constexpr uint8_t kShardZoom = 10;

// The radius the Nearby menu searches, and the number it prints ("None within
// 25 km"). It lives next to kShardZoom because it is what chose it: change one
// without the other and the 3x3 guarantee quietly stops holding.
constexpr double kSearchRadiusM = 25000.0;

// The inclusive shard range covering a Mercator bbox. West/east and south/north
// in metres, y growing north.
struct Range {
  uint32_t col0 = 0;
  uint32_t row0 = 0;
  uint32_t col1 = 0;
  uint32_t row1 = 0;

  uint32_t count() const { return (col1 - col0 + 1) * (row1 - row0 + 1); }
};

inline Range rangeForMercBbox(double west, double south, double east, double north) {
  Range range;
  // North-west corner gives the low col and the low row, because rows count
  // from the north (standard slippy, not TMS).
  MapTileGrid::mercToTileColRow(west, north, kShardZoom, range.col0, range.row0);
  MapTileGrid::mercToTileColRow(east, south, kShardZoom, range.col1, range.row1);
  return range;
}

// The shards a radius search from one point can touch. The bbox of a circle,
// not the circle: a shard whose corner is inside the box but whose nearest
// point is outside the radius is still opened, which costs one read and keeps
// the arithmetic honest -- the per-point distance test rejects its contents.
inline Range rangeForRadius(double mercX, double mercY, double radiusM) {
  return rangeForMercBbox(mercX - radiusM, mercY - radiusM, mercX + radiusM, mercY + radiusM);
}

// `<root>/points/10/<col>/<row>.tip`. Same shape as the tile path
// (MapTileSource::buildPath), with `points/` in place of `base/` -- the
// directory is `points/`, not `safety/`, because one file carries both kinds
// (../../../docs/point-file-spec.md, "Where it lives").
inline bool buildPath(char* out, size_t outLen, const char* rootDir, uint32_t col, uint32_t row) {
  if (out == nullptr || outLen == 0 || rootDir == nullptr) return false;
  const int written = snprintf(out, outLen, "%s/points/%u/%u/%u.tip", rootDir, static_cast<unsigned>(kShardZoom),
                               static_cast<unsigned>(col), static_cast<unsigned>(row));
  return written > 0 && static_cast<size_t>(written) < outLen;
}

}  // namespace MapPointShards
