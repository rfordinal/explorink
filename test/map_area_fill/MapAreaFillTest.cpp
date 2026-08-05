// A hatch must stay inside its ring.
//
// That is the whole failure mode of this code. Hatch is drawn as scan lines
// clipped to the polygon by pairing edge crossings, and every way of getting
// that pairing wrong -- counting a shared vertex twice, an off-by-one on the
// edge test, an odd crossing count -- produces a line that leaks out of the
// building and across the map. It looks like a stray road, not like a bug in a
// fill, so it needs a test that says "no ink outside the shape" rather than
// "some ink appeared".
#include <gtest/gtest.h>

#include <vector>

#include "MapAreaFill.h"
#include "PpmCanvas.h"

namespace {

constexpr int kSize = 120;

bool blackAt(const PpmCanvas& canvas, int x, int y) { return canvas.pixels()[static_cast<size_t>(y) * kSize + x] != 0; }

int blackCount(const PpmCanvas& canvas) {
  int count = 0;
  for (uint8_t value : canvas.pixels()) {
    if (value) ++count;
  }
  return count;
}

// Even-odd point-in-polygon over the ring, used as the independent oracle the
// fill is checked against.
bool insideRing(const std::vector<int16_t>& xs, const std::vector<int16_t>& ys, int px, int py) {
  bool inside = false;
  const size_t n = xs.size() - 1;  // ring is closed, last point repeats the first
  for (size_t i = 0; i < n; ++i) {
    const size_t j = (i + 1) % n;
    const bool straddles = (ys[i] > py) != (ys[j] > py);
    if (!straddles) continue;
    const double t = static_cast<double>(py - ys[i]) / (ys[j] - ys[i]);
    if (px < xs[i] + t * (xs[j] - xs[i])) inside = !inside;
  }
  return inside;
}

struct Ring {
  std::vector<int16_t> xs, ys;
};

Ring square(int x, int y, int size) {
  return Ring{{static_cast<int16_t>(x), static_cast<int16_t>(x + size), static_cast<int16_t>(x + size),
               static_cast<int16_t>(x), static_cast<int16_t>(x)},
              {static_cast<int16_t>(y), static_cast<int16_t>(y), static_cast<int16_t>(y + size),
               static_cast<int16_t>(y + size), static_cast<int16_t>(y)}};
}

// L-shape: the concave case, where a scan line crosses the ring four times and
// the middle pair must be left white.
Ring lShape() { return Ring{{20, 80, 80, 50, 50, 20, 20}, {20, 20, 50, 50, 90, 90, 20}}; }

// A triangle, so the crossings are on sloped edges rather than axis-aligned
// ones, and one vertex is shared by two edges at a scan line's exact height.
Ring triangle() { return Ring{{60, 100, 20, 60}, {20, 90, 90, 20}}; }

void expectHatchStaysInside(const Ring& ring, MapAreaFill::Pattern pattern, int spacing) {
  PpmCanvas canvas(kSize, kSize);
  MapAreaFill::hatchRing(canvas, ring.xs.data(), ring.ys.data(), static_cast<uint16_t>(ring.xs.size()), pattern,
                         spacing, MapInk::Black);
  ASSERT_GT(blackCount(canvas), 0) << "hatch drew nothing at all";

  int leaks = 0;
  for (int y = 0; y < kSize; ++y) {
    for (int x = 0; x < kSize; ++x) {
      if (!blackAt(canvas, x, y)) continue;
      if (insideRing(ring.xs, ring.ys, x, y)) continue;
      // One pixel of tolerance, diagonals included: crossings are rounded to
      // whole pixels, and the even-odd oracle counts a boundary pixel as
      // outside. A ring's corner pixel has its only interior neighbour on the
      // diagonal, so a 4-neighbour tolerance reports that corner as a leak.
      bool onBoundary = false;
      for (int dy = -1; dy <= 1 && !onBoundary; ++dy) {
        for (int dx = -1; dx <= 1 && !onBoundary; ++dx) {
          if (dx == 0 && dy == 0) continue;
          if (insideRing(ring.xs, ring.ys, x + dx, y + dy)) onBoundary = true;
        }
      }
      if (onBoundary) continue;
      ++leaks;
    }
  }
  EXPECT_EQ(leaks, 0) << "hatch leaked outside the ring";
}

}  // namespace

TEST(MapAreaFill, HatchStaysInsideASquare) {
  const Ring ring = square(20, 20, 60);
  for (auto pattern : {MapAreaFill::Pattern::Horizontal, MapAreaFill::Pattern::Vertical, MapAreaFill::Pattern::Cross,
                       MapAreaFill::Pattern::Diagonal, MapAreaFill::Pattern::AntiDiagonal}) {
    expectHatchStaysInside(ring, pattern, 5);
  }
}

TEST(MapAreaFill, HatchStaysInsideAConcaveRing) {
  const Ring ring = lShape();
  for (auto pattern : {MapAreaFill::Pattern::Horizontal, MapAreaFill::Pattern::Vertical, MapAreaFill::Pattern::Cross,
                       MapAreaFill::Pattern::Diagonal, MapAreaFill::Pattern::AntiDiagonal}) {
    expectHatchStaysInside(ring, pattern, 5);
  }
}

TEST(MapAreaFill, HatchStaysInsideASlopedRing) {
  const Ring ring = triangle();
  for (auto pattern : {MapAreaFill::Pattern::Horizontal, MapAreaFill::Pattern::Cross, MapAreaFill::Pattern::Diagonal}) {
    expectHatchStaysInside(ring, pattern, 4);
  }
}

// The notch of an L must stay white, or the "fill" is just a bounding box.
TEST(MapAreaFill, ConcaveNotchIsLeftWhite) {
  const Ring ring = lShape();
  PpmCanvas canvas(kSize, kSize);
  MapAreaFill::hatchRing(canvas, ring.xs.data(), ring.ys.data(), static_cast<uint16_t>(ring.xs.size()),
                         MapAreaFill::Pattern::Cross, 4, MapInk::Black);
  // (70, 70) is in the notch: right of the L's stem, below its arm.
  ASSERT_FALSE(insideRing(ring.xs, ring.ys, 70, 70)) << "test fixture wrong, that point should be outside";
  EXPECT_FALSE(blackAt(canvas, 70, 70));
}

// Every "no fill" spelling must draw nothing rather than fall through to a
// solid blob -- on 1-bit a solid building swallows the roads around it.
TEST(MapAreaFill, NoFillCasesDrawNothing) {
  const Ring ring = square(20, 20, 60);
  const uint16_t count = static_cast<uint16_t>(ring.xs.size());

  PpmCanvas none(kSize, kSize);
  MapAreaFill::hatchRing(none, ring.xs.data(), ring.ys.data(), count, MapAreaFill::Pattern::None, 5, MapInk::Black);
  EXPECT_EQ(blackCount(none), 0);

  PpmCanvas zeroSpacing(kSize, kSize);
  MapAreaFill::hatchRing(zeroSpacing, ring.xs.data(), ring.ys.data(), count, MapAreaFill::Pattern::Cross, 0,
                         MapInk::Black);
  EXPECT_EQ(blackCount(zeroSpacing), 0);

  PpmCanvas degenerate(kSize, kSize);
  MapAreaFill::hatchRing(degenerate, ring.xs.data(), ring.ys.data(), 3, MapAreaFill::Pattern::Cross, 5, MapInk::Black);
  EXPECT_EQ(blackCount(degenerate), 0);
}

// A wider spacing must draw strictly less ink. Sounds obvious; it is the
// property a style author is actually tuning, and an anchoring bug can break it.
TEST(MapAreaFill, WiderSpacingDrawsLessInk) {
  const Ring ring = square(10, 10, 100);
  const uint16_t count = static_cast<uint16_t>(ring.xs.size());
  int previous = -1;
  for (int spacing : {2, 4, 8, 16}) {
    PpmCanvas canvas(kSize, kSize);
    MapAreaFill::hatchRing(canvas, ring.xs.data(), ring.ys.data(), count, MapAreaFill::Pattern::Cross, spacing,
                           MapInk::Black);
    const int ink = blackCount(canvas);
    if (previous >= 0) EXPECT_LT(ink, previous) << "spacing " << spacing;
    previous = ink;
  }
}

// --- tones ---------------------------------------------------------------
//
// A tone is what a built-up area actually uses; the hatch above is for shapes
// big enough to carry lines. Three properties matter and each has bitten
// something: it must stay inside the ring, it must land at the density the name
// promises, and its phase must come from the screen rather than from the shape.

TEST(MapAreaFill, ToneStaysInsideEveryRing) {
  const Ring rings[] = {square(20, 20, 60), lShape(), triangle()};
  for (const Ring& ring : rings) {
    for (auto tone : {MapAreaTone::Stipple, MapAreaTone::Light, MapAreaTone::Dark, MapAreaTone::Solid}) {
      PpmCanvas canvas(kSize, kSize);
      MapAreaFill::toneRing(canvas, ring.xs.data(), ring.ys.data(), static_cast<uint16_t>(ring.xs.size()), tone);
      ASSERT_GT(blackCount(canvas), 0);
      for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
          if (!blackAt(canvas, x, y)) continue;
          if (insideRing(ring.xs, ring.ys, x, y)) continue;
          bool onBoundary = false;
          for (int dy = -1; dy <= 1 && !onBoundary; ++dy) {
            for (int dx = -1; dx <= 1 && !onBoundary; ++dx) {
              if (dx == 0 && dy == 0) continue;
              if (insideRing(ring.xs, ring.ys, x + dx, y + dy)) onBoundary = true;
            }
          }
          ASSERT_TRUE(onBoundary) << "tone leaked at " << x << "," << y;
        }
      }
    }
  }
}

// The names are a promise about density: 1 in 9, 1 in 4, 1 in 2, all of it.
// A style author picking "stipple" over "dark" is picking a weight.
TEST(MapAreaFill, ToneDensityMatchesItsName) {
  const Ring ring = square(10, 10, 90);  // 90x90 interior, big enough to average
  const uint16_t count = static_cast<uint16_t>(ring.xs.size());
  const double area = 90.0 * 90.0;

  struct Expectation {
    MapAreaTone tone;
    double fraction;
  };
  const Expectation expectations[] = {{MapAreaTone::Stipple, 1.0 / 9.0},
                                       {MapAreaTone::Light, 1.0 / 4.0},
                                       {MapAreaTone::Dark, 1.0 / 2.0},
                                       {MapAreaTone::Solid, 1.0}};
  for (const Expectation& expected : expectations) {
    PpmCanvas canvas(kSize, kSize);
    MapAreaFill::toneRing(canvas, ring.xs.data(), ring.ys.data(), count, expected.tone);
    const double got = blackCount(canvas) / area;
    EXPECT_NEAR(got, expected.fraction, 0.06) << "tone " << static_cast<int>(expected.tone);
  }
}

// Two rings side by side must share one texture, which only holds if the
// pattern is anchored in screen space. Anchor it to each shape instead and a row
// of houses turns into noise -- the whole reason buildings looked bad.
TEST(MapAreaFill, ToneIsAnchoredToTheScreenNotTheShape) {
  const Ring left = square(20, 20, 21);
  const Ring right = square(41, 20, 21);  // shares the edge at x = 41

  PpmCanvas separate(kSize, kSize);
  MapAreaFill::toneRing(separate, left.xs.data(), left.ys.data(), 5, MapAreaTone::Light);
  MapAreaFill::toneRing(separate, right.xs.data(), right.ys.data(), 5, MapAreaTone::Light);

  // One ring covering both, drawn in one go. If the phase came from the shape,
  // the two-ring version would differ from this.
  const Ring both = square(20, 20, 42);
  PpmCanvas together(kSize, kSize);
  MapAreaFill::toneRing(together, both.xs.data(), both.ys.data(), 5, MapAreaTone::Light);

  int mismatches = 0;
  for (int y = 21; y < 40; ++y) {
    for (int x = 21; x < 40; ++x) {
      if (blackAt(separate, x, y) != blackAt(together, x, y)) ++mismatches;
    }
  }
  EXPECT_EQ(mismatches, 0);
}

TEST(MapAreaFill, ToneNoneDrawsNothing) {
  const Ring ring = square(20, 20, 60);
  PpmCanvas canvas(kSize, kSize);
  MapAreaFill::toneRing(canvas, ring.xs.data(), ring.ys.data(), 5, MapAreaTone::None);
  EXPECT_EQ(blackCount(canvas), 0);
}

// The device paints Light and Dark through GfxRenderer::fillRectDither and the
// others pixel by pixel. That split must stay in sync with which tones
// GfxRenderer actually has, or the preview and the panel diverge silently.
TEST(MapAreaFill, NativeDitherClaimMatchesTheToneList) {
  EXPECT_TRUE(MapTone::hasNativeDither(MapAreaTone::Light));
  EXPECT_TRUE(MapTone::hasNativeDither(MapAreaTone::Dark));
  EXPECT_TRUE(MapTone::hasNativeDither(MapAreaTone::Solid));
  EXPECT_FALSE(MapTone::hasNativeDither(MapAreaTone::Stipple));
  EXPECT_FALSE(MapTone::hasNativeDither(MapAreaTone::None));
}

TEST(MapAreaFill, OutlineFollowsTheRingAndZeroWidthDrawsNothing) {
  const Ring ring = square(20, 20, 60);
  const uint16_t count = static_cast<uint16_t>(ring.xs.size());

  PpmCanvas drawn(kSize, kSize);
  MapAreaFill::outlineRing(drawn, ring.xs.data(), ring.ys.data(), count, 1, MapInk::Black);
  EXPECT_TRUE(blackAt(drawn, 20, 20));
  EXPECT_TRUE(blackAt(drawn, 50, 20));
  EXPECT_FALSE(blackAt(drawn, 50, 50)) << "outline must not fill the interior";

  PpmCanvas skipped(kSize, kSize);
  MapAreaFill::outlineRing(skipped, ring.xs.data(), ring.ys.data(), count, 0, MapInk::Black);
  EXPECT_EQ(blackCount(skipped), 0);
}
