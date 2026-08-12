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
  // Layer ids run 1..6 (MapTileReader::Layer), so 7 slots per tile including
  // the unused 0. Same layout crcBitFor() has always produced.
  static constexpr uint32_t kSlotsPerTile = 7;
  static constexpr uint32_t kBitCount = 128;

  uint64_t lo = 0;
  uint64_t hi = 0;

  bool test(uint32_t bit) const {
    if (bit < 64) return ((lo >> bit) & 1ull) != 0;
    if (bit < kBitCount) return ((hi >> (bit - 64)) & 1ull) != 0;
    return false;
  }

  void set(uint32_t bit) {
    if (bit < 64) {
      lo |= 1ull << bit;
    } else if (bit < kBitCount) {
      hi |= 1ull << (bit - 64);
    }
  }

  bool any() const { return lo != 0 || hi != 0; }

  void clear() {
    lo = 0;
    hi = 0;
  }
};

static_assert(MapViewport::kMaxTiles * MapLayerBits::kSlotsPerTile <= MapLayerBits::kBitCount,
              "one bit per (tile, layer) pair must fit MapLayerBits -- raise kBitCount with kMaxTiles");
