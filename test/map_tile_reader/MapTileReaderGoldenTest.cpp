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

#include <algorithm>
#include <cstring>
#include <fstream>
#include <vector>

#include "HeapProbe.h"
#include "MapMarkerMetrics.h"
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

// The style the golden PPM was rendered with, frozen here rather than read
// from the compiled data/mapstyle.json. The fixture guards the tile pipeline,
// so an author widening a road class in the style file must not turn it red:
// that edit changes the picture on purpose, and this test is not what should
// notice.
//
// Every width is 1 px on purpose, and that is what keeps this fixture stable.
// A one-pixel line is just Bresenham -- no thick-line decomposition, no brush
// shape -- so changing how a wide line is built (MapStroke.h) cannot move a
// pixel here. No casings and no puck disc for the same reason: the PPM then
// depends only on the tile pipeline, the projection, and three primitives.
// Buildings and water are off here as well, for the same stability reason and
// one more: the fixture tile has no such layers, so drawing them would be
// exercising the empty-layer path, not a render.
constexpr MapStyle kGoldenStyle = {
    .roadWidthPx = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
    .roadCasingPx = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    .buildingsEnabled = false,
    .buildingOutlinePx = 0,
    .buildingTone = MapAreaTone::None,
    .buildingHatch = MapAreaFill::Pattern::None,
    .buildingHatchSpacingPx = 0,
    .waterEnabled = false,
    .waterLinePx = {0, 0, 0, 0},
    .waterTone = MapAreaTone::None,
    .waterHatch = MapAreaFill::Pattern::None,
    .waterHatchSpacingPx = 0,
    .landuseEnabled = false,
    .landuseOutlinePx = {0, 0, 0, 0},
    .landuseTone = {MapAreaTone::None, MapAreaTone::None, MapAreaTone::None, MapAreaTone::None},
    .landuseHatch = {MapAreaFill::Pattern::None, MapAreaFill::Pattern::None, MapAreaFill::Pattern::None,
                     MapAreaFill::Pattern::None},
    .landuseHatchSpacingPx = {0, 0, 0, 0},
    .placeDotDiameterPx = 10,
    // No route. This fixture guards the tile pipeline, and the golden PPM must
    // stay byte-identical -- a route drawn over it would change every frame it
    // crosses. The route layer has its own test (test/map_route).
    .routeWidthPx = 0,
    .routeArrowLenPx = 0,
    .routeArrowWidthPx = 0,
    .markerXPx = 230,
    .markerYPx = 620,
    .puckRadiusPx = 0,  // arrow only, no disc
    .puckRingPx = 0,
    .puckArrowPx = 28,
};

std::string fixturesDir() { return std::string(MAP_TILE_READER_FIXTURES_DIR); }

MapPreviewRequest fixtureRequest() {
  MapPreviewRequest request;
  request.tilesDir = fixturesDir() + "/tiny-sd";
  request.lat = kAnchorLat;
  request.lon = kAnchorLon;
  request.heading = 0;
  request.style = &kGoldenStyle;
  request.markerY = kGoldenStyle.markerYPx;
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
  // 43, not the 90 this fixture read before 2026-08-06: MapTileSource now drops a
  // way whose bounding box cannot reach the screen before projecting it
  // (Config::screenWidth). The golden PPM below is unchanged by that, which is
  // the whole claim -- 47 of this tile's ways were being projected, clipped and
  // drawn into nothing.
  ASSERT_EQ(preview.waysDrawn, 43u);
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
  ASSERT_EQ(oneTile.waysDrawn, 43u);  // see RendersFixedViewportByteExact: was 90 before the off-screen reject

  PpmCanvas canvasB(kScreenWidth, kScreenHeight);
  const MapPreviewResult viewport = renderMapPreview(fixtureRequest(), canvasB);

  EXPECT_EQ(oneTile.sourceBytes, viewport.sourceBytes);
  EXPECT_EQ(oneTile.peakHeapDuringRender, viewport.peakHeapDuringRender);
}

// The layer limit is the first thing that has to move when the builder gains a
// layer, and getting it wrong is invisible in the nicest possible way: open()
// rejects the tile, the source counts it unavailable, and the screen fills with
// hatch that looks exactly like "no map data here". That is what happened when
// landuse landed on 2026-08-05 against a reader still capped at 5.
TEST(MapTileReader, AcceptsEveryLayerTheBuilderWrites) {
  // Six layers today: water, buildings, roads, places, junctions, landuse
  // (docs/map-data-spec.md, "Layer ids").
  EXPECT_GE(MapTileReader::kMaxLayers, 6u);
  EXPECT_EQ(static_cast<int>(MapTileReader::Layer::Landuse), 6);
}

// content_id is the tile-freshness signal, and it only works if this reader and
// mapbuilder compute it identically -- the device sends its value, the phone
// compares it against the published index, and one bit of disagreement means
// either a tile that never updates or a card that re-downloads itself forever.
//
// These constants come from mapbuilder's content_id_from_layer_crcs() run over
// these exact fixture files:
//
//   python3 -c "import tile_reader; print(hex(tile_reader.read_tile_identity(
//       'test/map_tile_reader/fixtures/tiny-sd/base/13/4484/2829.tib')[5]))"
//
// Real bytes, not a synthetic vector, so the byte layout of the layer directory
// is under test too. If a fixture is ever regenerated these must be recomputed
// -- and a mismatch here is the intended way to find that out.
TEST(MapTileReader, ContentIdMatchesMapbuilder) {
  struct Case {
    const char* path;
    uint32_t contentId;
  };
  const Case cases[] = {
      {MAP_TILE_READER_FIXTURES_DIR "/tiny-sd/base/13/4484/2829.tib", 0x0EBD55C8u},
      {MAP_TILE_READER_FIXTURES_DIR "/indexed-sd/base/13/4484/2829.tib", 0xF7A3DE8Du},
  };

  for (const Case& c : cases) {
    StdioFileSource file;
    MapTileReader reader;
    ASSERT_TRUE(reader.open(file, c.path)) << c.path;
    EXPECT_EQ(reader.contentId(), c.contentId) << c.path << " -- this reader and mapbuilder disagree on content_id";
    reader.close();
  }

  // Two tiles for the same z/x/y whose layers differ must not share a
  // content_id. Both fixtures are 13/4484/2829; the indexed one carries more
  // geometry. If these ever matched, the signal would be blind to exactly the
  // change it exists to catch.
  EXPECT_NE(cases[0].contentId, cases[1].contentId);
}

// The fixture tile predates the landuse layer, so asking for it must be an
// empty pass rather than a failure: hasLayer() says no and the tile still counts
// as read. A tile without a layer is not a broken tile.
TEST(MapTileReaderGolden, AnAbsentLayerIsAnEmptyPassNotAFailure) {
  StdioFileSource file;
  MapTileReader reader;
  ASSERT_TRUE(reader.open(file, (fixturesDir() + "/tiny-sd/base/13/4484/2829.tib").c_str()));
  EXPECT_TRUE(reader.hasLayer(MapTileReader::Layer::Roads));
  EXPECT_FALSE(reader.hasLayer(MapTileReader::Layer::Landuse));
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
// Draws every way record out of the open layer until the stream runs dry, which
// is what makes the folded crc32 verdict available (MapTileReader::layerCheck).
// The renderer's own walk does exactly this -- MapTileSource::nextWayRecord loops
// until readWayHeader fails.
void drainWayLayer(MapTileReader& reader) {
  int16_t xs[MapTileReader::kMaxWayPoints];
  int16_t ys[MapTileReader::kMaxWayPoints];
  MapTileReader::WayHeader header;
  while (reader.readWayHeader(header)) {
    if (!reader.readWayPoints(xs, ys, header.pointCount)) break;
  }
}

TEST(MapTileReader, CatchesLayerCrcCorruptionOnceTheLayerHasBeenRead) {
  // Changed 2026-08-06: the sum is folded out of the record stream rather than
  // checked by a second pass first, so beginLayer() no longer refuses a corrupt
  // layer -- the verdict arrives after the records have been handed out. That is
  // the trade this test now pins down, and the reason MapTileSource re-renders
  // when it sees a Failed.
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

  const auto corrupted = static_cast<MapTileReader::Layer>(corruptedLayerId);
  ASSERT_TRUE(reader.beginLayer(corrupted)) << "the directory still says this layer is there";
  EXPECT_EQ(reader.layerCheck(), MapTileReader::LayerCheck::NotFinished) << "nothing is known before the end";
  drainWayLayer(reader);
  EXPECT_EQ(reader.layerCheck(), MapTileReader::LayerCheck::Failed) << "and the corruption must be caught at the end";

  ASSERT_TRUE(reader.beginLayer(MapTileReader::Layer::Roads)) << "an intact layer must still open fine";
  drainWayLayer(reader);
  EXPECT_EQ(reader.layerCheck(), MapTileReader::LayerCheck::Passed);
}

TEST(MapTileReader, SkipCrc32FoldsNothingAndSaysSo) {
  // The flag's contract: it is the caller stating this pair already passed in
  // this frame, so nothing is folded and no verdict is produced. Written as a
  // test because it is a loaded gun -- a caller that sets it wrongly gets a
  // corrupt layer with no complaint.
  std::vector<uint8_t> data = readFile(fixturesDir() + "/tiny-sd/base/13/4484/2829.tib");
  ASSERT_GT(data.size(), 36u);

  const uint8_t layerCount = data[35];
  uint8_t targetLayerId = 0;
  for (uint8_t i = 0; i < layerCount; ++i) {
    const size_t entryOff = 36 + i * 13;
    uint32_t offset = 0, length = 0;
    std::memcpy(&offset, &data[entryOff + 1], sizeof(offset));
    std::memcpy(&length, &data[entryOff + 5], sizeof(length));
    if (length > 0) {
      data[offset] ^= 0xFF;
      targetLayerId = data[entryOff];
      break;
    }
  }
  ASSERT_NE(targetLayerId, 0);

  const std::string tmpPath = testing::TempDir() + "map_tile_reader_skip_crc.tib";
  writeFile(tmpPath, data);

  StdioFileSource file;
  MapTileReader reader;
  ASSERT_TRUE(reader.open(file, tmpPath.c_str()));
  const auto layer = static_cast<MapTileReader::Layer>(targetLayerId);

  ASSERT_TRUE(reader.beginLayer(layer));
  drainWayLayer(reader);
  EXPECT_EQ(reader.layerCheck(), MapTileReader::LayerCheck::Failed) << "folded: the corruption is caught";

  ASSERT_TRUE(reader.beginLayer(layer, true));
  drainWayLayer(reader);
  EXPECT_EQ(reader.layerCheck(), MapTileReader::LayerCheck::Skipped) << "skipped: no sum, no verdict, no complaint";
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
  // Three outcomes per record, not two: drawn, dropped by the mode mask, or
  // dropped because it cannot reach the screen (MapTileSource::waysOffScreen,
  // added 2026-08-06). The mask runs first, so a way it drops never reaches the
  // screen test -- which is why the two-term version of this identity fails and
  // the three-term one holds.
  EXPECT_EQ(with.waysDrawn + with.waysFiltered + with.waysOffScreen,
            without.waysDrawn + without.waysFiltered + without.waysOffScreen);
}

TEST(MapOffScreenReject, DropsWhatCannotReachTheScreenAndNothingElse) {
  // Same fixture, same view, with and without the screen test. The picture must
  // be identical and the way count must not be: that pair is the whole claim of
  // docs/optimization/01-render-pipeline.md's step 3.
  MapPreviewRequest withReject = fixtureRequest();
  PpmCanvas withCanvas(kScreenWidth, kScreenHeight);
  const MapPreviewResult with = renderMapPreview(withReject, withCanvas);

  // screenWidth/Height 0 disables the test inside MapTileSource, which is the
  // "before" this step is measured against. rejectAll is not a mode -- there is
  // no way to ask for more geometry than the screen needs.
  MapPreviewRequest noReject = fixtureRequest();
  noReject.rejectOffScreen = false;
  PpmCanvas withoutCanvas(kScreenWidth, kScreenHeight);
  const MapPreviewResult without = renderMapPreview(noReject, withoutCanvas);

  EXPECT_EQ(withCanvas.pixels(), withoutCanvas.pixels()) << "the reject must not change one pixel";
  EXPECT_LT(with.waysDrawn, without.waysDrawn) << "and it must actually drop something on this fixture";
  EXPECT_GT(with.waysOffScreen, 0u);
  EXPECT_EQ(without.waysOffScreen, 0u);
  // Same bytes off the card either way: reading is what advances the record
  // stream, so a rejected way is still read in full. Only the projecting, the
  // clipping and the drawing are skipped.
  EXPECT_EQ(with.bytesRead, without.bytesRead);
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

TEST(MapCellIndex, AWindowReadsFewerBytesAndTheSameRecords) {
  // The indexed fixture is the same tile with an index forced onto its small
  // layers (mapbuilder/tiles.py, build_indexed_layer's min_bytes) -- a
  // realistically-sized one would be a 430 KB file in the repo.
  const std::string indexed = fixturesDir() + "/indexed-sd/base/13/4484/2829.tib";

  // Whole layer, for the reference set.
  StdioFileSource wholeFile;
  MapTileReader whole;
  ASSERT_TRUE(whole.open(wholeFile, indexed.c_str()));
  ASSERT_TRUE(whole.beginLayer(MapTileReader::Layer::Buildings));
  std::vector<std::string> allRecords;
  {
    int16_t xs[MapTileReader::kMaxWayPoints];
    int16_t ys[MapTileReader::kMaxWayPoints];
    MapTileReader::WayHeader header;
    while (whole.readWayHeader(header)) {
      if (!whole.readWayPoints(xs, ys, header.pointCount)) break;
      std::string key;
      for (uint16_t i = 0; i < header.pointCount; ++i) {
        key += std::to_string(xs[i]) + "," + std::to_string(ys[i]) + ";";
      }
      allRecords.push_back(key);
    }
  }
  EXPECT_EQ(whole.layerCheck(), MapTileReader::LayerCheck::Passed);
  ASSERT_FALSE(allRecords.empty());
  EXPECT_EQ(whole.cellsSkipped(), 0u) << "a whole-layer read skips nothing";

  // The whole window: every cell wanted. Must produce exactly the same records,
  // and skip nothing.
  StdioFileSource allCellsFile;
  MapTileReader allCells;
  ASSERT_TRUE(allCells.open(allCellsFile, indexed.c_str()));
  ASSERT_TRUE(allCells.beginLayerCells(MapTileReader::Layer::Buildings, 0, MapTileReader::kCellGrid - 1, 0,
                                       MapTileReader::kCellGrid - 1));
  std::vector<std::string> windowed;
  {
    int16_t xs[MapTileReader::kMaxWayPoints];
    int16_t ys[MapTileReader::kMaxWayPoints];
    MapTileReader::WayHeader header;
    while (allCells.readWayHeader(header)) {
      if (!allCells.readWayPoints(xs, ys, header.pointCount)) break;
      std::string key;
      for (uint16_t i = 0; i < header.pointCount; ++i) {
        key += std::to_string(xs[i]) + "," + std::to_string(ys[i]) + ";";
      }
      windowed.push_back(key);
    }
  }
  EXPECT_EQ(allCells.cellsSkipped(), 0u);
  std::sort(allRecords.begin(), allRecords.end());
  std::sort(windowed.begin(), windowed.end());
  EXPECT_EQ(windowed, allRecords) << "the full window must be the whole layer, record for record";

  // One cell only: fewer bytes, and every record it returns must be one the whole
  // layer had. This is the property the index lives or dies on -- it may return
  // less, it may never invent or corrupt.
  StdioFileSource oneCellFile;
  MapTileReader oneCell;
  ASSERT_TRUE(oneCell.open(oneCellFile, indexed.c_str()));
  ASSERT_TRUE(oneCell.beginLayerCells(MapTileReader::Layer::Buildings, 0, 0, 0, 0));
  size_t narrowCount = 0;
  {
    int16_t xs[MapTileReader::kMaxWayPoints];
    int16_t ys[MapTileReader::kMaxWayPoints];
    MapTileReader::WayHeader header;
    while (oneCell.readWayHeader(header)) {
      if (!oneCell.readWayPoints(xs, ys, header.pointCount)) break;
      std::string key;
      for (uint16_t i = 0; i < header.pointCount; ++i) {
        key += std::to_string(xs[i]) + "," + std::to_string(ys[i]) + ";";
      }
      EXPECT_NE(std::find(allRecords.begin(), allRecords.end(), key), allRecords.end())
          << "a windowed read returned a record the whole layer does not have";
      ++narrowCount;
    }
  }
  EXPECT_GT(oneCell.cellsSkipped(), 0u) << "one cell of 64 must skip the rest";
  EXPECT_GT(oneCell.bytesSkipped(), 0u);
  EXPECT_LT(narrowCount, allRecords.size()) << "and it must return fewer records than the whole layer";
}

TEST(MapCellIndex, AnUnindexedLayerFallsBackToTheWholeLayer) {
  // The committed fixture carries no index -- every layer is far under the
  // writer's threshold. Asking for one cell of it must still return everything,
  // because correctness cannot depend on whether the writer thought an index was
  // worth it.
  StdioFileSource file;
  MapTileReader reader;
  ASSERT_TRUE(reader.open(file, (fixturesDir() + "/tiny-sd/base/13/4484/2829.tib").c_str()));
  ASSERT_TRUE(reader.beginLayerCells(MapTileReader::Layer::Roads, 0, 0, 0, 0));
  drainWayLayer(reader);
  EXPECT_EQ(reader.layerCheck(), MapTileReader::LayerCheck::Passed) << "and the layer's own sum still checks";
  EXPECT_EQ(reader.cellsSkipped(), 0u);
  EXPECT_EQ(reader.bytesSkipped(), 0u);
}

TEST(MapBuildingsPerRung, OnlyTheClosestRungDrawsThemAndSkippingThemSkipsTheRead) {
  // The rule (MapViewport::ZoomStep::buildings): rung 0 draws buildings, no other
  // rung does. Decided by the maintainer 2026-08-06 -- at 3 m/px a building is
  // 7 px and the layer cost 4,122 ms of that rung's 7,463 on hardware.
  EXPECT_TRUE(MapViewport::kZoomLadder[0].buildings);
  for (int step = 1; step < MapViewport::kZoomStepCount; ++step) {
    EXPECT_FALSE(MapViewport::kZoomLadder[step].buildings) << "step " << step;
  }
  // And the mirror: built-up is the wash that replaces them, so exactly the rungs
  // without buildings have it. Every rung must draw one or the other, or a village
  // is roads in empty white -- which is what rung 1 looked like for one build.
  EXPECT_FALSE(MapViewport::kZoomLadder[0].builtUp);
  for (int step = 1; step < MapViewport::kZoomStepCount; ++step) {
    EXPECT_TRUE(MapViewport::kZoomLadder[step].builtUp) << "step " << step;
  }
  for (int step = 0; step < MapViewport::kZoomStepCount; ++step) {
    EXPECT_NE(MapViewport::kZoomLadder[step].buildings, MapViewport::kZoomLadder[step].builtUp)
        << "step " << step << " must draw buildings or the wash, never neither and never both";
  }

  // And the flag actually reaches the renderer. The golden style has buildings
  // off, so this needs one that has them on -- the fixture tile carries 30 of
  // them.
  MapStyle withBuildings = kGoldenStyle;
  withBuildings.buildingsEnabled = true;
  withBuildings.buildingHatch = MapAreaFill::Pattern::Cross;
  withBuildings.buildingHatchSpacingPx = 4;

  MapPreviewRequest on = fixtureRequest();
  on.style = &withBuildings;
  on.drawBuildings = true;
  PpmCanvas onCanvas(kScreenWidth, kScreenHeight);
  const MapPreviewResult drawn = renderMapPreview(on, onCanvas);

  MapPreviewRequest off = on;
  off.drawBuildings = false;
  PpmCanvas offCanvas(kScreenWidth, kScreenHeight);
  const MapPreviewResult skipped = renderMapPreview(off, offCanvas);

  EXPECT_NE(onCanvas.pixels(), offCanvas.pixels()) << "the flag must change the picture";
  // The point of gating the layer rather than the drawing: an unopened layer
  // costs no card read. That is where the milliseconds come from.
  EXPECT_LT(skipped.bytesRead, drawn.bytesRead) << "and it must skip the read, not just the draw";
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
  // Every mode starts on a rung that exists. The two ladders have been
  // different lengths since 2026-08-12, so each default is checked against its
  // own -- a marker default checked against the zoom count would pass while
  // indexing off the marker table.
  for (uint8_t mode = 0; mode < kMapRideModeCount; ++mode) {
    EXPECT_LT(kDefaultZoomStepForMode[mode], MapViewport::kZoomStepCount);
    EXPECT_LT(kDefaultMarkerStepForMode[mode], MapViewport::kMarkerStepCount);
  }
}

TEST(MapZoomLadder, CoarseRungsShrinkTheMarkerAndTightenTheMoveFloor) {
  // Rungs 5 and 6 exist for the regional view (docs/zoom-rungs.md). Two things
  // must hold down the whole ladder, both for the same reason -- a screen pixel
  // is worth more ground the further out the rung is:
  //
  //   the marker never grows as the ladder goes out (it covers more ground), and
  //   the move floor never grows either (a fix moves fewer pixels).
  for (int step = 1; step < MapViewport::kZoomStepCount; ++step) {
    const MapViewport::ZoomStep& rung = MapViewport::kZoomLadder[step];
    const MapViewport::ZoomStep& prev = MapViewport::kZoomLadder[step - 1];
    EXPECT_LE(rung.markerScale8, prev.markerScale8) << "step " << step;
    EXPECT_LE(rung.minMovePx, prev.minMovePx) << "step " << step;
    EXPECT_GT(rung.markerScale8, 0) << "step " << step;
    EXPECT_GT(rung.minMovePx, 0) << "step " << step;
    EXPECT_LE(rung.markerScale8, 8) << "step " << step << ": 8 is full size, not a scale to exceed";
  }
  // The coarse rungs are the ones that asked for this: at 45 m/px a full-size
  // 54 px ring covers 2.4 km of map.
  EXPECT_LT(MapViewport::kZoomLadder[MapViewport::kZoomStepCount - 1].markerScale8, 8);

  // And the ground a fix must cover before the panel is touched stays in the
  // same order of magnitude across the ladder, which is the point of making the
  // floor per rung rather than fixing the pixel count.
  for (int step = 0; step < MapViewport::kZoomStepCount; ++step) {
    const MapViewport::ZoomStep& rung = MapViewport::kZoomLadder[step];
    const double groundM = rung.mpp * rung.minMovePx;
    EXPECT_GE(groundM, 10.0) << "step " << step;
    EXPECT_LE(groundM, 150.0) << "step " << step;
  }
}

TEST(MapZoomLadder, MarkerMetricsScaleWithTheRungAndStayDrawable) {
  int previousRing = 1 << 30;
  for (int step = 0; step < MapViewport::kZoomStepCount; ++step) {
    const MarkerMetrics m = markerMetricsFor(MapViewport::kZoomLadder[step].markerScale8);
    // Nothing may round away to nothing: a 0 px stroke or half-width draws no
    // marker at all, which is the one state the map screen must never reach.
    EXPECT_GT(m.ring, 0) << "step " << step;
    EXPECT_GE(m.ringWidth, 2) << "step " << step;
    EXPECT_GT(m.hikeDot, 0) << "step " << step;
    EXPECT_GT(m.hikeHandReach, 0) << "step " << step;
    EXPECT_GT(m.rideTipLen, 0) << "step " << step;
    EXPECT_GT(m.cycleTipLen, 0) << "step " << step;
    // Everything the marker draws stays inside the box the patch save/restore
    // uses, or a move smears a trail across the map. The same condition the
    // header static_asserts; here it is checked as a value, not a compile.
    EXPECT_LE(m.hikeHandReach + m.hikeHandHalfW, m.box / 2) << "step " << step;
    EXPECT_LE(m.rideTipLen, m.box / 2) << "step " << step;
    EXPECT_LE(m.box, kMarkerMetricsFull.box) << "step " << step << ": the patch buffer is sized for the full marker";
    EXPECT_LE(m.ring, previousRing) << "step " << step;
    previousRing = m.ring;
  }
  EXPECT_EQ(markerMetricsFor(8).ring, 54) << "rung 0 must still draw exactly the marker it always drew";
  EXPECT_EQ(markerMetricsFor(8).box, 64);
}

TEST(MapZoomLadder, ZoomStepAtClampsLikeMarkerYForStep) {
  // A step arrives as a persisted byte or off a console command, so both
  // accessors have to survive nonsense rather than index the table with it.
  EXPECT_EQ(MapViewport::zoomStepAt(-1).mpp, MapViewport::kZoomLadder[0].mpp);
  EXPECT_EQ(MapViewport::zoomStepAt(99).mpp, MapViewport::kZoomLadder[MapViewport::kZoomStepCount - 1].mpp);
  for (int step = 0; step < MapViewport::kZoomStepCount; ++step) {
    EXPECT_EQ(MapViewport::zoomStepAt(step).mpp, MapViewport::kZoomLadder[step].mpp) << "step " << step;
  }
}
