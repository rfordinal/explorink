#include <gtest/gtest.h>

#include <cstdint>

#include "MissingTileRetry.h"

// What the map's "auto retry in N min" line is built from. Both halves were
// wrong once: the countdown was taken from the whole store rather than the
// tiles on screen, and an earlier version read a wrapped clock as a 35,584
// minute wait.

TEST(MissingTileRetry, TheZoomLevelHasToMatch) {
  // The same ground is a different tile at every rung, so a z13 refusal says
  // nothing about the z11 parent the rider is looking at.
  EXPECT_TRUE(missingTileRetryInRange(12, 2200, 1400, 12, 2199, 1399, 2201, 1401));
  EXPECT_FALSE(missingTileRetryInRange(13, 2200, 1400, 12, 2199, 1399, 2201, 1401));
  EXPECT_FALSE(missingTileRetryInRange(11, 2200, 1400, 12, 2199, 1399, 2201, 1401));
}

TEST(MissingTileRetry, TheRectangleIsInclusiveOnBothEdges) {
  EXPECT_TRUE(missingTileRetryInRange(12, 2199, 1399, 12, 2199, 1399, 2201, 1401));
  EXPECT_TRUE(missingTileRetryInRange(12, 2201, 1401, 12, 2199, 1399, 2201, 1401));
  EXPECT_FALSE(missingTileRetryInRange(12, 2198, 1400, 12, 2199, 1399, 2201, 1401));
  EXPECT_FALSE(missingTileRetryInRange(12, 2202, 1400, 12, 2199, 1399, 2201, 1401));
  EXPECT_FALSE(missingTileRetryInRange(12, 2200, 1398, 12, 2199, 1399, 2201, 1401));
  EXPECT_FALSE(missingTileRetryInRange(12, 2200, 1402, 12, 2199, 1399, 2201, 1401));
}

TEST(MissingTileRetry, ASingleTileViewportIsItsOwnRectangle) {
  EXPECT_TRUE(missingTileRetryInRange(12, 2200, 1400, 12, 2200, 1400, 2200, 1400));
  EXPECT_FALSE(missingTileRetryInRange(12, 2201, 1400, 12, 2200, 1400, 2200, 1400));
}

TEST(MissingTileRetry, NeverRefusedIsNotAPendingRetry) {
  // 0 is the store's "no schedule", not a time.
  EXPECT_EQ(missingTileRetryRemainingMs(0, 0), 0u);
  EXPECT_EQ(missingTileRetryRemainingMs(0, 1'000'000), 0u);
}

TEST(MissingTileRetry, APendingRetryCountsDown) {
  EXPECT_EQ(missingTileRetryRemainingMs(100'000, 10'000), 90'000u);
  EXPECT_EQ(missingTileRetryRemainingMs(100'000, 99'999), 1u);
}

TEST(MissingTileRetry, ARetryAlreadyDueIsZero) {
  EXPECT_EQ(missingTileRetryRemainingMs(100'000, 100'000), 0u);
  EXPECT_EQ(missingTileRetryRemainingMs(100'000, 100'001), 0u);
}

TEST(MissingTileRetry, AWrappedClockDoesNotInventAWait) {
  // millis() aliases every ~24.8 days against a signed difference. A retry
  // stamped before the wrap is long expired; reading it as positive is what
  // printed "auto retry in 35584 min" on the panel.
  constexpr uint32_t kLongExpired = 180'000;          // stamped early in the run
  constexpr uint32_t kDeepIntoTheRun = 2'160'000'000; // ~25 days of uptime
  EXPECT_EQ(missingTileRetryRemainingMs(kLongExpired, kDeepIntoTheRun), 0u);
}

TEST(MissingTileRetry, ARetryStampedAcrossTheWrapStillCountsDown) {
  // now near 2^32, deadline 5 s later and wrapped to a small number: the
  // unsigned subtraction is still the real distance.
  constexpr uint32_t kNow = 0xFFFFFFFFu - 2'000u;
  constexpr uint32_t kRetryAt = kNow + 5'000u;  // wraps
  EXPECT_EQ(missingTileRetryRemainingMs(kRetryAt, kNow), 5'000u);
}
