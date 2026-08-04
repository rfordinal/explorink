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
#include "MapModeMask.h"
#include "MapPreviewPipeline.h"
#include "MapTileReader.h"
#include "MapTileSource.h"
#include "MapViewport.h"
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
  // Step 1, not 0: the fixture's tile-boundary-crossing viewport (one tile
  // present, its neighbour deliberately missing) was built around 3.0 m/px.
  // That value still lives on the ladder, just at a different index now.
  request.zoom = 1;
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

// --- P5: the mode filter and the marker-height ladder -------------------
//
// Both are render-time parameters over the same tiles. The fixture is one
// real tile, so "same bytes read, different ways drawn" is checkable here
// rather than only on the device.

TEST(MapModeFilter, HikeDrawsEverythingRideDoesAndMore) {
  // A property of the masks themselves, not of any one tile: hike is a
  // strict superset of ride, and cycle sits between them. This is what makes
  // "switch to hike and see more" true everywhere rather than only where
  // the test fixture happens to have a footpath.
  const MapModeMasks masks;
  const uint32_t ride = masks.forMode(MapRideMode::Ride);
  const uint32_t hike = masks.forMode(MapRideMode::Hike);
  const uint32_t cycle = masks.forMode(MapRideMode::Cycle);

  EXPECT_EQ(ride & hike, ride) << "hike must draw everything ride does";
  EXPECT_NE(ride, hike) << "and strictly more, or the mode switch is invisible";
  EXPECT_EQ(ride & cycle, ride);

  // The classes the phase is actually about: paths and footways are hike's,
  // and tracks are everyone's -- a forest track is the point of a trail
  // device, and what it is closed to is the no_motor flag's job, not the
  // class list's (docs/map-data-spec.md, "The class enum").
  EXPECT_FALSE(ride & mapClassBit(MapClassId::Path));
  EXPECT_FALSE(ride & mapClassBit(MapClassId::Footway));
  EXPECT_TRUE(hike & mapClassBit(MapClassId::Path));
  EXPECT_TRUE(hike & mapClassBit(MapClassId::Footway));
  EXPECT_TRUE(ride & mapClassBit(MapClassId::Track));
  EXPECT_TRUE(hike & mapClassBit(MapClassId::Track));
}

TEST(MapModeFilter, SameTilesSameBytesDifferentWaysDrawn) {
  // The committed fixture is one rural tile carrying only `service` and
  // `track`, so no two *modes* differ over it -- that difference is a device
  // gate. What is checkable here is the mechanism: drop one class from the
  // mask and the same tile reads identically and draws less.
  MapPreviewRequest withTracks = fixtureRequest();
  withTracks.classMask = MapModeMasks{}.forMode(MapRideMode::Ride);
  PpmCanvas withCanvas(kScreenWidth, kScreenHeight);
  const MapPreviewResult with = renderMapPreview(withTracks, withCanvas);

  MapPreviewRequest withoutTracks = withTracks;
  withoutTracks.classMask &= ~mapClassBit(MapClassId::Track);
  PpmCanvas withoutCanvas(kScreenWidth, kScreenHeight);
  const MapPreviewResult without = renderMapPreview(withoutTracks, withoutCanvas);

  EXPECT_GT(with.waysDrawn, without.waysDrawn);
  EXPECT_LT(with.waysFiltered, without.waysFiltered);
  EXPECT_NE(withCanvas.pixels(), withoutCanvas.pixels()) << "the filter must change the picture";

  // Nothing about the tile set changed -- same tiles, same records read off
  // the card, only a different subset drawn. This is the property that lets
  // one card serve all three modes (docs/map-data-spec.md).
  EXPECT_EQ(with.tilesLoaded, without.tilesLoaded);
  EXPECT_EQ(with.waysDrawn + with.waysFiltered, without.waysDrawn + without.waysFiltered);
}

TEST(MapModeFilter, AnEmptyMaskDrawsNoWaysButStillReadsTheTiles) {
  MapPreviewRequest request = fixtureRequest();
  request.classMask = 0;
  PpmCanvas canvas(kScreenWidth, kScreenHeight);
  const MapPreviewResult result = renderMapPreview(request, canvas);

  EXPECT_EQ(result.waysDrawn, 0u);
  EXPECT_GT(result.waysFiltered, 0u);
  EXPECT_GT(result.tilesLoaded, 0);
}

TEST(MapModeFilter, ClassNamesResolveToTheEnumTheTilesCarry) {
  uint8_t id = 0;
  ASSERT_TRUE(mapClassIdFromName("motorway", id));
  EXPECT_EQ(id, static_cast<uint8_t>(MapClassId::Motorway));
  ASSERT_TRUE(mapClassIdFromName("living_street", id));
  EXPECT_EQ(id, static_cast<uint8_t>(MapClassId::LivingStreet));
  ASSERT_TRUE(mapClassIdFromName("unknown", id));
  EXPECT_EQ(id, static_cast<uint8_t>(MapClassId::Unknown));

  // A reserved slot has no name, so it can never be switched on by one.
  EXPECT_FALSE(mapClassIdFromName("", id));
  EXPECT_FALSE(mapClassIdFromName("motorway_link", id));
  EXPECT_FALSE(mapClassIdFromName("21", id));
}

TEST(MapModeFilter, ModeNamesRoundTrip) {
  for (const MapRideMode mode : {MapRideMode::Ride, MapRideMode::Hike, MapRideMode::Cycle}) {
    MapRideMode parsed = MapRideMode::Ride;
    ASSERT_TRUE(mapRideModeFromName(mapRideModeName(mode), parsed)) << mapRideModeName(mode);
    EXPECT_EQ(parsed, mode);
  }
  MapRideMode unused = MapRideMode::Ride;
  EXPECT_FALSE(mapRideModeFromName("drive", unused));
}

TEST(MapMarkerLadder, EveryRungIsOnScreenAndMovesThePicture) {
  std::vector<uint8_t> previous;
  for (int step = 0; step < MapViewport::kMarkerStepCount; ++step) {
    const int16_t y = MapViewport::markerYForStep(step);
    // A rung that hides the puck is not a usable navigation state, whatever
    // the style file is allowed to express (docs/architecture-plan.md).
    EXPECT_GE(y, 0);
    EXPECT_LT(y, kScreenHeight);
    if (step > 0) EXPECT_GT(y, MapViewport::markerYForStep(step - 1)) << "the ladder must be monotonic";

    MapPreviewRequest request = fixtureRequest();
    request.markerY = y;
    PpmCanvas canvas(kScreenWidth, kScreenHeight);
    renderMapPreview(request, canvas);
    EXPECT_NE(canvas.pixels(), previous) << "step " << step << " drew the same picture as the one below it";
    previous = canvas.pixels();
  }

  // Out of range clamps rather than indexing off the end -- the step is a
  // byte out of a settings file a user can edit.
  EXPECT_EQ(MapViewport::markerYForStep(-1), MapViewport::kMarkerLadder[0]);
  EXPECT_EQ(MapViewport::markerYForStep(99), MapViewport::kMarkerLadder[MapViewport::kMarkerStepCount - 1]);
}

TEST(MapZoomLadder, EveryStepIsReachableAndMapsToOneLod) {
  for (int step = 0; step < MapViewport::kZoomStepCount; ++step) {
    const MapViewport::ZoomStep& rung = MapViewport::kZoomLadder[step];
    EXPECT_GT(rung.mpp, 0.0);
    EXPECT_GE(rung.z, 11);
    EXPECT_LE(rung.z, 13);
    if (step > 0) {
      EXPECT_GT(rung.mpp, MapViewport::kZoomLadder[step - 1].mpp) << "the ladder must be monotonic";
      // A step must never sit on an LOD boundary in a way that would make
      // two adjacent steps thrash between tile sets.
      EXPECT_GE(MapViewport::kZoomLadder[step - 1].z, rung.z);
    }
  }
  // Every mode starts on a rung that exists.
  for (uint8_t mode = 0; mode < kMapRideModeCount; ++mode) {
    EXPECT_LT(kDefaultZoomStepForMode[mode], MapViewport::kZoomStepCount);
    EXPECT_LT(kDefaultMarkerStepForMode[mode], MapViewport::kMarkerStepCount);
  }
}
