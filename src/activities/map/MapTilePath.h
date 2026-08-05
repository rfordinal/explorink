#pragma once

#include <cstdint>
#include <cstring>

// One tile's coordinates, read back out of a tile file's path.
struct MapTileCoord {
  uint8_t z = 0;
  uint32_t col = 0;
  uint32_t row = 0;
};

// Parses `<anything>base/<z>/<col>/<row>.tib` -- the layout
// MapTileSource::buildPath() writes and the one a BLE sender must use
// (MapTileSource.cpp:28-31, ../../../docs/ble-map-transfer-protocol.md in the
// parent xteink repo). Everything before the `base/` segment is ignored, so
// both the absolute card path and the sender's root-relative path parse the
// same:
//
//   /trailink/base/12/2199/1416.tib
//   base/12/2199/1416.tib
//
// Why this exists: a file that lands over BLE is the answer to a
// MissingTilesStore entry, and the path is the only place the transfer knows
// which entry (MapTransferReceiver never sees a tile coordinate, only bytes
// and a path). Returns false for anything that is not a tile -- a route file
// or a style push is a legitimate transfer that simply clears no entry.
//
// Header-only: one small function with no state, and the native test wants it
// without linking the map activity.
inline bool parseMapTilePath(const char* path, MapTileCoord& out) {
  if (path == nullptr) return false;

  // The `base/` segment, on a segment boundary so `mybase/...` does not match.
  // Only one layer exists today; a second one would look for its own name here.
  const char* p = nullptr;
  for (const char* c = path; *c != '\0'; ++c) {
    if (*c != 'b') continue;
    if (std::strncmp(c, "base/", 5) != 0) continue;
    if (c != path && *(c - 1) != '/') continue;
    p = c + 5;
    break;
  }
  if (p == nullptr) return false;

  // z / col / row, decimal, no sign, no leading '+'. A component that is empty
  // or holds anything else fails the whole parse rather than parsing partly.
  uint32_t parts[3] = {0, 0, 0};
  for (int i = 0; i < 3; ++i) {
    if (*p < '0' || *p > '9') return false;
    uint32_t value = 0;
    while (*p >= '0' && *p <= '9') {
      // Real cols and rows are under 2^15 at z13 (../../../docs/map-data-spec.md).
      // This cap is only here so a junk path cannot wrap the accumulator.
      if (value > 0xFFFFFFu) return false;
      value = value * 10 + static_cast<uint32_t>(*p - '0');
      ++p;
    }
    parts[i] = value;
    if (i < 2) {
      if (*p != '/') return false;
      ++p;
    }
  }

  if (std::strcmp(p, ".tib") != 0) return false;
  if (parts[0] > 255) return false;  // z is a uint8_t in MissingTileHit

  out.z = static_cast<uint8_t>(parts[0]);
  out.col = parts[1];
  out.row = parts[2];
  return true;
}
