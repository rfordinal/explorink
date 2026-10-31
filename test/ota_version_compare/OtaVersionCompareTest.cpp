#include <gtest/gtest.h>

#include "OtaVersionCompare.h"

using ota_version::isVersionNewer;
using ota_version::parseVersion;

TEST(OtaVersionCompare, ParsesBareVersion) {
  int major, minor, patch;
  ASSERT_TRUE(parseVersion("0.2.1-alpha", &major, &minor, &patch));
  EXPECT_EQ(major, 0);
  EXPECT_EQ(minor, 2);
  EXPECT_EQ(patch, 1);
}

TEST(OtaVersionCompare, ParsesPlainTagWithVPrefix) {
  int major, minor, patch;
  ASSERT_TRUE(parseVersion("explorink-v0.2.1-alpha", &major, &minor, &patch));
  EXPECT_EQ(major, 0);
  EXPECT_EQ(minor, 2);
  EXPECT_EQ(patch, 1);
}

TEST(OtaVersionCompare, ParsesDevicePrefixedTag) {
  // Device names embed digits ("x4pro", "t5s3pro") -- the marker must be the
  // "-v<digit>" right before the version, not the first digit anywhere.
  int major, minor, patch;
  ASSERT_TRUE(parseVersion("explorink-x4pro-v0.2.0-alpha", &major, &minor, &patch));
  EXPECT_EQ(major, 0);
  EXPECT_EQ(minor, 2);
  EXPECT_EQ(patch, 0);

  ASSERT_TRUE(parseVersion("explorink-t5s3pro-v1.2.3", &major, &minor, &patch));
  EXPECT_EQ(major, 1);
  EXPECT_EQ(minor, 2);
  EXPECT_EQ(patch, 3);
}

TEST(OtaVersionCompare, ParsesDevBuildVersion) {
  // TRAILINK_VERSION for a dev build: "<base>-dev-<branch>-<sha>".
  int major, minor, patch;
  ASSERT_TRUE(parseVersion("0.2.1-dev-t2008-firmware-board-check-f1ec3e85", &major, &minor, &patch));
  EXPECT_EQ(major, 0);
  EXPECT_EQ(minor, 2);
  EXPECT_EQ(patch, 1);
}

TEST(OtaVersionCompare, RejectsUnparseableTag) {
  int major, minor, patch;
  EXPECT_FALSE(parseVersion("explorink-nightly", &major, &minor, &patch));
  EXPECT_FALSE(parseVersion("", &major, &minor, &patch));
  EXPECT_FALSE(parseVersion(nullptr, &major, &minor, &patch));
}

TEST(OtaVersionCompare, RejectsIncompleteVersion) {
  // Only two components after the marker -- must not silently accept a
  // partial parse (the bug this replaces: sscanf's return value was never
  // checked, leaving minor/patch uninitialized).
  int major, minor, patch;
  EXPECT_FALSE(parseVersion("explorink-v0.2-alpha", &major, &minor, &patch));
}

TEST(OtaVersionCompare, NewerMajorMinorPatch) {
  EXPECT_TRUE(isVersionNewer("0.2.1", "explorink-v1.0.0"));
  EXPECT_TRUE(isVersionNewer("0.2.1", "explorink-v0.3.0"));
  EXPECT_TRUE(isVersionNewer("0.2.1", "explorink-v0.2.2"));
}

TEST(OtaVersionCompare, NotNewerWhenOlderOrEqual) {
  EXPECT_FALSE(isVersionNewer("0.2.1", "explorink-v0.2.0"));
  EXPECT_FALSE(isVersionNewer("0.2.1", "explorink-v0.2.1"));
  EXPECT_FALSE(isVersionNewer("1.0.0", "explorink-v0.9.9"));
}

TEST(OtaVersionCompare, RcBuildTreatsSameVersionAsNewer) {
  // An RC build of a version is superseded by that same version's final cut.
  EXPECT_TRUE(isVersionNewer("0.2.1-rc+abcdef", "explorink-v0.2.1"));
  EXPECT_FALSE(isVersionNewer("0.2.1-rc+abcdef", "explorink-v0.2.0"));
}

TEST(OtaVersionCompare, RealWorldTagShapes) {
  // The actual shapes seen in docs/public-release.md.
  EXPECT_TRUE(isVersionNewer("0.1.0", "explorink-v0.2.0-alpha"));
  EXPECT_TRUE(isVersionNewer("0.2.0-x4pro", "explorink-x4pro-v0.2.1-alpha"));
  EXPECT_FALSE(isVersionNewer("0.2.1-alpha", "explorink-v0.2.1-alpha"));
}

TEST(OtaVersionCompare, UnparseableLatestNeverNewer) { EXPECT_FALSE(isVersionNewer("0.2.1", "not-a-version-tag")); }
