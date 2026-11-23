#include <gtest/gtest.h>

#include <set>
#include <string>
#include <vector>

#include "components/lock/PatternLock.h"

using patternlock::Entry;
using patternlock::Step;

namespace {

std::string encoded(const Entry& e) {
  char buf[patternlock::kPathBufBytes] = {0};
  e.encode(buf);
  return {buf};
}

// Walks every legal path of 4 to 8 points from `start` and counts them. The same
// enumeration that produced the numbers in PatternLock.h's header comment, so the
// claim in the docs is a test and not a story.
size_t countPaths(const uint8_t start) {
  size_t total = 0;
  std::vector<Entry> frontier{Entry(start)};
  while (!frontier.empty()) {
    std::vector<Entry> next;
    for (const Entry& e : frontier) {
      for (const Step s : {Step::Left, Step::Right, Step::Up, Step::Down}) {
        Entry child = e;
        if (!child.step(s)) continue;
        if (child.longEnough()) ++total;
        if (!child.full()) next.push_back(child);
      }
    }
    frontier = next;
  }
  return total;
}

}  // namespace

TEST(PatternLock, StartsOnTheConfiguredCellWithOnePoint) {
  const Entry e(patternlock::kDefaultStart);
  EXPECT_EQ(e.cursor(), patternlock::kDefaultStart);
  EXPECT_EQ(e.points(), 1u);
  EXPECT_FALSE(e.longEnough());
  EXPECT_FALSE(e.full());
}

TEST(PatternLock, AnOutOfRangeStartFallsBackToTheDefault) {
  const Entry e(99);
  EXPECT_EQ(e.cursor(), patternlock::kDefaultStart);
}

TEST(PatternLock, StepsMoveOneCellInTheRightDirection) {
  Entry e(0);  // top-left
  ASSERT_TRUE(e.step(Step::Right));
  EXPECT_EQ(e.cursor(), 1);
  ASSERT_TRUE(e.step(Step::Down));
  EXPECT_EQ(e.cursor(), 1 + patternlock::kCols);
  ASSERT_TRUE(e.step(Step::Left));
  EXPECT_EQ(e.cursor(), patternlock::kCols);
  ASSERT_TRUE(e.step(Step::Up));
  EXPECT_EQ(e.cursor(), 0);
  EXPECT_EQ(e.points(), 5u);
}

TEST(PatternLock, AStepIntoAWallChangesNothing) {
  Entry e(0);
  EXPECT_FALSE(e.step(Step::Left));
  EXPECT_FALSE(e.step(Step::Up));
  EXPECT_EQ(e.points(), 1u);
  EXPECT_EQ(e.cursor(), 0);

  Entry corner(patternlock::kCells - 1);
  EXPECT_FALSE(corner.step(Step::Right));
  EXPECT_FALSE(corner.step(Step::Down));
  EXPECT_EQ(corner.points(), 1u);
}

TEST(PatternLock, ACellMayBeSteppedOnTwice) {
  Entry e(0);
  ASSERT_TRUE(e.step(Step::Right));
  ASSERT_TRUE(e.step(Step::Left));
  ASSERT_TRUE(e.step(Step::Right));
  EXPECT_EQ(encoded(e), "0101");
  EXPECT_TRUE(e.longEnough());
}

TEST(PatternLock, TheEighthPointIsTheLast) {
  Entry e(0);
  for (int i = 0; i < 7; ++i) ASSERT_TRUE(e.step(i % 2 == 0 ? Step::Right : Step::Left));
  EXPECT_EQ(e.points(), patternlock::kMaxPoints);
  EXPECT_TRUE(e.full());
  EXPECT_FALSE(e.step(Step::Down));
  EXPECT_EQ(e.points(), patternlock::kMaxPoints);
}

TEST(PatternLock, UndoWalksBackAndStopsAtTheStart) {
  Entry e(0);
  ASSERT_TRUE(e.step(Step::Right));
  ASSERT_TRUE(e.step(Step::Down));
  EXPECT_TRUE(e.undo());
  EXPECT_EQ(e.cursor(), 1);
  EXPECT_TRUE(e.undo());
  EXPECT_EQ(e.cursor(), 0);
  // Only the start left: the screen reads this false as "leave", not as "eat the press".
  EXPECT_FALSE(e.undo());
  EXPECT_EQ(e.points(), 1u);
}

TEST(PatternLock, EncodingIsOneCharPerPointInOrder) {
  Entry e(0);
  ASSERT_TRUE(e.step(Step::Down));   // 4
  ASSERT_TRUE(e.step(Step::Down));   // 8
  ASSERT_TRUE(e.step(Step::Down));   // 12
  ASSERT_TRUE(e.step(Step::Right));  // 13
  ASSERT_TRUE(e.step(Step::Right));  // 14
  ASSERT_TRUE(e.step(Step::Right));  // 15
  EXPECT_EQ(encoded(e), "048cdef");
}

TEST(PatternLock, SecureWipeZeroesTheBuffer) {
  char buf[patternlock::kPathBufBytes] = "5674";
  patternlock::secureWipe(buf, sizeof(buf));
  for (size_t i = 0; i < sizeof(buf); ++i) EXPECT_EQ(buf[i], '\0') << i;
}

TEST(PatternLock, WipeLeavesNothingButTheStart) {
  Entry e(0);
  ASSERT_TRUE(e.step(Step::Right));
  ASSERT_TRUE(e.step(Step::Right));
  e.wipe();
  EXPECT_EQ(e.points(), 1u);
  EXPECT_EQ(encoded(e), "0");
}

TEST(PatternLock, WellFormedAcceptsWhatTheScreenCanProduce) {
  EXPECT_TRUE(patternlock::isWellFormed("0156", 0));
  EXPECT_TRUE(patternlock::isWellFormed("048cdef", 0));
  // Too short, too long, a diagonal, a jump, a bad char, empty, null.
  EXPECT_FALSE(patternlock::isWellFormed("014", 0));
  EXPECT_FALSE(patternlock::isWellFormed("010101010", 0));
  EXPECT_FALSE(patternlock::isWellFormed("0509", 0));
  EXPECT_FALSE(patternlock::isWellFormed("08cd", 0));
  EXPECT_FALSE(patternlock::isWellFormed("01z4", 0));
  EXPECT_FALSE(patternlock::isWellFormed("", 0));
  EXPECT_FALSE(patternlock::isWellFormed(nullptr, 0));
}

// The permanent-lockout case the validator exists to catch: a stored pattern that
// is a legal walk, against a gate whose cursor starts somewhere else. Every path
// the screen can produce begins with the start cell, so nothing can ever match.
TEST(PatternLock, WellFormedRejectsAPatternThatStartsElsewhere) {
  EXPECT_TRUE(patternlock::isWellFormed("0156", 0));
  EXPECT_FALSE(patternlock::isWellFormed("0156", patternlock::kDefaultStart));
  EXPECT_EQ(patternlock::startCellOf("0156"), 0);
  EXPECT_EQ(patternlock::startCellOf("5674"), 5);
  EXPECT_EQ(patternlock::startCellOf(""), patternlock::kCells);
  EXPECT_EQ(patternlock::startCellOf(nullptr), patternlock::kCells);
  // What a caller is meant to do: read the start off the pattern it stored.
  const char* stored = "0156";
  EXPECT_TRUE(patternlock::isWellFormed(stored, patternlock::startCellOf(stored)));
}

TEST(PatternLock, EveryPathTheScreenCanProduceIsWellFormed) {
  // The two halves have to agree, or provisioning refuses a pattern a rider just
  // entered -- a lockout from a mismatch between a producer and its own validator.
  std::vector<Entry> frontier{Entry(patternlock::kDefaultStart)};
  size_t checked = 0;
  while (!frontier.empty()) {
    std::vector<Entry> next;
    for (const Entry& e : frontier) {
      for (const Step s : {Step::Left, Step::Right, Step::Up, Step::Down}) {
        Entry child = e;
        if (!child.step(s)) continue;
        if (child.longEnough()) {
          char buf[patternlock::kPathBufBytes] = {0};
          child.encode(buf);
          EXPECT_TRUE(patternlock::isWellFormed(buf, patternlock::kDefaultStart)) << buf;
          ++checked;
        }
        if (!child.full()) next.push_back(child);
      }
    }
    frontier = next;
  }
  EXPECT_GT(checked, 7000u);
}

// The size of the space, counted rather than argued about. These are the figures
// PatternLock.h quotes and docs/pattern-lock.md repeats; if a grid dimension or a
// length bound moves, this test is what says so.
TEST(PatternLock, TheSpaceIsTwoThousandEightHundredFromACorner) {
  EXPECT_EQ(countPaths(0), 2810u);
}

TEST(PatternLock, TheSpaceIsFourThousandFiveHundredFortyFiveFromAnEdge) {
  // The third figure docs/pattern-lock.md quotes. It had no test behind it until
  // a review pointed out the doc claimed all three were covered.
  EXPECT_EQ(countPaths(1), 4545u);
}

TEST(PatternLock, TheSpaceIsSevenThousandThreeHundredFiftyFromTheMiddle) {
  EXPECT_EQ(countPaths(patternlock::kDefaultStart), 7350u);
  // And that is why the default start is interior: 2.6x a corner for nothing.
  EXPECT_GT(countPaths(patternlock::kDefaultStart), countPaths(0) * 2);
}

TEST(PatternLock, DistinctPathsEncodeDistinctly) {
  // An encoding collision would make two patterns one, which is a silent halving of
  // a space this small.
  std::set<std::string> seen;
  std::vector<Entry> frontier{Entry(patternlock::kDefaultStart)};
  size_t produced = 0;
  while (!frontier.empty()) {
    std::vector<Entry> next;
    for (const Entry& e : frontier) {
      for (const Step s : {Step::Left, Step::Right, Step::Up, Step::Down}) {
        Entry child = e;
        if (!child.step(s)) continue;
        if (child.longEnough()) {
          seen.insert(encoded(child));
          ++produced;
        }
        if (!child.full()) next.push_back(child);
      }
    }
    frontier = next;
  }
  EXPECT_EQ(seen.size(), produced);
}
