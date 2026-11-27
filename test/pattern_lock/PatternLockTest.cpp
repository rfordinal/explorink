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

// Walks every legal path of `minPoints` to `maxPoints` points from `start` and
// counts them. The same enumeration that produced the numbers in PatternLock.h's
// header comment and in scripts/pattern_space.py, so the claim in the docs is a
// test and not a story -- and it is parameterised because the two callers of this
// screen ask for different ranges and both tables are quoted.
size_t countPaths(const uint8_t start, const size_t minPoints = patternlock::kMinPoints,
                  const size_t maxPoints = patternlock::kMaxPoints) {
  size_t total = 0;
  std::vector<Entry> frontier{Entry(start, minPoints, maxPoints)};
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

TEST(PatternLock, TheLastPointIsTheCallersMaximum) {
  Entry e(0);
  for (size_t i = 0; i + 1 < patternlock::kMaxPoints; ++i) ASSERT_TRUE(e.step(i % 2 == 0 ? Step::Right : Step::Left));
  EXPECT_EQ(e.points(), patternlock::kMaxPoints);
  EXPECT_TRUE(e.full());
  EXPECT_FALSE(e.step(Step::Down));
  EXPECT_EQ(e.points(), patternlock::kMaxPoints);
}

TEST(PatternLock, ACallersOwnRangeIsWhatFullAndLongEnoughMean) {
  // The wallet asks for 8 to 12 because its floor is a key's floor; a screen lock
  // would ask for less. Both get the same class, so the range has to be the
  // instance's and not the file's (WalletCrypto.h, "Why 8 to 12").
  Entry e(0, 8, 10);
  EXPECT_EQ(e.minPoints(), 8u);
  EXPECT_EQ(e.maxPoints(), 10u);
  for (int i = 0; i < 6; ++i) ASSERT_TRUE(e.step(i % 2 == 0 ? Step::Right : Step::Left));
  EXPECT_EQ(e.points(), 7u);
  EXPECT_FALSE(e.longEnough()) << "seven points is below this caller's floor of eight";
  ASSERT_TRUE(e.step(Step::Right));
  EXPECT_TRUE(e.longEnough());
  ASSERT_TRUE(e.step(Step::Left));
  ASSERT_TRUE(e.step(Step::Right));
  EXPECT_TRUE(e.full()) << "ten is this caller's ceiling even though the file allows twelve";
  EXPECT_FALSE(e.step(Step::Left));
}

TEST(PatternLock, ARangeOutsideTheFileIsClampedRatherThanObeyed) {
  // A caller that asks for more than the path buffer holds would otherwise walk
  // off the end of it, and one that asks for less than four would arm CONFIRM on
  // a two-point pattern. Both are clamped, not trusted.
  const Entry tooBig(0, 2, 99);
  EXPECT_EQ(tooBig.minPoints(), patternlock::kMinPoints);
  EXPECT_EQ(tooBig.maxPoints(), patternlock::kMaxPoints);
  // Swapped bounds: ordered rather than left as a gate whose CONFIRM never arms.
  const Entry swapped(0, 10, 6);
  EXPECT_LE(swapped.minPoints(), swapped.maxPoints());
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
  EXPECT_FALSE(patternlock::isWellFormed("0101010101010", 0));
  EXPECT_FALSE(patternlock::isWellFormed("0509", 0));
  EXPECT_FALSE(patternlock::isWellFormed("08cd", 0));
  EXPECT_FALSE(patternlock::isWellFormed("01z4", 0));
  EXPECT_FALSE(patternlock::isWellFormed("", 0));
  EXPECT_FALSE(patternlock::isWellFormed(nullptr, 0));
}

TEST(PatternLock, WellFormedHonoursTheCallersRangeNotJustTheFiles) {
  // The second half of the lockout guard. A caller whose gate demands eight points
  // must validate against eight, or it stores a five-point pattern the gate will
  // never let a rider finish -- the same shape of failure as a wrong start cell,
  // one range later.
  EXPECT_TRUE(patternlock::isWellFormed("0156", 0));
  EXPECT_FALSE(patternlock::isWellFormed("0156", 0, 8, 12));
  EXPECT_TRUE(patternlock::isWellFormed("01237654", 0, 8, 12));
  // A range the file cannot honour is clamped the same way Entry clamps it, so a
  // validator and a producer configured alike agree.
  EXPECT_TRUE(patternlock::isWellFormed("0156", 0, 2, 99));
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
  // Exact, not a floor. The whole point of walking the producer against its own
  // validator is that a disagreement is a lockout, so "enough of them agreed" is
  // not the assertion that catches one -- the count is the space itself
  // (TheWalletsRangeIsEightHundredThousandFromTheMiddle).
  EXPECT_EQ(checked, 808526u);
}

// The size of the space, counted rather than argued about. These are the figures
// PatternLock.h quotes, scripts/pattern_space.py prints and docs/pattern-lock.md
// repeats; if a grid dimension or a length bound moves, this test is what says so.
//
// Two ranges, because two callers quote two tables: 4 to 8 is the screen lock's
// and the gate's own default, 8 to 12 is the wallet's.
TEST(PatternLock, TheSpaceIsTwoThousandEightHundredFromACorner) { EXPECT_EQ(countPaths(0, 4, 8), 2810u); }

TEST(PatternLock, TheSpaceIsFourThousandFiveHundredFortyFiveFromAnEdge) {
  // The third figure docs/pattern-lock.md quotes. It had no test behind it until
  // a review pointed out the doc claimed all three were covered.
  EXPECT_EQ(countPaths(1, 4, 8), 4545u);
}

TEST(PatternLock, TheSpaceIsSevenThousandThreeHundredFiftyFromTheMiddle) {
  EXPECT_EQ(countPaths(patternlock::kDefaultStart, 4, 8), 7350u);
  // And that is why the default start is interior: 2.6x a corner for nothing.
  EXPECT_GT(countPaths(patternlock::kDefaultStart, 4, 8), countPaths(0, 4, 8) * 2);
}

TEST(PatternLock, TheWalletsRangeIsEightHundredThousandFromTheMiddle) {
  // What 8 to 12 points buys, and the reason the wallet asks for it: 19.6 bits
  // against the 12.8 of the gate's own default, which is what the dot PIN it
  // replaced was worth (WalletCrypto.h, "Why 8 to 12").
  EXPECT_EQ(countPaths(patternlock::kDefaultStart, 8, 12), 806270u);
  EXPECT_EQ(countPaths(0, 8, 12), 307970u);
  EXPECT_EQ(countPaths(1, 8, 12), 498305u);
  // Every point is worth about 3.24x -- four directions less the walls -- so the
  // whole 4-to-8 space is a rounding error inside the 4-to-12 one.
  EXPECT_EQ(countPaths(patternlock::kDefaultStart, 4, 12), 808526u);
  EXPECT_EQ(countPaths(patternlock::kDefaultStart, 4, 12) - countPaths(patternlock::kDefaultStart, 8, 12), 2256u);
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

// ---------------------------------------------------------------------------
// Enrollment: the comparison, and the floor the two walks have to clear
// ---------------------------------------------------------------------------

// The buffers both sides of an enrollment compare are the ones the screen
// declares: fixed size, zero-initialised, written by encode().
TEST(PatternLock, TwoIdenticalWalksCompareEqual) {
  Entry a(5, 8, 12);
  Entry b(5, 8, 12);
  for (const Step s : {Step::Right, Step::Down, Step::Left, Step::Left, Step::Up, Step::Up, Step::Right}) {
    ASSERT_TRUE(a.step(s));
    ASSERT_TRUE(b.step(s));
  }
  char first[patternlock::kPathBufBytes] = {0};
  char second[patternlock::kPathBufBytes] = {0};
  a.encode(first);
  b.encode(second);
  EXPECT_EQ(a.points(), 8u);
  EXPECT_TRUE(patternlock::samePattern(first, second));
}

TEST(PatternLock, AWalkThatDiffersByOneStepIsNotTheSame) {
  Entry a(5, 8, 12);
  Entry b(5, 8, 12);
  for (const Step s : {Step::Right, Step::Down, Step::Left, Step::Left, Step::Up, Step::Up}) {
    ASSERT_TRUE(a.step(s));
    ASSERT_TRUE(b.step(s));
  }
  ASSERT_TRUE(a.step(Step::Right));
  ASSERT_TRUE(b.step(Step::Down));
  char first[patternlock::kPathBufBytes] = {0};
  char second[patternlock::kPathBufBytes] = {0};
  a.encode(first);
  b.encode(second);
  EXPECT_FALSE(patternlock::samePattern(first, second));
}

// A prefix is the mistake a rider actually makes: the second walk is the first
// one with the last point missing. Without the tail of the buffer in the
// comparison this is where an early-exit strcmp would still say "equal" if the
// terminator had been forgotten.
TEST(PatternLock, APrefixOfTheFirstWalkIsNotTheSame) {
  Entry a(5, 8, 12);
  for (const Step s : {Step::Right, Step::Down, Step::Left, Step::Left, Step::Up, Step::Up, Step::Right, Step::Down}) {
    ASSERT_TRUE(a.step(s));
  }
  Entry b = a;
  ASSERT_TRUE(b.undo());
  char first[patternlock::kPathBufBytes] = {0};
  char second[patternlock::kPathBufBytes] = {0};
  a.encode(first);
  b.encode(second);
  EXPECT_EQ(a.points(), 9u);
  EXPECT_EQ(b.points(), 8u);
  EXPECT_FALSE(patternlock::samePattern(first, second));
}

TEST(PatternLock, SamePatternRefusesANullBuffer) {
  char path[patternlock::kPathBufBytes] = {0};
  EXPECT_FALSE(patternlock::samePattern(path, nullptr));
  EXPECT_FALSE(patternlock::samePattern(nullptr, path));
}

// The floor an enrollment has to clear before it may store anything. It is the
// wallet's 8, not this file's 4, and it is the same number the gate will later
// demand -- a pattern stored below it is one the gate would never let a rider
// finish (isWellFormed, and the Config::minPoints comment).
TEST(PatternLock, AnEnrollmentWalkIsNotStorableBelowTheWalletsFloor) {
  Entry e(5, 8, 12);
  for (size_t i = 1; i < 8; ++i) {
    EXPECT_FALSE(e.longEnough()) << "stored at " << e.points() << " points";
    ASSERT_TRUE(e.step(i % 2 == 0 ? Step::Right : Step::Left));
  }
  EXPECT_EQ(e.points(), 8u);
  EXPECT_TRUE(e.longEnough());

  // And what it stores at the floor is what the device will later accept.
  char path[patternlock::kPathBufBytes] = {0};
  e.encode(path);
  EXPECT_TRUE(patternlock::isWellFormed(path, patternlock::startCellOf(path), 8, 12));
}

// Any start cell a rider can pick in enrollment produces patterns the gate can
// walk afterwards -- the picker's whole output range, not just the default.
TEST(PatternLock, EveryStartCellAnEnrollmentCanPickIsUsable) {
  for (uint8_t start = 0; start < patternlock::kCells; ++start) {
    Entry e(start, 8, 12);
    while (!e.longEnough()) {
      // Always a legal step from anywhere: right if there is room, else left.
      if (!e.step(Step::Right)) ASSERT_TRUE(e.step(Step::Left));
    }
    char path[patternlock::kPathBufBytes] = {0};
    e.encode(path);
    EXPECT_EQ(patternlock::startCellOf(path), start);
    EXPECT_TRUE(patternlock::isWellFormed(path, start, 8, 12)) << "start cell " << static_cast<int>(start);
  }
}

// The one input shape that separates samePattern() from a strcmp: two buffers
// holding the same walk with different garbage past the terminator. Every other
// comparison test in this file passes against `strcmp(a, b) == 0`, so without
// this one the fixed-length, whole-buffer property the header spends thirteen
// lines justifying has no coverage at all (review, 2026-11-26).
//
// It also pins the header's own warning: the enrollment's buffers must be
// zero-initialised, and a caller that forgets gets an unequal compare rather
// than a silent pass.
TEST(PatternLock, SamePatternReadsPastTheTerminatorWhereStrcmpStops) {
  Entry e(5, 8, 12);
  for (const Step s : {Step::Right, Step::Down, Step::Left, Step::Left, Step::Up, Step::Up, Step::Right}) {
    ASSERT_TRUE(e.step(s));
  }

  char clean[patternlock::kPathBufBytes] = {0};
  char dirty[patternlock::kPathBufBytes];
  for (char& c : dirty) c = 'X';
  e.encode(clean);
  e.encode(dirty);

  // The same walk, the same characters up to and including the terminator.
  EXPECT_STREQ(clean, dirty);
  // And not the same buffer, which is what this comparison answers.
  EXPECT_FALSE(patternlock::samePattern(clean, dirty));
  EXPECT_TRUE(patternlock::samePattern(clean, clean));
}
