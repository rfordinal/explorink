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
