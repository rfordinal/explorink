#pragma once

#include <cstddef>
#include <cstdint>

// Standard IEEE CRC32 (poly 0xEDB88320, reflected) -- the same algorithm
// zlib.crc32 uses. Every map data format on the card is checksummed with it,
// and the value must match mapbuilder's zlib.crc32 bit-for-bit or a round-trip
// check means nothing (mapbuilder/tiles.py, mapbuilder/route_file.py).
//
// Self-contained on purpose: no zlib, no miniz. Two readers need it -- .tib
// tiles (MapTileReader) and .tir routes (MapRouteReader) -- and a second copy
// of a checksum implementation is exactly the kind of thing that agrees on the
// test fixture and disagrees on real data.
//
// Table-free: 8 shifts a byte against a 1 KB flash table on a device at 87.6 %
// flash. The tile path checksums at most a layer at a time, off an SD card that
// is far slower than this loop.
namespace MapCrc32 {

inline constexpr uint32_t kInit = 0xFFFFFFFFu;

// Folds `len` bytes into a running crc. Seed with kInit, finish with final().
inline uint32_t update(uint32_t crc, const uint8_t* data, size_t len) {
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      const uint32_t mask = -(crc & 1u);
      crc = (crc >> 1) ^ (0xEDB88320u & mask);
    }
  }
  return crc;
}

inline uint32_t final(uint32_t crc) { return crc ^ 0xFFFFFFFFu; }

// One-shot, for a buffer already in RAM.
inline uint32_t of(const uint8_t* data, size_t len) { return final(update(kInit, data, len)); }

}  // namespace MapCrc32
