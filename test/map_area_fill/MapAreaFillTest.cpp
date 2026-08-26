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

#include <cmath>

#include <vector>

#include "MapAreaFill.h"
#include "MapTextMask.h"
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
  const Expectation expectations[] = {{MapAreaTone::Micro, 1.0 / 16.0},
                                       {MapAreaTone::MicroStagger, 1.0 / 16.0},
                                       {MapAreaTone::Stipple, 1.0 / 9.0},
                                       {MapAreaTone::StippleStagger, 1.0 / 9.0},
                                       {MapTone::dots(5, false), 1.0 / 25.0},
                                       {MapTone::dots(5, true), 1.0 / 25.0},
                                       {MapAreaTone::Dense, 1.0 / 4.0},
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

// The halo is a dilation in mask space, and its output box has to grow with the
// radius. Reusing the input box clips the outermost ring, which is invisible at
// radius 1 -- glyphs rarely touch their own box edge -- and obvious at 2.
TEST(MapTextMask, DilationGrowsTheBoxAndTheInk) {
  MapTextMask mask;
  mask.reset(9, 9);
  mask.set(4, 4);  // one pixel, dead centre

  for (const int radius : {1, 2, 3}) {
    MapTextMask grown;
    ASSERT_TRUE(mask.dilateInto(grown, radius)) << radius;
    EXPECT_EQ(grown.w, 9 + 2 * radius) << radius;
    EXPECT_EQ(grown.h, 9 + 2 * radius) << radius;
    // A single pixel becomes a (2r+1) square, so nothing is lost at the edges.
    int inked = 0;
    for (int y = 0; y < grown.h; ++y) {
      for (int x = 0; x < grown.w; ++x) {
        if (grown.get(x, y)) ++inked;
      }
    }
    EXPECT_EQ(inked, (2 * radius + 1) * (2 * radius + 1)) << radius;
    // And it stays centred, so the blit needs no offset of its own.
    EXPECT_TRUE(grown.get(grown.w / 2, grown.h / 2)) << radius;
  }

  // Radius 0 means no halo, and says so rather than returning an empty mask that
  // the caller would blit for nothing.
  MapTextMask none;
  EXPECT_FALSE(mask.dilateInto(none, 0));

  // A radius that would not fit is refused, so a clipped halo cannot be drawn.
  MapTextMask wide;
  wide.reset(MapTextMask::kMaxW, MapTextMask::kMaxH);
  MapTextMask overflow;
  EXPECT_FALSE(wide.dilateInto(overflow, 1));
}

// The text rotation basis must be a rotation, never a reflection. This got it
// wrong twice: once by picking the turn from the wrong vector, and once by taking
// the reading direction and the up direction as two independent inputs -- that
// pair has determinant -1, so every height number came out mirrored, which is
// only visible if you look at a label big enough to read.
TEST(MapTextMask, BasisIsARotationAndNeverAReflection) {
  // A full circle of up directions, including the axes and the diagonals.
  for (int deg = 0; deg < 360; deg += 5) {
    const double rad = deg * 3.14159265358979 / 180.0;
    const int32_t upX = static_cast<int32_t>(1000.0 * std::cos(rad));
    const int32_t upY = static_cast<int32_t>(1000.0 * std::sin(rad));
    if (upX == 0 && upY == 0) continue;
    int rx = 0, ry = 0, dx = 0, dy = 0;
    ASSERT_TRUE(mapTextBasisFromUp(upX, upY, rx, ry, dx, dy)) << deg;

    // Determinant positive: a rotation. Negative would be a mirror.
    const long det = static_cast<long>(rx) * dy - static_cast<long>(ry) * dx;
    EXPECT_GT(det, 0) << "deg " << deg << ": basis is a reflection";
    // And it is a rotation of the right size -- 1024 * 1024, within rounding.
    EXPECT_NEAR(static_cast<double>(det) / (1024.0 * 1024.0), 1.0, 0.02) << "deg " << deg;

    // `down` is the opposite of the asked-for up, so the glyphs' top points at it.
    EXPECT_LT(static_cast<long>(dx) * upX + static_cast<long>(dy) * upY, 0) << "deg " << deg;
    // And the two axes stay perpendicular.
    const long dot = static_cast<long>(rx) * dx + static_cast<long>(ry) * dy;
    EXPECT_NEAR(static_cast<double>(dot) / (1024.0 * 1024.0), 0.0, 0.02) << "deg " << deg;
  }
}

// Zero up must not divide by anything. Upright is the right answer: a number with
// no orientation still says its height.
TEST(MapTextMask, ZeroUpFallsBackToUpright) {
  int rx = 0, ry = 0, dx = 0, dy = 0;
  EXPECT_FALSE(mapTextBasisFromUp(0, 0, rx, ry, dx, dy));
  EXPECT_EQ(rx, 1024);
  EXPECT_EQ(ry, 0);
  EXPECT_EQ(dx, 0);
  EXPECT_EQ(dy, 1024);
}

// A staggered dot grid has the same density as its regular twin and a different
// picture. Both halves matter: same density is what makes them comparable on the
// panel with nothing else moving, and a different picture is the whole point.
TEST(MapAreaFill, StaggerChangesThePatternAndNotTheDensity) {
  const Ring ring = square(10, 10, 90);
  const uint16_t count = static_cast<uint16_t>(ring.xs.size());

  for (const int period : {3, 4, 5}) {
    PpmCanvas plain(kSize, kSize);
    PpmCanvas offset(kSize, kSize);
    MapAreaFill::toneRing(plain, ring.xs.data(), ring.ys.data(), count, MapTone::dots(period, false));
    MapAreaFill::toneRing(offset, ring.xs.data(), ring.ys.data(), count, MapTone::dots(period, true));
    // Near, not equal: in an infinite plane the two densities are identical, and
    // in a 90x90 window the offset shifts which columns fall inside the ring, so
    // the counts differ by a few dots at the edge. Measured 342 against 333 at
    // period 5. Asserting equality would be asserting the window, not the tone.
    const double plainCount = blackCount(plain);
    const double offsetCount = blackCount(offset);
    EXPECT_NEAR(offsetCount / plainCount, 1.0, 0.05) << "period " << period << ": density must not move";
    EXPECT_NE(plain.pixels(), offset.pixels()) << "period " << period << ": stagger must change the picture";
  }
}

// The period is packed into the tone value, so a style can name a density
// directly. If the packing and the decoders ever disagree, every dot tone shifts
// silently -- these are the round trips that would break first.
TEST(MapAreaFill, DotPeriodSurvivesThePacking) {
  for (int period = MapTone::kMinDotPeriod; period <= MapTone::kMaxDotPeriod; ++period) {
    for (const bool stagger : {false, true}) {
      const MapAreaTone tone = MapTone::dots(period, stagger);
      EXPECT_TRUE(MapTone::isDots(tone)) << period;
      EXPECT_EQ(MapTone::dotPeriod(tone), period);
      EXPECT_EQ(MapTone::isStaggered(tone), stagger) << period;
      // A dot grid must never be claimed by the native dither path: that would
      // paint GfxRenderer's period-2 pattern instead of the asked-for one.
      EXPECT_FALSE(MapTone::hasNativeDither(tone)) << period;
    }
  }
  // Out-of-range periods clamp rather than producing a value that decodes as
  // something else entirely.
  EXPECT_EQ(MapTone::dotPeriod(MapTone::dots(1, false)), MapTone::kMinDotPeriod);
  EXPECT_EQ(MapTone::dotPeriod(MapTone::dots(99, false)), MapTone::kMaxDotPeriod);
  // And the fixed patterns stay out of the dot range.
  for (const MapAreaTone tone : {MapAreaTone::None, MapAreaTone::Light, MapAreaTone::Dark, MapAreaTone::Solid}) {
    EXPECT_FALSE(MapTone::isDots(tone)) << static_cast<int>(tone);
    EXPECT_EQ(MapTone::dotPeriod(tone), 0);
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
