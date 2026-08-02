#pragma once

#include <cstdint>

class IMapCanvas;
class MapProjection;

// A tile that is absent, truncated or crc32-mismatched is drawn as hatch,
// never as white -- docs/map-data-spec.md, "Which tiles to load". White means
// empty countryside. Hatch means nobody knows what is there. Same honesty
// rule as the map timestamp.
//
// The hatch is drawn in tile-local space and projected like any other
// geometry, so it rotates with the heading and lands exactly on the tile the
// data is missing from. No new canvas primitive: it is diagonal drawLine
// calls, clipped to the screen by the canvas adapter.
namespace MapHatch {

// Screen-space distance between hatch lines. Wide enough to read as texture
// rather than fill on a 1-bit panel.
inline constexpr int kSpacingPx = 24;

// Bounds the work when a tile is huge against the current m/px. A z11 tile at
// 3 m/px would otherwise ask for thousands of lines it cannot show.
inline constexpr int kMaxLines = 192;

void drawTile(IMapCanvas& canvas, const MapProjection& proj, uint8_t z, uint32_t col, uint32_t row);

}  // namespace MapHatch
