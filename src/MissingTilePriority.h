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

// Where the rider was last seen, in tile coordinates, one pair per LOD tier
// (indexed by missingTileTierRank, so [0] is z12, [1] is z11, [2] is z13).
//
// The point of it: a gap 200 km behind the rider and a gap under their wheels
// are worth very different amounts, and until now the list could not tell them
// apart -- hit count ranked a tile hatched twelve times last week above the one
// the rider is riding into. On a link that moves ~7 kB/s and a fetch that is
// routinely cut short, the first minute has to spend itself near the rider.
//
// `valid` false means there is no last fix (a device that has never had one),
// and the order falls back to what it was before.
struct MissingTileAnchor {
  bool valid = false;
  uint32_t col[4] = {0, 0, 0, 0};
  uint32_t row[4] = {0, 0, 0, 0};
};

// Whole tiles away from the anchor, Manhattan.
//
// Manhattan and not Euclidean: this is a coarse bucket, not a measurement --
// one z12 tile is about 9.8 km wide, so a tile either is the rider's own square,
// or a neighbour, or somewhere else entirely. Integer arithmetic on purpose;
// this runs inside a sort comparator on a core with no hardware floating point.
template <typename Tile>
constexpr uint32_t missingTileDistance(const Tile& t, const MissingTileAnchor& anchor) {
  const uint8_t rank = missingTileTierRank(t.z);
  const uint32_t ac = anchor.col[rank];
  const uint32_t ar = anchor.row[rank];
  const uint32_t dc = t.col > ac ? t.col - ac : ac - t.col;
  const uint32_t dr = t.row > ar ? t.row - ar : ar - t.row;
  return dc + dr;
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
constexpr bool missingTileFetchBefore(const Tile& a, const Tile& b, const MissingTileAnchor& anchor) {
  const uint8_t rankA = missingTileTierRank(a.z);
  const uint8_t rankB = missingTileTierRank(b.z);
  if (rankA != rankB) return rankA < rankB;
  // Distance beats hit count, and only inside a tier. "Near the rider" is a
  // statement about now; a hit count is a statement about the past, and the
  // rider cannot ride into last week. Tier still comes first -- a nearby detail
  // close-up is not worth delaying the regional view the rider navigates by.
  if (anchor.valid) {
    const uint32_t distA = missingTileDistance(a, anchor);
    const uint32_t distB = missingTileDistance(b, anchor);
    if (distA != distB) return distA < distB;
  }
  if (a.count != b.count) return a.count > b.count;
  if (a.col != b.col) return a.col < b.col;
  return a.row < b.row;
}

// Without an anchor: the order this had before a last fix was taken into
// account. Kept so a caller with no position (and the native test) can still
// ask for a total order.
template <typename Tile>
constexpr bool missingTileFetchBefore(const Tile& a, const Tile& b) {
  return missingTileFetchBefore(a, b, MissingTileAnchor{});
}
