#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

#include "MissingTilePriority.h"

namespace {

// Field-for-field what MissingTileHit and MapMissingTile both are. Declared
// here rather than including either: MissingTilesStore.h pulls ArduinoJson and
// the SD-backed PersistableStore, and this suite is about the ordering policy
// alone.
struct Tile {
  uint8_t z = 0;
  uint32_t col = 0;
  uint32_t row = 0;
  uint32_t count = 0;
};

std::vector<Tile> sorted(std::vector<Tile> tiles) {
  std::sort(tiles.begin(), tiles.end(), [](const Tile& a, const Tile& b) { return missingTileFetchBefore(a, b); });
  return tiles;
}

}  // namespace

TEST(MissingTilePriority, TierRankIsRegionalOverviewDetail) {
  EXPECT_EQ(missingTileTierRank(12), 0);  // regional -- ride mode's default view
  EXPECT_EQ(missingTileTierRank(11), 1);  // overview
  EXPECT_EQ(missingTileTierRank(13), 2);  // detail
}

TEST(MissingTilePriority, ZOutsideTheThreeLodsSortsLast) {
  // This firmware's zoom ladder cannot produce one, so it is a stale file or
  // a bug -- either way not the first thing to spend a transfer on.
  EXPECT_EQ(missingTileTierRank(0), 3);
  EXPECT_EQ(missingTileTierRank(10), 3);
  EXPECT_EQ(missingTileTierRank(14), 3);
  EXPECT_EQ(missingTileTierRank(255), 3);
}

TEST(MissingTilePriority, TierBeatsCount) {
  const std::vector<Tile> order = sorted({
      Tile{13, 1, 1, 50},  // detail, hatched constantly
      Tile{12, 2, 2, 1},   // regional, hatched once
  });
  // The whole point of the tier being the primary key.
  EXPECT_EQ(order[0].z, 12);
  EXPECT_EQ(order[1].z, 13);
}

TEST(MissingTilePriority, CountBreaksTiesInsideATier) {
  const std::vector<Tile> order = sorted({
      Tile{12, 1, 1, 2},
      Tile{12, 2, 2, 12},
      Tile{12, 3, 3, 7},
  });
  EXPECT_EQ(order[0].count, 12u);
  EXPECT_EQ(order[1].count, 7u);
  EXPECT_EQ(order[2].count, 2u);
}

TEST(MissingTilePriority, AllThreeTiersInOneList) {
  const std::vector<Tile> order = sorted({
      Tile{11, 1, 1, 3},
      Tile{13, 2, 2, 9},
      Tile{12, 3, 3, 1},
      Tile{11, 4, 4, 8},
  });
  ASSERT_EQ(order.size(), 4u);
  EXPECT_EQ(order[0].z, 12);  // regional first, count irrelevant
  EXPECT_EQ(order[1].z, 11);  // then overview, highest count of the two
  EXPECT_EQ(order[1].count, 8u);
  EXPECT_EQ(order[2].z, 11);
  EXPECT_EQ(order[3].z, 13);  // detail last, even at count 9
}

TEST(MissingTilePriority, OrderIsTotalSoPagingIsRepeatable) {
  // Same tier, same count: col then row decide. Without this the order is not
  // total, std::sort is free to return either arrangement, and two reads of
  // the same list can page in different sequences -- a reader would miss
  // entries and see others twice.
  const Tile a{12, 100, 200, 5};
  const Tile b{12, 100, 201, 5};
  const Tile c{12, 101, 200, 5};

  EXPECT_TRUE(missingTileFetchBefore(a, b));
  EXPECT_FALSE(missingTileFetchBefore(b, a));
  EXPECT_TRUE(missingTileFetchBefore(b, c));
  EXPECT_FALSE(missingTileFetchBefore(c, b));

  // Irreflexive, as a strict weak ordering must be.
  EXPECT_FALSE(missingTileFetchBefore(a, a));
}

// --- the last known position ------------------------------------------------
//
// A fetch is routinely cut short, so the first squares to go out must be the
// ones under the rider. These lock that in.

namespace {

std::vector<Tile> sortedNear(std::vector<Tile> tiles, const MissingTileAnchor& anchor) {
  std::sort(tiles.begin(), tiles.end(),
            [&anchor](const Tile& a, const Tile& b) { return missingTileFetchBefore(a, b, anchor); });
  return tiles;
}

// The rider at z12 2200/1400, with the other two tiers set to the same numbers
// -- the tests below stay inside one tier, so only that tier's pair matters.
MissingTileAnchor anchorAt(uint32_t col, uint32_t row) {
  MissingTileAnchor a;
  a.valid = true;
  for (int i = 0; i < 4; ++i) {
    a.col[i] = col;
    a.row[i] = row;
  }
  return a;
}

}  // namespace

TEST(MissingTilePriority, DistanceIsManhattanInWholeTiles) {
  const MissingTileAnchor anchor = anchorAt(2200, 1400);
  EXPECT_EQ(missingTileDistance(Tile{12, 2200, 1400, 0}, anchor), 0u);
  EXPECT_EQ(missingTileDistance(Tile{12, 2201, 1400, 0}, anchor), 1u);
  EXPECT_EQ(missingTileDistance(Tile{12, 2199, 1399, 0}, anchor), 2u);
  // Absolute, not signed: west and north of the rider count the same as east
  // and south.
  EXPECT_EQ(missingTileDistance(Tile{12, 2190, 1400, 0}, anchor), 10u);
  EXPECT_EQ(missingTileDistance(Tile{12, 2210, 1400, 0}, anchor), 10u);
}

TEST(MissingTilePriority, NearTheRiderBeatsAHigherHitCountFarAway) {
  const MissingTileAnchor anchor = anchorAt(2200, 1400);
  const std::vector<Tile> order = sortedNear(
      {
          Tile{12, 2260, 1400, 40},  // hatched forty times, six hundred km away
          Tile{12, 2201, 1400, 1},   // hatched once, next door
      },
      anchor);
  EXPECT_EQ(order[0].col, 2201u);
  EXPECT_EQ(order[1].col, 2260u);
}

TEST(MissingTilePriority, HitCountStillBreaksTiesAtEqualDistance) {
  const MissingTileAnchor anchor = anchorAt(2200, 1400);
  // Both one tile away, so the older signal decides -- distance is a bucket,
  // not a replacement for the hit count.
  const std::vector<Tile> order = sortedNear(
      {
          Tile{12, 2201, 1400, 2},
          Tile{12, 2199, 1400, 9},
      },
      anchor);
  EXPECT_EQ(order[0].count, 9u);
  EXPECT_EQ(order[1].count, 2u);
}

TEST(MissingTilePriority, TierStillOutranksDistance) {
  const MissingTileAnchor anchor = anchorAt(2200, 1400);
  // A detail close-up right under the rider does not delay the regional view
  // they navigate by, even though it is nearer.
  const std::vector<Tile> order = sortedNear(
      {
          Tile{13, 2200, 1400, 1},
          Tile{12, 2205, 1400, 1},
      },
      anchor);
  EXPECT_EQ(order[0].z, 12);
  EXPECT_EQ(order[1].z, 13);
}

TEST(MissingTilePriority, WithoutAFixTheOldOrderIsUnchanged) {
  // A device that has never had a position must behave exactly as before.
  const MissingTileAnchor none;
  const std::vector<Tile> withAnchorArg = sortedNear(
      {
          Tile{12, 2260, 1400, 40},
          Tile{12, 2201, 1400, 1},
      },
      none);
  EXPECT_EQ(withAnchorArg[0].count, 40u);
  EXPECT_EQ(withAnchorArg[1].count, 1u);
}

TEST(MissingTilePriority, DistanceOrderIsStillTotal) {
  const MissingTileAnchor anchor = anchorAt(2200, 1400);
  // Equidistant, equal count: col then row, as before, or paging repeats and
  // skips entries.
  const Tile a{12, 2201, 1400, 5};
  const Tile b{12, 2199, 1400, 5};
  EXPECT_TRUE(missingTileFetchBefore(b, a, anchor));
  EXPECT_FALSE(missingTileFetchBefore(a, b, anchor));
  EXPECT_FALSE(missingTileFetchBefore(a, a, anchor));
}
