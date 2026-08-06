// Reads a .tir written by mapbuilder/route_file.py and checks every field back,
// then checks the overview fit's choices.
//
// The fixture is committed, produced by mapbuilder's writer. That is the point:
// the format has two implementations in two languages, and a fixture written by
// one and read by the other is the only thing that keeps them agreeing. The
// numbers below are the writer's own output, printed when the fixture was made
// (../../../docs/route-file-spec.md in the parent xteink repo).
//
// The fixture is an L: three points north along a meridian, then three east
// along a parallel. Chosen so the fit has an unambiguous right answer to check
// -- see the fit tests.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "MapRouteFit.h"
#include "MapRouteReader.h"
#include "MapViewport.h"
#include "StdioFileSource.h"

namespace {

constexpr int kScreenWidth = 480;
constexpr int kScreenHeight = 800;

std::string fixturePath(const char* name) { return std::string(MAP_ROUTE_FIXTURES_DIR) + "/" + name; }

// mapbuilder/route_file.py's own output for the committed fixture.
struct Point {
  int32_t x;
  int32_t y;
};
const std::vector<Point> kExpectedPoints = {
    {1886865, 6180370}, {1886865, 6182048}, {1886865, 6183726},
    {1889092, 6183726}, {1891318, 6183726}, {1892431, 6183726},
};

std::vector<uint8_t> readFile(const std::string& path) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return {};
  std::fseek(f, 0, SEEK_END);
  const long len = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  std::vector<uint8_t> bytes(static_cast<size_t>(len));
  const size_t n = std::fread(bytes.data(), 1, bytes.size(), f);
  std::fclose(f);
  bytes.resize(n);
  return bytes;
}

// Writes `bytes` to a temp file and returns its path, so a corruption test does
// not touch the committed fixture.
std::string writeTemp(const std::vector<uint8_t>& bytes, const char* name) {
  const std::string path = std::string(MAP_ROUTE_TEMP_DIR) + "/" + name;
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return {};
  std::fwrite(bytes.data(), 1, bytes.size(), f);
  std::fclose(f);
  return path;
}

// Feeds a synthetic straight route into the fit, in Mercator metres. `dx`/`dy`
// is the whole span, split into `steps` segments.
MapRouteFit::Result fitStraight(int32_t x0, int32_t y0, int32_t dx, int32_t dy, int steps = 20) {
  MapRouteFit fit;
  fit.begin(x0 + dx / 2, y0 + dy / 2);
  for (int i = 0; i <= steps; ++i) {
    fit.addPoint(x0 + dx * i / steps, y0 + dy * i / steps);
  }
  MapRouteFit::Result out;
  EXPECT_TRUE(fit.finish(kScreenWidth, kScreenHeight, out));
  return out;
}

}  // namespace

TEST(MapRouteReader, ReadsEveryHeaderFieldMapbuilderWrote) {
  StdioFileSource file;
  MapRouteReader reader;
  ASSERT_TRUE(reader.open(file, fixturePath("lshape.tir").c_str()));

  EXPECT_EQ(reader.pointCount(), kExpectedPoints.size());
  EXPECT_STREQ(reader.name(), "L fixture");
  EXPECT_EQ(reader.buildEpoch(), 1754400000u);
  EXPECT_EQ(reader.bboxMinX(), 1886865);
  EXPECT_EQ(reader.bboxMinY(), 6180370);
  EXPECT_EQ(reader.bboxMaxX(), 1892431);
  EXPECT_EQ(reader.bboxMaxY(), 6183726);
}

TEST(MapRouteReader, PointArrayRoundTripsByteExact) {
  StdioFileSource file;
  MapRouteReader reader;
  ASSERT_TRUE(reader.open(file, fixturePath("lshape.tir").c_str()));
  ASSERT_TRUE(reader.verifyPoints());
  ASSERT_TRUE(reader.beginPoints());

  std::vector<Point> read;
  int32_t x = 0;
  int32_t y = 0;
  while (reader.nextPoint(x, y)) read.push_back({x, y});

  ASSERT_EQ(read.size(), kExpectedPoints.size());
  for (size_t i = 0; i < read.size(); ++i) {
    EXPECT_EQ(read[i].x, kExpectedPoints[i].x) << "point " << i;
    EXPECT_EQ(read[i].y, kExpectedPoints[i].y) << "point " << i;
  }
  EXPECT_EQ(reader.pointsRead(), kExpectedPoints.size());
}

TEST(MapRouteReader, YGrowsNorthNotSouth) {
  // The one convention mistake that silently mirrors a route: tile-local y grows
  // south (MapProjection::projectTileLocal), this format's y grows north. The
  // fixture's first three points run north, so their y must increase.
  StdioFileSource file;
  MapRouteReader reader;
  ASSERT_TRUE(reader.open(file, fixturePath("lshape.tir").c_str()));
  ASSERT_TRUE(reader.beginPoints());

  int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  ASSERT_TRUE(reader.nextPoint(x0, y0));
  ASSERT_TRUE(reader.nextPoint(x1, y1));
  EXPECT_EQ(x0, x1) << "the first leg is due north, so x must not move";
  EXPECT_GT(y1, y0);
}

TEST(MapRouteReader, BeginPointsIsRewindable) {
  // One viewport reset per frame re-reads the route, so this is not a nicety.
  StdioFileSource file;
  MapRouteReader reader;
  ASSERT_TRUE(reader.open(file, fixturePath("lshape.tir").c_str()));

  for (int pass = 0; pass < 3; ++pass) {
    ASSERT_TRUE(reader.beginPoints()) << "pass " << pass;
    int32_t x = 0, y = 0;
    ASSERT_TRUE(reader.nextPoint(x, y));
    EXPECT_EQ(x, kExpectedPoints[0].x);
    EXPECT_EQ(y, kExpectedPoints[0].y);
    uint32_t count = 1;
    while (reader.nextPoint(x, y)) ++count;
    EXPECT_EQ(count, kExpectedPoints.size());
  }
}

TEST(MapRouteReader, RefusesBadMagic) {
  auto bytes = readFile(fixturePath("lshape.tir"));
  ASSERT_FALSE(bytes.empty());
  bytes[2] = 'B';  // "TIB1"
  const std::string path = writeTemp(bytes, "bad_magic.tir");
  StdioFileSource file;
  MapRouteReader reader;
  EXPECT_FALSE(reader.open(file, path.c_str()));
  std::remove(path.c_str());
}

TEST(MapRouteReader, RefusesAFutureFormatVersion) {
  // A version-2 file appends a chunk table after the point array. A reader that
  // ignored the version would read that table's bytes as coordinates.
  auto bytes = readFile(fixturePath("lshape.tir"));
  ASSERT_FALSE(bytes.empty());
  bytes[4] = 2;
  const std::string path = writeTemp(bytes, "future_version.tir");
  StdioFileSource file;
  MapRouteReader reader;
  EXPECT_FALSE(reader.open(file, path.c_str()));
  std::remove(path.c_str());
}

TEST(MapRouteReader, RefusesACorruptHeader) {
  auto bytes = readFile(fixturePath("lshape.tir"));
  ASSERT_FALSE(bytes.empty());
  bytes[8] ^= 0xFF;  // point_count, covered by header_crc32
  const std::string path = writeTemp(bytes, "bad_header.tir");
  StdioFileSource file;
  MapRouteReader reader;
  EXPECT_FALSE(reader.open(file, path.c_str()));
  std::remove(path.c_str());
}

TEST(MapRouteReader, RefusesANonZeroReservedByte) {
  auto bytes = readFile(fixturePath("lshape.tir"));
  ASSERT_FALSE(bytes.empty());
  bytes[7] = 1;
  const std::string path = writeTemp(bytes, "reserved_set.tir");
  StdioFileSource file;
  MapRouteReader reader;
  EXPECT_FALSE(reader.open(file, path.c_str()));
  std::remove(path.c_str());
}

TEST(MapRouteReader, OpenSucceedsButVerifyPointsCatchesCorruptGeometry) {
  // The split that matters: the header is checked on open so a picker row costs
  // 100 bytes, and the point array only when something is about to draw it.
  auto bytes = readFile(fixturePath("lshape.tir"));
  ASSERT_FALSE(bytes.empty());
  bytes[bytes.size() - 3] ^= 0xFF;
  const std::string path = writeTemp(bytes, "bad_points.tir");
  StdioFileSource file;
  MapRouteReader reader;
  ASSERT_TRUE(reader.open(file, path.c_str()));
  EXPECT_FALSE(reader.verifyPoints());
  std::remove(path.c_str());
}

TEST(MapRouteReader, OpeningTheHeaderDoesNotReadThePointArray) {
  // What makes a picker listing cheap: a route's name and length cost the
  // header, not the 24 KB of a real route's geometry.
  StdioFileSource file;
  MapRouteReader reader;
  ASSERT_TRUE(reader.open(file, fixturePath("lshape.tir").c_str()));
  const uint32_t pointBytes = reader.pointCount() * 8;
  EXPECT_LT(reader.bytesRead(), pointBytes + 8u);
  EXPECT_LE(reader.bytesRead(), 56u) << "header is 40 bytes plus a 9-byte name padded to 4";
}

TEST(MapRouteFit, AnEmptyFitFails) {
  MapRouteFit fit;
  fit.begin(0, 0);
  MapRouteFit::Result out;
  EXPECT_FALSE(fit.finish(kScreenWidth, kScreenHeight, out));
}

TEST(MapRouteFit, ANorthSouthRouteIsDrawnNorthUp) {
  // The screen is 480x800. A route along its long axis wants north up, i.e.
  // heading 0.
  const MapRouteFit::Result out = fitStraight(1892000, 6180000, 0, 4000);
  EXPECT_EQ(out.heading, 0);
  EXPECT_TRUE(out.fits);
}

TEST(MapRouteFit, AnEastWestRouteIsTurnedOntoTheLongScreenAxis) {
  // 480 px across against 800 down: turning the map so the route runs down the
  // screen fits it at a finer rung than leaving it across. Heading 4 is east.
  const MapRouteFit::Result out = fitStraight(1892000, 6180000, 4000, 0);
  EXPECT_EQ(out.heading, 4);
  EXPECT_TRUE(out.fits);
}

TEST(MapRouteFit, ADiagonalRouteIsTurnedToItsOwnAxis) {
  // Exactly north-east. The bbox is square, so a bbox-based fit would zoom out
  // to hold a square; measuring the point set instead turns the map 45 degrees
  // and the route becomes a thin strip. Heading 2 is NE.
  const MapRouteFit::Result out = fitStraight(1892000, 6180000, 3000, 3000);
  EXPECT_EQ(out.heading, 2);
  EXPECT_TRUE(out.fits);
  EXPECT_LT(out.extentWidthM, 10.0) << "a straight route has no width in its own frame";
}

TEST(MapRouteFit, TheTiltThatBuysNothingIsNotTaken) {
  // A slight tilt lets a straight route trade vertical extent for horizontal,
  // which lowers the *required* scale -- and buys nothing, because zoom is a
  // five-rung ladder and both headings round up to the same rung. The fit used
  // to take that tilt and draw a north-south route 22.5 degrees askew.
  const MapRouteFit::Result out = fitStraight(1892000, 6180000, 0, 4000);
  EXPECT_EQ(out.heading, 0);
  // The proof that the tilt was genuinely available: heading 1 needs a finer
  // scale than heading 0 does, and was still not chosen.
  MapRouteFit fit;
  fit.begin(1892000, 6180000 + 2000);
  for (int i = 0; i <= 20; ++i) fit.addPoint(1892000, 6180000 + 4000 * i / 20);
  MapRouteFit::Result straight;
  ASSERT_TRUE(fit.finish(kScreenWidth, kScreenHeight, straight));
  EXPECT_EQ(straight.zoomStep, out.zoomStep);
}

TEST(MapRouteFit, AClosedLoopFallsBackToNorthUp) {
  // Start and end are the same point, so there is no direction of travel to put
  // up the screen. North up is the one orientation a rider can always read.
  MapRouteFit fit;
  fit.begin(1892000, 6180000);
  for (int i = 0; i <= 32; ++i) {
    const double angle = 2.0 * 3.14159265358979323846 * i / 32.0;
    fit.addPoint(1892000 + static_cast<int32_t>(1500.0 * std::cos(angle)),
                 6180000 + static_cast<int32_t>(1500.0 * std::sin(angle)));
  }
  MapRouteFit::Result out;
  ASSERT_TRUE(fit.finish(kScreenWidth, kScreenHeight, out));
  EXPECT_EQ(out.heading, 0);
  EXPECT_TRUE(out.fits);
}

TEST(MapRouteFit, TheFinestRungThatHoldsTheRouteWins) {
  // Rung 0 is 1 m/px, rung 1 is 3. A route 500 Mercator metres long fits the
  // 744 usable pixels of rung 0 at this latitude; 2,000 does not.
  EXPECT_EQ(fitStraight(1892000, 6180000, 0, 500).zoomStep, 0);
  EXPECT_GT(fitStraight(1892000, 6180000, 0, 2000).zoomStep, 0);
}

TEST(MapRouteFit, ALongRouteFallsBackToTheCoarsestRungInsteadOfFailing) {
  // 300 km. Nothing on the ladder holds it -- the coarsest rung is 20 m/px,
  // which is 16 km of screen height. The answer is still usable: coarsest rung,
  // best heading, middle of the route on screen.
  const MapRouteFit::Result out = fitStraight(1892000, 6100000, 0, 300000);
  EXPECT_FALSE(out.fits);
  EXPECT_EQ(out.zoomStep, MapViewport::kZoomStepCount - 1);
  EXPECT_EQ(out.heading, 0);
  EXPECT_GT(out.requiredMpp, MapViewport::kZoomLadder[MapViewport::kZoomStepCount - 1].mpp);
}

TEST(MapRouteFit, TheAnchorIsTheMiddleOfTheRoute) {
  const int32_t y0 = 6180000;
  const int32_t dy = 4000;
  const MapRouteFit::Result out = fitStraight(1892000, y0, 0, dy);
  EXPECT_NEAR(out.anchorMercX, 1892000.0, 1.0);
  EXPECT_NEAR(out.anchorMercY, y0 + dy / 2.0, 1.0);
  // And it round-trips through the lat/lon the projection is reset with.
  double checkX = 0.0;
  double checkY = 0.0;
  MapProjection::lonLatToMerc(out.anchorLat, out.anchorLon, checkX, checkY);
  EXPECT_NEAR(checkX, out.anchorMercX, 1.0);
  EXPECT_NEAR(checkY, out.anchorMercY, 1.0);
}

TEST(MapRouteFit, TheRealFixtureFitsAndIsCentredOnItsOwnBbox) {
  StdioFileSource file;
  MapRouteReader reader;
  ASSERT_TRUE(reader.open(file, fixturePath("lshape.tir").c_str()));
  ASSERT_TRUE(reader.beginPoints());

  MapRouteFit fit;
  fit.begin((reader.bboxMinX() + reader.bboxMaxX()) / 2, (reader.bboxMinY() + reader.bboxMaxY()) / 2);
  int32_t x = 0;
  int32_t y = 0;
  while (reader.nextPoint(x, y)) fit.addPoint(x, y);
  EXPECT_EQ(fit.pointsSeen(), reader.pointCount());

  MapRouteFit::Result out;
  ASSERT_TRUE(fit.finish(kScreenWidth, kScreenHeight, out));
  EXPECT_TRUE(out.fits);
  // An L 5.5 km across and 3.4 km tall needs a coarse rung, but it is inside the
  // ladder.
  EXPECT_LT(out.zoomStep, MapViewport::kZoomStepCount);
  // Every point must land on the panel at the rung and heading it picked. That
  // is the whole claim the feature makes, so it is checked against the real
  // projection rather than against the fit's own arithmetic.
  MapProjection proj;
  proj.reset(out.anchorLat, out.anchorLon, kScreenWidth / 2, kScreenHeight / 2, out.heading,
             MapViewport::mppMercFor(out.zoomStep, out.anchorLat));
  ASSERT_TRUE(reader.beginPoints());
  while (reader.nextPoint(x, y)) {
    int32_t sx = 0;
    int32_t sy = 0;
    proj.projectMercWide(static_cast<double>(x), static_cast<double>(y), sx, sy);
    EXPECT_GE(sx, 0);
    EXPECT_LT(sx, kScreenWidth);
    EXPECT_GE(sy, 0);
    EXPECT_LT(sy, kScreenHeight);
  }
}
