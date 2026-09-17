#include <gtest/gtest.h>

#include "MapGnssBars.h"

// The GNSS block in the map header. Every reading here is a real one off this
// L76K -- the table in MapGnssBars.h says where each came from -- plus the
// thresholds' own edges.

namespace {

using MapGnssBars::barHeightPx;
using MapGnssBars::Block;
using MapGnssBars::resolve;
using MapGnssBars::State;

constexpr int kIconHeight = 14;  // kHeaderIconHeight, the header's icon row

// The panel showing exactly what this reading resolves to, so a following
// resolve() measures its hysteresis against a real previous frame.
State drawn(uint8_t tracked, uint8_t bestSnr) {
  State state;
  const Block block = resolve(tracked, bestSnr, state);
  return State{block.bars, block.heightStep};
}

}  // namespace

// ## The readings that produced this calibration

TEST(GnssBars, HearsNothingDrawsNothing) {
  State state;
  const Block block = resolve(0, 0, state);
  EXPECT_EQ(block.bars, 0);
  EXPECT_EQ(block.heightStep, 0);
  EXPECT_EQ(barHeightPx(block.heightStep, kIconHeight), 0);
}

TEST(GnssBars, IndoorBenchIsOneBarShortOfAnything) {
  // docs/gnss.md: tracked=1 bestsnr=29, no fix. One satellite is not a bar.
  State state;
  const Block block = resolve(1, 29, state);
  EXPECT_EQ(block.bars, 0);
  EXPECT_EQ(block.heightStep, 1);  // it hears something, and only just
}

TEST(GnssBars, NoSkySampleStaysAtTheBottom) {
  // docs/gnss.md GNSS_NOFIX: tracked=4 bestsnr=19. The old calibration filled
  // the whole block on this reading; 19 dB-Hz is below the floor, so nothing is
  // tall enough to draw.
  State state;
  const Block block = resolve(4, 19, state);
  EXPECT_EQ(block.bars, 1);
  EXPECT_EQ(block.heightStep, 0);
  EXPECT_EQ(barHeightPx(block.heightStep, kIconHeight), 0);
}

TEST(GnssBars, IndoorFixIsTwoBarsAtHalfHeight) {
  // docs/gnss.md: tracked=9 bestsnr=32, a real fix through a ceiling. Two bars
  // is the maintainer's "it should normally have a position by now".
  State state;
  const Block block = resolve(9, 32, state);
  EXPECT_EQ(block.bars, 2);
  EXPECT_EQ(block.heightStep, 2);
  EXPECT_EQ(barHeightPx(block.heightStep, kIconHeight), 7);
}

TEST(GnssBars, RideFixIsTwoBarsAtThreeQuarters) {
  // docs/gnss.md GNSS_FIX: tracked=11 bestsnr=38.
  State state;
  const Block block = resolve(11, 38, state);
  EXPECT_EQ(block.bars, 2);
  EXPECT_EQ(block.heightStep, 3);
  EXPECT_EQ(barHeightPx(block.heightStep, kIconHeight), 11);
}

TEST(GnssBars, OpenSkyFillsTheBlock) {
  // 16 satellites was reached under open sky on 2026-09-09, which is what put
  // the top rung there. 45 dB-Hz is an ordinary open-sky best.
  State state;
  const Block block = resolve(16, 45, state);
  EXPECT_EQ(block.bars, 4);
  EXPECT_EQ(block.heightStep, 4);
  EXPECT_EQ(barHeightPx(block.heightStep, kIconHeight), kIconHeight);
}

TEST(GnssBars, MoreThanTheTopRungStillFills) {
  State state;
  EXPECT_EQ(resolve(30, 55, state).bars, 4);
  EXPECT_EQ(resolve(30, 55, state).heightStep, 4);
}

// ## The rungs themselves

TEST(GnssBars, EveryBarRungLightsAtItsThreshold) {
  State state;  // nothing drawn, so no hysteresis credit anywhere
  EXPECT_EQ(resolve(3, 45, state).bars, 0);
  EXPECT_EQ(resolve(4, 45, state).bars, 1);
  EXPECT_EQ(resolve(7, 45, state).bars, 1);
  EXPECT_EQ(resolve(8, 45, state).bars, 2);
  EXPECT_EQ(resolve(11, 45, state).bars, 2);
  EXPECT_EQ(resolve(12, 45, state).bars, 3);
  EXPECT_EQ(resolve(15, 45, state).bars, 3);
  EXPECT_EQ(resolve(16, 45, state).bars, 4);
}

TEST(GnssBars, EveryHeightRungLightsAtItsThreshold) {
  State state;
  EXPECT_EQ(resolve(16, 25, state).heightStep, 0);
  EXPECT_EQ(resolve(16, 26, state).heightStep, 1);
  EXPECT_EQ(resolve(16, 30, state).heightStep, 1);
  EXPECT_EQ(resolve(16, 31, state).heightStep, 2);
  EXPECT_EQ(resolve(16, 35, state).heightStep, 2);
  EXPECT_EQ(resolve(16, 36, state).heightStep, 3);
  EXPECT_EQ(resolve(16, 39, state).heightStep, 3);
  EXPECT_EQ(resolve(16, 40, state).heightStep, 4);
}

TEST(GnssBars, EveryStepIsDistinguishableOnThePanel) {
  // Four steps on a 14 px row. Rounding down would put the first two at 3 and
  // 7 px, and a 3 px bar is a thick baseline tick, not a bar.
  EXPECT_EQ(barHeightPx(1, kIconHeight), 4);
  EXPECT_EQ(barHeightPx(2, kIconHeight), 7);
  EXPECT_EQ(barHeightPx(3, kIconHeight), 11);
  EXPECT_EQ(barHeightPx(4, kIconHeight), 14);
}

// ## The hysteresis, which exists to keep the panel still

TEST(GnssBars, ALitBarSurvivesOneSatelliteDropping) {
  // Drawn at 8 tracked (two bars). 7 must not drop the second bar: a satellite
  // going in and out every few seconds would cost a windowed refresh each time.
  State state = drawn(8, 45);
  ASSERT_EQ(state.bars, 2);
  EXPECT_EQ(resolve(7, 45, state).bars, 2);
  EXPECT_EQ(resolve(6, 45, state).bars, 2);  // still only two below the rung
  EXPECT_EQ(resolve(5, 45, state).bars, 1);  // past the slack, it goes
}

TEST(GnssBars, TheHysteresisNeverLetsARungOverlapTheOneBelow) {
  // Rungs are four satellites apart and the slack is two, so a bar can never
  // hold on into the territory of the bar below it.
  State state = drawn(12, 45);
  ASSERT_EQ(state.bars, 3);
  EXPECT_EQ(resolve(10, 45, state).bars, 3);  // the slack, and no further
  EXPECT_EQ(resolve(9, 45, state).bars, 2);   // never below the second rung's own 8
}

TEST(GnssBars, ALitHeightStepSurvivesADecibelOfWander) {
  State state = drawn(16, 36);
  ASSERT_EQ(state.heightStep, 3);
  EXPECT_EQ(resolve(16, 35, state).heightStep, 3);
  EXPECT_EQ(resolve(16, 34, state).heightStep, 3);
  EXPECT_EQ(resolve(16, 33, state).heightStep, 2);
}

TEST(GnssBars, TheHysteresisCannotResurrectAReceiverThatWentDeaf) {
  // The one case the slack must not cover: zero means the antenna hears
  // nothing, and an empty block is the whole point of the feature.
  State state = drawn(16, 45);
  const Block block = resolve(0, 0, state);
  EXPECT_EQ(block.bars, 0);
  EXPECT_EQ(block.heightStep, 0);
}

TEST(GnssBars, NothingDrawnYetIsNotAnEmptyBlock) {
  // The header's repaint decision compares a fresh resolve() against this. If
  // "never drawn" equalled "empty", the first pass on a deaf receiver would
  // decide the block was already correct and draw no baseline ticks at all.
  State state;
  const Block empty;
  EXPECT_NE(state.bars, empty.bars);
  EXPECT_NE(state.heightStep, empty.heightStep);
}
