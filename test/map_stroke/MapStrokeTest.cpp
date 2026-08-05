// A wide road must look wide, and solid, at every bearing.
//
// This suite exists because two thick-line implementations failed that on real
// data before it did (both found 2026-08-05):
//
//   1. GfxRenderer::drawLine(lineWidth) stacks its copies downward in y
//      (GfxRenderer.cpp:713-717). A north-south road came out one pixel wide
//      whatever the style said, an east-west one full width.
//   2. Offsetting copies along the true perpendicular striped every diagonal:
//      consecutive copies land 1.41 px apart, so the road drew as parallel
//      hairlines with white between them.
//
// Both are invisible in a unit test that only checks "some pixels are black",
// and both are obvious in a rendered map. So these tests measure the two things
// that were actually wrong: perpendicular thickness, and gaps.
#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include "MapStroke.h"
#include "PpmCanvas.h"

namespace {

constexpr int kSize = 201;
constexpr int kCentre = 100;

// Ink coverage of a line through the centre at `degrees`, drawn `width` wide.
int blackPixels(const PpmCanvas& canvas) {
  int count = 0;
  for (uint8_t value : canvas.pixels()) {
    if (value) ++count;
  }
  return count;
}

PpmCanvas renderRay(double degrees, int width, int length) {
  PpmCanvas canvas(kSize, kSize);
  const double radians = degrees * M_PI / 180.0;
  const int x2 = kCentre + static_cast<int>(std::lround(std::cos(radians) * length));
  const int y2 = kCentre + static_cast<int>(std::lround(std::sin(radians) * length));
  canvas.drawLine(kCentre, kCentre, x2, y2, width, MapInk::Black);
  return canvas;
}

// Longest run of white pixels strictly between the first and last black pixel
// of a row or column. A gap inside the stroke shows up here and nowhere else.
int longestInteriorWhiteRun(const PpmCanvas& canvas, bool byRow) {
  const std::vector<uint8_t>& pixels = canvas.pixels();
  int worst = 0;
  for (int outer = 0; outer < kSize; ++outer) {
    int firstBlack = -1, lastBlack = -1;
    for (int inner = 0; inner < kSize; ++inner) {
      const int x = byRow ? inner : outer;
      const int y = byRow ? outer : inner;
      if (!pixels[static_cast<size_t>(y) * kSize + x]) continue;
      if (firstBlack < 0) firstBlack = inner;
      lastBlack = inner;
    }
    if (firstBlack < 0) continue;
    int run = 0;
    for (int inner = firstBlack; inner <= lastBlack; ++inner) {
      const int x = byRow ? inner : outer;
      const int y = byRow ? outer : inner;
      if (pixels[static_cast<size_t>(y) * kSize + x]) {
        run = 0;
        continue;
      }
      ++run;
      if (run > worst) worst = run;
    }
  }
  return worst;
}

}  // namespace

// The defect that started this: thickness must not depend on bearing. 16
// headings because that is what the device snaps to (MapHeading).
TEST(MapStroke, WidthHoldsAtEveryHeading) {
  constexpr int kWidth = 8;
  constexpr int kLength = 80;
  // Area of a `w` wide, `len` long stroke, ignoring the ends. Generous bounds:
  // this is catching 1 px versus 8 px, not arguing about a rounding.
  const double expected = static_cast<double>(kWidth) * kLength;

  for (int step = 0; step < 16; ++step) {
    const double degrees = step * 22.5;
    const PpmCanvas canvas = renderRay(degrees, kWidth, kLength);
    const double area = blackPixels(canvas);
    EXPECT_GT(area, expected * 0.75) << "heading " << degrees << " drew too little ink -- thin road";
    EXPECT_LT(area, expected * 1.4) << "heading " << degrees << " drew too much ink -- fat road";
  }
}

// The second defect: a diagonal stroke must be solid, not striped.
TEST(MapStroke, NoInteriorGapsAtEveryHeading) {
  constexpr int kWidth = 8;
  for (int step = 0; step < 16; ++step) {
    const double degrees = step * 22.5;
    const PpmCanvas canvas = renderRay(degrees, kWidth, 80);
    // Scan both ways: a gap left by stacking along x hides from a row scan.
    EXPECT_EQ(longestInteriorWhiteRun(canvas, true), 0) << "heading " << degrees << " has a row gap";
    EXPECT_EQ(longestInteriorWhiteRun(canvas, false), 0) << "heading " << degrees << " has a column gap";
  }
}

// A 1 px line means one pixel, at every angle. Scaling the copy count for
// bearing would make a diagonal hairline two pixels thick -- and the golden
// fixture (test/map_tile_reader) renders every class at 1 px precisely so it
// does not depend on any of the arithmetic above.
TEST(MapStroke, HairlineStaysOnePixelWide) {
  for (int step = 0; step < 16; ++step) {
    const MapStroke::Stack stack = MapStroke::stackFor(0, 0, 40, step * 3, 1);
    EXPECT_EQ(stack.count, 1) << "heading step " << step;
    EXPECT_EQ(stack.first, 0);
  }
}

// Stacking has to happen along the axis the line moves fastest in, or
// consecutive copies are further apart than one pixel of the line itself.
TEST(MapStroke, StacksAcrossTheDominantAxis) {
  EXPECT_FALSE(MapStroke::stackFor(0, 0, 10, 100, 6).alongY) << "steep line must stack in x";
  EXPECT_TRUE(MapStroke::stackFor(0, 0, 100, 10, 6).alongY) << "shallow line must stack in y";
}

// A diagonal needs more copies than a horizontal one to reach the same
// perpendicular width: len/major is 1.41 at 45 degrees.
TEST(MapStroke, DiagonalNeedsMoreCopiesThanHorizontal) {
  const MapStroke::Stack flat = MapStroke::stackFor(0, 0, 100, 0, 8);
  const MapStroke::Stack diagonal = MapStroke::stackFor(0, 0, 100, 100, 8);
  EXPECT_EQ(flat.count, 8);
  EXPECT_EQ(diagonal.count, 12);  // ceil(8 * 1.4142)
}

// A way with one point repeated has no direction. It must not divide by zero
// or draw nothing.
TEST(MapStroke, ZeroLengthSegmentStillDrawsSomething) {
  const MapStroke::Stack stack = MapStroke::stackFor(50, 50, 50, 50, 6);
  EXPECT_EQ(stack.count, 6);

  PpmCanvas canvas(kSize, kSize);
  canvas.drawLine(50, 50, 50, 50, 6, MapInk::Black);
  EXPECT_GT(blackPixels(canvas), 0);
}
