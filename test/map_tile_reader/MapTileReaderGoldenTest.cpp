// P2 gate 1 (docs/prototype-plan.md): render a fixed coordinate from a
// small committed tile set and compare against a committed PPM, byte-exact.
// P2.5 kept that PPM byte-identical while replacing the buffered pipeline
// with a streaming one -- the golden file is the whole safety net for that
// refactor, so it is never regenerated to make a test pass.
//
// The tile fixture (fixtures/tiny-sd) is one real .tib file copied from a
// mapbuilder build around Sisulakov mlyn/Sisolaky (48.5312N 17.0728E) --
// small enough to commit, real enough to exercise roads and places
// together. The anchor sits at the tile's own centre so the viewport at
// zoom step 0 needs only this one tile plus one (deliberately absent)
// neighbour, which doubles as coverage for the missing-tile path.
#include <gtest/gtest.h>

#include <cstring>
#include <fstream>
#include <vector>

#include "HeapProbe.h"
#include "MapPreviewPipeline.h"
#include "MapTileReader.h"
#include "MapTileSource.h"
#include "PpmCanvas.h"
#include "StdioFileSource.h"

namespace {

constexpr int kScreenWidth = 480;
constexpr int kScreenHeight = 800;

constexpr double kAnchorLat = 48.531158410819025;
constexpr double kAnchorLon = 17.072751469276742;

std::string fixturesDir() { return std::string(MAP_TILE_READER_FIXTURES_DIR); }

MapPreviewRequest fixtureRequest() {
  MapPreviewRequest request;
  request.tilesDir = fixturesDir() + "/tiny-sd";
  request.lat = kAnchorLat;
  request.lon = kAnchorLon;
  request.heading = 0;
  request.zoom = 0;
  return request;
}

std::vector<uint8_t> readFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

void writeFile(const std::string& path, const std::vector<uint8_t>& data) {
  std::ofstream f(path, std::ios::binary);
  f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

}  // namespace

TEST(MapTileReaderGolden, RendersFixedViewportByteExact) {
  PpmCanvas canvas(kScreenWidth, kScreenHeight);
  const MapPreviewResult preview = renderMapPreview(fixtureRequest(), canvas);

  ASSERT_EQ(preview.tilesLoaded, 1);
  ASSERT_EQ(preview.tilesMissing, 1);  // the viewport's other touched tile is deliberately not in the fixture
  ASSERT_EQ(preview.waysDrawn, 90u);
  ASSERT_EQ(preview.placesDrawn, 2u);

  const std::string outPath = testing::TempDir() + "map_tile_reader_golden_out.ppm";
  ASSERT_TRUE(canvas.writePpm(outPath));

  const std::vector<uint8_t> actual = readFile(outPath);
  const std::vector<uint8_t> golden = readFile(fixturesDir() + "/golden.ppm");

  ASSERT_FALSE(golden.empty()) << "missing golden.ppm fixture";
  EXPECT_EQ(actual, golden);
}

// The O(1) claim, measured rather than asserted about a constant. The old
// version of this test compared MapTileReader::peakBufferBytes() against
// kStreamBufferSize -- a constexpr function returning that same constant --
// so it passed by construction and would still have passed if the reader
// read whole files into a vector.
//
// This one watches real heap traffic (HeapProbe.h) across the render. The
// streaming path holds nothing per way, per place or per tile, so the
// answer is zero bytes and zero allocations. A vector anywhere in the
// geometry path fails it immediately.
TEST(MapTileReaderGolden, RenderAllocatesNothing) {
  PpmCanvas canvas(kScreenWidth, kScreenHeight);
  const MapPreviewResult preview = renderMapPreview(fixtureRequest(), canvas);

  EXPECT_EQ(preview.allocsDuringRender, 0u);
  EXPECT_EQ(preview.peakHeapDuringRender, 0u);

  // The resident half is the whole rest of the cost: one stream buffer, one
  // way's point scratch, one place name, one path. Fixed at compile time,
  // and small enough that a 3x3 viewport of dense tiles pays it once.
  EXPECT_LE(preview.sourceBytes, 8u * 1024u);
}

// Same instrument, across tiles of very different sizes: the number must not
// move with how much map is in the file. Rendering only the fixture tile
// versus rendering the fixture tile plus its missing neighbour exercises
// both the data path and the absent-tile path at the same fixed cost.
TEST(MapTileReaderGolden, PeakRamDoesNotMoveWithTileContent) {
  MapPreviewRequest single = fixtureRequest();
  single.singleTile = true;
  single.tileCol = 4484;
  single.tileRow = 2829;

  PpmCanvas canvasA(kScreenWidth, kScreenHeight);
  const MapPreviewResult oneTile = renderMapPreview(single, canvasA);
  ASSERT_EQ(oneTile.tilesLoaded, 1);
  ASSERT_EQ(oneTile.waysDrawn, 90u);

  PpmCanvas canvasB(kScreenWidth, kScreenHeight);
  const MapPreviewResult viewport = renderMapPreview(fixtureRequest(), canvasB);

  EXPECT_EQ(oneTile.sourceBytes, viewport.sourceBytes);
  EXPECT_EQ(oneTile.peakHeapDuringRender, viewport.peakHeapDuringRender);
}

// A corrupt point_count must not reach a caller's buffer. The cap lives in
// the reader, so this holds for every caller including the P4 device one.
TEST(MapTileReader, RejectsWayPointCountAboveCap) {
  int16_t xs[MapTileReader::kMaxWayPoints];
  int16_t ys[MapTileReader::kMaxWayPoints];
  MapTileReader reader;
  EXPECT_FALSE(reader.readWayPoints(xs, ys, MapTileReader::kMaxWayPoints + 1));
}

// Format version 2's crc split (docs/map-data-spec.md, "Tile file format"):
// magic, then version, then the header crc, then anything else. These three
// tests exercise that order directly, on a mutated copy of the same fixture
// tile the golden render above already proved is a valid v2 file.
TEST(MapTileReader, RejectsFormatVersionOne) {
  std::vector<uint8_t> data = readFile(fixturesDir() + "/tiny-sd/base/13/4484/2829.tib");
  ASSERT_GE(data.size(), 6u);
  const uint16_t v1 = 1;
  std::memcpy(&data[4], &v1, sizeof(v1));  // version, right after the 4-byte magic

  const std::string tmpPath = testing::TempDir() + "map_tile_reader_v1.tib";
  writeFile(tmpPath, data);

  StdioFileSource file;
  MapTileReader reader;
  EXPECT_FALSE(reader.open(file, tmpPath.c_str()))
      << "a version-1 file must be refused before its (differently-shaped) directory is ever parsed";
}

TEST(MapTileReader, RejectsHeaderCrcCorruption) {
  std::vector<uint8_t> data = readFile(fixturesDir() + "/tiny-sd/base/13/4484/2829.tib");
  ASSERT_GT(data.size(), 10u);
  data[10] ^= 0xFF;  // inside origin_x -- fixed header, well before the layer directory

  const std::string tmpPath = testing::TempDir() + "map_tile_reader_bad_header.tib";
  writeFile(tmpPath, data);

  StdioFileSource file;
  MapTileReader reader;
  EXPECT_FALSE(reader.open(file, tmpPath.c_str()));
}

// A flipped byte inside one layer's data must fail only that layer's crc32
// -- open() (header crc only) and every other layer must be unaffected.
// That is the property the whole per-layer split is for: a corrupt
// buildings layer, say, must not cost roads or places anything.
TEST(MapTileReader, RejectsLayerCrcCorruptionWithoutBreakingOtherLayers) {
  std::vector<uint8_t> data = readFile(fixturesDir() + "/tiny-sd/base/13/4484/2829.tib");
  ASSERT_GT(data.size(), 36u);

  const uint8_t layerCount = data[35];
  uint8_t corruptedLayerId = 0;
  for (uint8_t i = 0; i < layerCount; ++i) {
    const size_t entryOff = 36 + i * 13;
    ASSERT_GE(data.size(), entryOff + 13);
    const uint8_t id = data[entryOff];
    uint32_t offset = 0, length = 0;
    std::memcpy(&offset, &data[entryOff + 1], sizeof(offset));
    std::memcpy(&length, &data[entryOff + 5], sizeof(length));
    if (id != static_cast<uint8_t>(MapTileReader::Layer::Roads) && length > 0) {
      data[offset] ^= 0xFF;
      corruptedLayerId = id;
      break;
    }
  }
  ASSERT_NE(corruptedLayerId, 0) << "fixture tile has no non-roads layer to corrupt";

  const std::string tmpPath = testing::TempDir() + "map_tile_reader_bad_layer.tib";
  writeFile(tmpPath, data);

  StdioFileSource file;
  MapTileReader reader;
  ASSERT_TRUE(reader.open(file, tmpPath.c_str())) << "header crc32 must still be valid";
  EXPECT_FALSE(reader.beginLayer(static_cast<MapTileReader::Layer>(corruptedLayerId)));
  EXPECT_TRUE(reader.beginLayer(MapTileReader::Layer::Roads)) << "an intact layer must still open fine";
}
