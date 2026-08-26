#pragma once

#include <cstdint>

#include "MapViewport.h"

// One bit per (tile index, layer id) pair inside one viewport reset: which
// layers have already passed their crc32 this frame, and which are known bad.
//
// This was a bare `uint64_t` while `MapViewport::kMaxTiles` was 9 -- 9 tiles x
// 7 layer slots = 63 bits, one to spare, and every call site carried a
// `bit < 64` guard. Rungs 5 and 6 raised the tile cap to 16, which is 112 bits.
//
// The old guards made that *safe* but not correct: past bit 63 the memo simply
// stopped recording, so a layer would be crc-checked again on every one of
// MapRenderer::kRoadPasses passes (the check the memo exists to skip), and a
// layer already known corrupt would be streamed again instead of hatched. Both
// failures land exactly on the tiles a coarse rung adds, and neither is visible
// -- the frame still draws, just slower and re-reading known-bad bytes.
//
// So: an explicit 128-bit set, sized by static_assert against the tile cap, and
// no guard at the call sites beyond what this offers.
struct MapLayerBits {
  // Layer ids run 1..15 (MapTileReader::kMaxLayers), so 16 slots per tile
  // including the unused 0. Same layout crcBitFor() has always produced.
  //
  // 16 tiles x 16 slots is 256, which is kBitCount exactly -- and unlike the
  // 8-slot version this is not a coincidence to be nervous about: the slot count
  // was raised to 15 layers precisely so a future layer id needs no format
  // change, and this table is what pays for that. 32 bytes per instance against
  // 16, which is the whole cost of the guarantee.
  static constexpr uint32_t kSlotsPerTile = 16;
  static constexpr uint32_t kBitCount = 256;

  uint64_t w[4] = {0, 0, 0, 0};

  bool test(uint32_t bit) const {
    if (bit >= kBitCount) return false;
    return ((w[bit >> 6] >> (bit & 63)) & 1ull) != 0;
  }

  void set(uint32_t bit) {
    if (bit >= kBitCount) return;
    w[bit >> 6] |= 1ull << (bit & 63);
  }

  bool any() const { return w[0] != 0 || w[1] != 0 || w[2] != 0 || w[3] != 0; }

  void clear() {
    w[0] = 0;
    w[1] = 0;
    w[2] = 0;
    w[3] = 0;
  }
};

static_assert(MapViewport::kMaxTiles * MapLayerBits::kSlotsPerTile <= MapLayerBits::kBitCount,
              "one bit per (tile, layer) pair must fit MapLayerBits -- raise kBitCount with kMaxTiles");
