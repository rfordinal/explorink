// P2 gate 1 (docs/prototype-plan.md): render a fixed coordinate from a
// small committed tile set and compare against a committed PPM, byte-exact.
//
// The tile fixture (fixtures/tiny-sd) is one real .tib file copied from a
// mapbuilder build around Sisulakov mlyn/Sisolaky (48.5312N 17.0728E) --
// small enough to commit, real enough to exercise roads and places
// together. The anchor sits at the tile's own centre so the viewport at
// zoom step 0 needs only this one tile plus one (deliberately absent)
// neighbour, which doubles as coverage for the missing-tile path.
#include <gtest/gtest.h>

#include <fstream>
#include <vector>

#include "MapPreviewPipeline.h"
#include "MapTileReader.h"
#include "PpmCanvas.h"

namespace {

constexpr int kScreenWidth = 480;
constexpr int kScreenHeight = 800;

constexpr double kAnchorLat = 48.531158410819025;
constexpr double kAnchorLon = 17.072751469276742;

std::string fixturesDir() { return std::string(MAP_TILE_READER_FIXTURES_DIR); }

std::vector<uint8_t> readFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

}  // namespace

TEST(MapTileReaderGolden, RendersFixedViewportByteExact) {
  const MapPreviewResult preview =
      buildMapPreview(fixturesDir() + "/tiny-sd", kAnchorLat, kAnchorLon, /*heading=*/0, /*zoom=*/0);

  ASSERT_EQ(preview.tilesLoaded, 1);
  ASSERT_EQ(preview.tilesMissing, 1);  // the viewport's other touched tile is deliberately not in the fixture
  ASSERT_EQ(preview.state.ways.size(), 90u);
  ASSERT_EQ(preview.state.placeDots.size(), 2u);

  PpmCanvas canvas(kScreenWidth, kScreenHeight);
  MapRenderer::render(canvas, preview.state);

  const std::string outPath = testing::TempDir() + "map_tile_reader_golden_out.ppm";
  ASSERT_TRUE(canvas.writePpm(outPath));

  const std::vector<uint8_t> actual = readFile(outPath);
  const std::vector<uint8_t> golden = readFile(fixturesDir() + "/golden.ppm");

  ASSERT_FALSE(golden.empty()) << "missing golden.ppm fixture";
  EXPECT_EQ(actual, golden);
}

// Instruments the O(1)-in-tile-size claim directly: MapTileReader's stream
// buffer is a fixed constant regardless of which tile was read.
TEST(MapTileReaderGolden, ReaderBufferIsFlat) {
  EXPECT_EQ(MapTileReader::peakBufferBytes(), MapTileReader::kStreamBufferSize);
}
