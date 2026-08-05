// The grey encoding is silent when it is wrong: a flipped plane bit nudges the
// complement of what was drawn, and the only way to notice is to look at the
// panel. These tests pin the table in docs/eink-grayscale.md ("Per-pixel
// encoding") so a future edit to GrayShade.h has to break a test first.

#include <GrayShade.h>
#include <gtest/gtest.h>

namespace {

// The BW base frame: black and BOTH greys are ink. This is the fact that makes
// a lost grey read black instead of white.
TEST(GrayShade, BasePassInksEverythingButWhite) {
  EXPECT_FALSE(grayInks(GrayShade::White, GrayPass::Base));
  EXPECT_TRUE(grayInks(GrayShade::LightGray, GrayPass::Base));
  EXPECT_TRUE(grayInks(GrayShade::DarkGray, GrayPass::Base));
  EXPECT_TRUE(grayInks(GrayShade::Black, GrayPass::Base));
}

// Base pass draws into the framebuffer, where true == black. No inversion here.
TEST(GrayShade, BasePassStateIsNotInverted) {
  EXPECT_TRUE(grayPixelState(GrayShade::Black, GrayPass::Base));
  EXPECT_TRUE(grayPixelState(GrayShade::DarkGray, GrayPass::Base));
  EXPECT_FALSE(grayPixelState(GrayShade::White, GrayPass::Base));
}

// MSB is the superset: set for both greys, so it means "this pixel is grey".
TEST(GrayShade, MsbPlaneCoversBothGreys) {
  EXPECT_TRUE(grayInks(GrayShade::LightGray, GrayPass::Msb));
  EXPECT_TRUE(grayInks(GrayShade::DarkGray, GrayPass::Msb));
  EXPECT_FALSE(grayInks(GrayShade::Black, GrayPass::Msb));
  EXPECT_FALSE(grayInks(GrayShade::White, GrayPass::Msb));
}

// LSB is the darker grey alone.
TEST(GrayShade, LsbPlaneIsDarkGreyOnly) {
  EXPECT_TRUE(grayInks(GrayShade::DarkGray, GrayPass::Lsb));
  EXPECT_FALSE(grayInks(GrayShade::LightGray, GrayPass::Lsb));
  EXPECT_FALSE(grayInks(GrayShade::Black, GrayPass::Lsb));
  EXPECT_FALSE(grayInks(GrayShade::White, GrayPass::Lsb));
}

// Plane passes invert: drawPixel(state=false) is what sets the bit, and a set
// bit is what asks for the nudge.
TEST(GrayShade, PlanePassesInvertPixelState) {
  EXPECT_FALSE(grayPixelState(GrayShade::DarkGray, GrayPass::Lsb));
  EXPECT_FALSE(grayPixelState(GrayShade::DarkGray, GrayPass::Msb));
  EXPECT_FALSE(grayPixelState(GrayShade::LightGray, GrayPass::Msb));
  // Not grey in this plane -> clear the bit -> LUT slot 00, no drive.
  EXPECT_TRUE(grayPixelState(GrayShade::LightGray, GrayPass::Lsb));
  EXPECT_TRUE(grayPixelState(GrayShade::Black, GrayPass::Msb));
  EXPECT_TRUE(grayPixelState(GrayShade::White, GrayPass::Msb));
}

// The LUT slot table, exactly as the driver reads it (Ssd1677Luts.h:11-42):
// black/white 00 = no drive, light grey 10 = mid nudge, dark grey 11 = stronger.
TEST(GrayShade, PlaneBitsMatchTheLutSlotTable) {
  struct Row {
    GrayShade shade;
    bool msbBit;
    bool lsbBit;
  };
  constexpr Row rows[] = {
      {GrayShade::Black, false, false},
      {GrayShade::White, false, false},
      {GrayShade::LightGray, true, false},
      {GrayShade::DarkGray, true, true},
  };

  for (const Row& row : rows) {
    EXPECT_EQ(grayPlaneBit(row.shade, GrayPass::Msb), row.msbBit) << "shade " << static_cast<int>(row.shade);
    EXPECT_EQ(grayPlaneBit(row.shade, GrayPass::Lsb), row.lsbBit) << "shade " << static_cast<int>(row.shade);
  }
}

// Unused slot 01 (LSB set, MSB clear) must be unreachable: no shade may ask for
// the darker nudge without also claiming to be grey.
TEST(GrayShade, NoShadeReachesTheUnusedSlot) {
  constexpr GrayShade all[] = {GrayShade::White, GrayShade::LightGray, GrayShade::DarkGray, GrayShade::Black};
  for (const GrayShade shade : all) {
    if (grayPlaneBit(shade, GrayPass::Lsb)) {
      EXPECT_TRUE(grayPlaneBit(shade, GrayPass::Msb)) << "shade " << static_cast<int>(shade) << " hits slot 01";
    }
  }
}

// Everything above is constexpr, so the table is also fixed at compile time.
static_assert(grayPixelState(GrayShade::Black, GrayPass::Base), "black is ink in the base frame");
static_assert(!grayPixelState(GrayShade::DarkGray, GrayPass::Lsb), "dark grey sets the LSB bit");
static_assert(grayPlaneBit(GrayShade::LightGray, GrayPass::Msb) && !grayPlaneBit(GrayShade::LightGray, GrayPass::Lsb),
              "light grey is LUT slot 10");

}  // namespace
