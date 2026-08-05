#pragma once

#include <cstdint>

// Which gap the rider most wants filled first. A fetch can be cut short --
// the rider rides off, the battery goes, the phone walks out of range
// mid-transfer -- so the request list goes out in this order and whatever
// finished is already the most useful subset.
//
// Its own header, deliberately: MissingTilesStore.h pulls in ArduinoJson and
// the SD-backed PersistableStore, and the native test wants the policy
// without the storage behind it.

// LOD tier rank, lowest fetched first: regional, then overview, then detail.
//
// Ride mode is the primary use case and opens at zoom step 2, which is the
// regional LOD (`kDefaultZoomStepForMode` in
// src/activities/map/MapRideMode.h:27, `kZoomLadder` step 2 = z12 in
// src/activities/map/MapViewport.h:28-34). So regional is the view the rider
// actually looks at by default, overview is the next one out, and detail is a
// close-up nobody navigates by. A z outside the three LODs
// (../docs/map-data-spec.md, "Levels of detail": z13/z12/z11) sorts last --
// this firmware's ladder cannot produce one, so it is a stale file or a bug,
// and either way not what to spend the first minute of a transfer on.
constexpr uint8_t missingTileTierRank(uint8_t z) {
  switch (z) {
    case 12:
      return 0;  // regional
    case 11:
      return 1;  // overview
    case 13:
      return 2;  // detail
    default:
      return 3;
  }
}

// True when `a` should be fetched before `b`.
//
// Tier first, hit count second: a tile hatched once at regional still beats a
// tile hatched fifty times at detail, because the tier says "the rider's
// normal view is incomplete here" while the count only says "a close-up would
// have been nice". Count breaks ties inside a tier -- a tile hatched twelve
// times mattered to the ride more than one hatched once
// (`MissingTileHit::count`, ../docs/missing-tiles.md).
//
// Tuning knob, not a law: if a detail tile hatched constantly should ever
// outrank a regional tile hatched once, the two keys become one weighted
// score instead of this strict lexicographic pair.
//
// col/row last, so the order is total. Two entries can otherwise share a
// tier and a count, and a non-total order lets the same list page in a
// different sequence each time it is read (std::sort is not stable).
//
// A template rather than eight scalar parameters: the store's
// `MissingTileHit` and the console's `MapMissingTile` are field-for-field
// copies of each other for include-dependency reasons, and both need this.
// Header-only and inlined at both call sites, so the CLAUDE.md warning about
// template bloat does not bite -- there is no out-of-line copy to duplicate.
template <typename Tile>
constexpr bool missingTileFetchBefore(const Tile& a, const Tile& b) {
  const uint8_t rankA = missingTileTierRank(a.z);
  const uint8_t rankB = missingTileTierRank(b.z);
  if (rankA != rankB) return rankA < rankB;
  if (a.count != b.count) return a.count > b.count;
  if (a.col != b.col) return a.col < b.col;
  return a.row < b.row;
}
