#include "LoraPeerTable.h"

#include <gtest/gtest.h>

#include <cstring>

// The heard-nodes table, on the host.
//
// Every rule it has is arithmetic over a fixed array, and every one of them is
// a rule somebody will meet on a hill with no laptop: a node that stops sending
// its name, a node that sends an advert with no fix, a seventeenth node, and a
// device that has been awake for 49 days. None of those are debuggable on a
// panel, so they are settled here.

namespace {

// A key that differs from every other by its first bytes, so a failure names
// which node it was.
void keyFor(uint8_t out[32], uint8_t n) {
  memset(out, 0, 32);
  out[0] = n;
  out[31] = static_cast<uint8_t>(0xA0 + n);
}

}  // namespace

TEST(LoraPeerTable, FirstAdvertIsNewAndRepeatIsNot) {
  LoraPeerTable table;
  uint8_t key[32];
  keyFor(key, 1);

  EXPECT_TRUE(table.heard(key, "ALPHA", false, 0, 0, 100, -40, 9, 1000));
  EXPECT_FALSE(table.heard(key, "ALPHA", false, 0, 0, 101, -42, 8, 2000));
  EXPECT_EQ(table.count(), 1u);
  EXPECT_EQ(table.at(0).adverts, 2);
  EXPECT_EQ(table.at(0).rssiDbm, -42);
  EXPECT_EQ(table.at(0).heardUptimeMs, 2000u);
}

TEST(LoraPeerTable, WholeKeyIdentifiesANode) {
  // Two nodes whose keys share a long prefix are two nodes. MeshCore's own path
  // hashes are one byte and collide by design, so a table keyed on a prefix
  // would silently merge two riders into one marker.
  LoraPeerTable table;
  uint8_t a[32], b[32];
  memset(a, 0x5C, 32);
  memcpy(b, a, 32);
  b[31] = 0x5D;

  EXPECT_TRUE(table.heard(a, "A", false, 0, 0, 1, -40, 9, 1000));
  EXPECT_TRUE(table.heard(b, "B", false, 0, 0, 1, -40, 9, 1000));
  EXPECT_EQ(table.count(), 2u);
}

TEST(LoraPeerTable, ANamelessAdvertDoesNotEraseAKnownName) {
  // The name field is optional per packet, not per node. Treating its absence
  // as "they are nameless now" makes a node flicker between its name and its
  // key on every other advert.
  LoraPeerTable table;
  uint8_t key[32];
  keyFor(key, 2);

  table.heard(key, "BRAVO", false, 0, 0, 1, -40, 9, 1000);
  table.heard(key, nullptr, false, 0, 0, 2, -41, 9, 2000);
  EXPECT_STREQ(table.at(0).name, "BRAVO");

  table.heard(key, "", false, 0, 0, 3, -41, 9, 3000);
  EXPECT_STREQ(table.at(0).name, "BRAVO");
}

TEST(LoraPeerTable, AnAdvertWithNoFixDoesNotEraseAKnownPosition) {
  // The difference between "has not told us" and "is at 0,0" is one the whole
  // group layer keeps. A rider who loses their fix under a cliff must not jump
  // into the Atlantic on everybody's panel.
  LoraPeerTable table;
  uint8_t key[32];
  keyFor(key, 3);

  table.heard(key, "CHAR", true, 48208000, 17064000, 1, -40, 9, 1000);
  table.heard(key, "CHAR", false, 0, 0, 2, -40, 9, 2000);

  EXPECT_TRUE(table.at(0).hasLoc);
  EXPECT_EQ(table.at(0).latE6, 48208000);
  EXPECT_EQ(table.at(0).lonE6, 17064000);
}

TEST(LoraPeerTable, NeverHeardAPositionStaysUnset) {
  LoraPeerTable table;
  uint8_t key[32];
  keyFor(key, 4);

  table.heard(key, "DELTA", false, 0, 0, 1, -40, 9, 1000);
  EXPECT_FALSE(table.at(0).hasLoc);
}

TEST(LoraPeerTable, NewestHeardIsListedFirst) {
  LoraPeerTable table;
  uint8_t a[32], b[32], c[32];
  keyFor(a, 10);
  keyFor(b, 11);
  keyFor(c, 12);

  table.heard(a, "A", false, 0, 0, 1, -40, 9, 1000);
  table.heard(b, "B", false, 0, 0, 1, -40, 9, 2000);
  table.heard(c, "C", false, 0, 0, 1, -40, 9, 3000);
  EXPECT_STREQ(table.at(0).name, "C");
  EXPECT_STREQ(table.at(2).name, "A");

  // Hearing A again moves it to the head, and moves nobody else.
  table.heard(a, "A", false, 0, 0, 2, -40, 9, 4000);
  EXPECT_STREQ(table.at(0).name, "A");
  EXPECT_STREQ(table.at(1).name, "C");
  EXPECT_STREQ(table.at(2).name, "B");
}

TEST(LoraPeerTable, AFullTableEvictsTheNodeHeardLongestAgo) {
  LoraPeerTable table;
  uint8_t key[32];
  for (uint8_t i = 0; i < LoraPeerTable::kMaxPeers; ++i) {
    keyFor(key, i);
    table.heard(key, "x", false, 0, 0, 1, -40, 9, 1000u + i);
  }
  EXPECT_EQ(table.count(), LoraPeerTable::kMaxPeers);

  // Node 0 was heard first and never again, so it is the one that goes.
  uint8_t fresh[32];
  keyFor(fresh, 200);
  EXPECT_TRUE(table.heard(fresh, "NEW", false, 0, 0, 1, -40, 9, 9000));
  EXPECT_EQ(table.count(), LoraPeerTable::kMaxPeers);

  keyFor(key, 0);
  EXPECT_EQ(table.find(key), -1);
  keyFor(key, 1);
  EXPECT_NE(table.find(key), -1);
  EXPECT_STREQ(table.at(0).name, "NEW");
}

TEST(LoraPeerTable, AKnownNodeInAFullTableEvictsNobody) {
  LoraPeerTable table;
  uint8_t key[32];
  for (uint8_t i = 0; i < LoraPeerTable::kMaxPeers; ++i) {
    keyFor(key, i);
    table.heard(key, "x", false, 0, 0, 1, -40, 9, 1000u + i);
  }

  keyFor(key, 0);  // the oldest, and the one talking
  EXPECT_FALSE(table.heard(key, "x", false, 0, 0, 2, -40, 9, 9000));
  EXPECT_EQ(table.count(), LoraPeerTable::kMaxPeers);
  for (uint8_t i = 0; i < LoraPeerTable::kMaxPeers; ++i) {
    keyFor(key, i);
    EXPECT_NE(table.find(key), -1) << "node " << static_cast<int>(i) << " was dropped";
  }
}

TEST(LoraPeerTable, EvictionSurvivesTheMillisWrap) {
  // millis() wraps after 49 days. The eviction compares ages as unsigned
  // differences for that reason; an ordinary `<` on the raw stamps would call
  // the node heard one second ago the oldest in the table and drop it.
  LoraPeerTable table;
  uint8_t key[32];
  constexpr uint32_t kJustBeforeWrap = 0xFFFFF000u;

  // Fifteen nodes heard just before the wrap, one heard just after it.
  for (uint8_t i = 0; i < LoraPeerTable::kMaxPeers - 1; ++i) {
    keyFor(key, i);
    table.heard(key, "old", false, 0, 0, 1, -40, 9, kJustBeforeWrap + i);
  }
  uint8_t afterWrap[32];
  keyFor(afterWrap, 100);
  table.heard(afterWrap, "fresh", false, 0, 0, 1, -40, 9, 500);  // wrapped past zero

  uint8_t fresh[32];
  keyFor(fresh, 200);
  table.heard(fresh, "newest", false, 0, 0, 1, -40, 9, 1000);

  // The node from after the wrap is the second-newest and must still be here;
  // the one evicted is the oldest of the pre-wrap batch.
  EXPECT_NE(table.find(afterWrap), -1);
  keyFor(key, 0);
  EXPECT_EQ(table.find(key), -1);
}

TEST(LoraPeerTable, ALongNameIsTruncatedAndStaysTerminated) {
  LoraPeerTable table;
  uint8_t key[32];
  keyFor(key, 5);

  table.heard(key, "0123456789012345678901234567890", false, 0, 0, 1, -40, 9, 1000);
  EXPECT_EQ(strlen(table.at(0).name), sizeof(table.at(0).name) - 1);
  EXPECT_STREQ(table.at(0).name, "01234567890123456789");
}

TEST(LoraPeerTable, ClearEmptiesIt) {
  LoraPeerTable table;
  uint8_t key[32];
  keyFor(key, 6);
  table.heard(key, "E", false, 0, 0, 1, -40, 9, 1000);
  table.clear();
  EXPECT_EQ(table.count(), 0u);
  EXPECT_EQ(table.find(key), -1);
}
