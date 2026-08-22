// Reads a .tip shard written by mapbuilder/tilegen/point_file.py and checks
// every field back, then checks what the reader refuses.
//
// The fixture is committed, produced by that writer. That is the point: the
// format has two implementations in two languages, and a fixture written by one
// and read by the other is the only thing that keeps them agreeing
// (../../../docs/point-file-spec.md in the parent xteink repo). The numbers
// below are the writer's own output, printed when the fixture was made.
//
// Six points in one z10 shard (561/353): two water (one unverified), a hut with
// seasonal+fee, an unnamed hospital, a restricted pharmacy and a transport
// point. Chosen so the flag mask, the name pool and the sort order all have
// something to prove.

#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "MapPointQuery.h"
#include "MapPointReader.h"
#include "MapPointShards.h"
#include "MapPointTypes.h"
#include "StdioFileSource.h"

namespace {

std::string fixturePath(const char* name) { return std::string(MAP_POINTS_FIXTURES_DIR) + "/" + name; }

struct Expected {
  int32_t x;
  int32_t y;
  uint8_t kind;
  uint8_t category;
  uint8_t flags;
  const char* name;
};

// point_file.py's own output, in the order it writes: sorted by
// (kind, category, x, y), which is why pharmacy (7) comes before transport (10)
// and the unnamed hospital sits between the huts and the pharmacy.
const std::vector<Expected> kExpected = {
    {1925827, 6207260, 1, 1, 0x01, "Spring"},
    {1926940, 6208944, 1, 1, 0x00, "Drinking water"},
    {1928054, 6210628, 1, 3, 0x0c, "Chata Vrátna"},
    {1924714, 6205577, 1, 6, 0x00, ""},
    {1926384, 6209786, 1, 7, 0x02, "Lekáreň"},
    {1925271, 6208102, 1, 10, 0x00, "Sološnica, obec"},
};

constexpr uint32_t kBuildEpoch = 1755800000;
constexpr size_t kFixtureBytes = 202;

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

// Writes `bytes` to a temp file so a corruption test never touches the
// committed fixture.
std::string writeTemp(const std::vector<uint8_t>& bytes, const char* name) {
  const std::string path = std::string(MAP_POINTS_TEMP_DIR) + "/" + name;
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return "";
  std::fwrite(bytes.data(), 1, bytes.size(), f);
  std::fclose(f);
  return path;
}

bool opens(const std::vector<uint8_t>& bytes, const char* name) {
  const std::string path = writeTemp(bytes, name);
  StdioFileSource file;
  MapPointReader reader;
  const bool ok = reader.open(file, path.c_str());
  if (ok) reader.close();
  return ok;
}

}  // namespace

TEST(MapPointReader, ReadsTheHeaderTheWriterWrote) {
  StdioFileSource file;
  MapPointReader reader;
  ASSERT_TRUE(reader.open(file, fixturePath("shard.tip").c_str()));

  EXPECT_EQ(reader.pointCount(), kExpected.size());
  EXPECT_EQ(reader.buildEpoch(), kBuildEpoch);
  // Safety only: the fixture carries no landmark, so bit 2 must be clear. That
  // is what lets a safety-only walk skip a landmark-only shard unread.
  EXPECT_EQ(reader.kindsPresent(), 1u << static_cast<uint8_t>(MapPointKind::Safety));

  int32_t minX = kExpected[0].x;
  int32_t minY = kExpected[0].y;
  int32_t maxX = minX;
  int32_t maxY = minY;
  for (const Expected& e : kExpected) {
    minX = std::min(minX, e.x);
    minY = std::min(minY, e.y);
    maxX = std::max(maxX, e.x);
    maxY = std::max(maxY, e.y);
  }
  EXPECT_EQ(reader.bboxMinX(), minX);
  EXPECT_EQ(reader.bboxMinY(), minY);
  EXPECT_EQ(reader.bboxMaxX(), maxX);
  EXPECT_EQ(reader.bboxMaxY(), maxY);
}

TEST(MapPointReader, ReadsEveryRecordAndName) {
  StdioFileSource file;
  MapPointReader reader;
  ASSERT_TRUE(reader.open(file, fixturePath("shard.tip").c_str()));
  ASSERT_TRUE(reader.verifyBody());
  ASSERT_TRUE(reader.beginRecords());

  for (size_t i = 0; i < kExpected.size(); ++i) {
    MapPointReader::Record record;
    ASSERT_TRUE(reader.nextRecord(record)) << "record " << i;
    const Expected& e = kExpected[i];
    EXPECT_EQ(record.x, e.x) << "record " << i;
    EXPECT_EQ(record.y, e.y) << "record " << i;
    EXPECT_EQ(static_cast<uint8_t>(record.kind), e.kind) << "record " << i;
    EXPECT_EQ(record.category, e.category) << "record " << i;
    EXPECT_EQ(record.flags, e.flags) << "record " << i;

    // readName() seeks into the pool and back, so the walk has to survive it --
    // this loop is exactly what the Nearby list does per printed row.
    char name[MapPointReader::kMaxNameBytes + 1] = {};
    ASSERT_TRUE(reader.readName(record, name, sizeof(name))) << "record " << i;
    EXPECT_STREQ(name, e.name) << "record " << i;
  }

  MapPointReader::Record past;
  EXPECT_FALSE(reader.nextRecord(past));
}

TEST(MapPointReader, YGrowsNorth) {
  // Mixing Mercator y with the tile format's tile-local y (which grows south)
  // mirrors every point about its own latitude. The writer asserts this too.
  StdioFileSource file;
  MapPointReader reader;
  ASSERT_TRUE(reader.open(file, fixturePath("shard.tip").c_str()));
  ASSERT_TRUE(reader.beginRecords());

  // The hut is the northernmost point in the fixture and the hospital the
  // southernmost, by latitude.
  int32_t hutY = 0;
  int32_t hospitalY = 0;
  MapPointReader::Record record;
  while (reader.nextRecord(record)) {
    if (record.category == static_cast<uint8_t>(MapSafetyCategory::Hut)) hutY = record.y;
    if (record.category == static_cast<uint8_t>(MapSafetyCategory::Hospital)) hospitalY = record.y;
  }
  EXPECT_GT(hutY, hospitalY);
}

TEST(MapPointReader, TheFlagMaskIsTheFourConditions) {
  // unverified, restricted, seasonal, fee draw the corner flag. not_potable
  // does not -- such a point is dropped by the writer, so a reader must never
  // see one under water (../../../docs/point-file-spec.md).
  EXPECT_EQ(kPointFlaggedOnMapMask, kPointUnverified | kPointRestricted | kPointSeasonal | kPointFee);
  EXPECT_EQ(kPointFlaggedOnMapMask & kPointNotPotable, 0);

  StdioFileSource file;
  MapPointReader reader;
  ASSERT_TRUE(reader.open(file, fixturePath("shard.tip").c_str()));
  ASSERT_TRUE(reader.beginRecords());

  int flagged = 0;
  MapPointReader::Record record;
  while (reader.nextRecord(record)) {
    if ((record.flags & kPointFlaggedOnMapMask) != 0) ++flagged;
    EXPECT_EQ(record.flags & kPointNotPotable, 0) << "a not_potable point reached the file";
  }
  // Spring (unverified), the hut (seasonal+fee) and the pharmacy (restricted).
  EXPECT_EQ(flagged, 3);
}

TEST(MapPointReader, RefusesACorruptShard) {
  const std::vector<uint8_t> good = readFile(fixturePath("shard.tip"));
  ASSERT_EQ(good.size(), kFixtureBytes);
  ASSERT_TRUE(opens(good, "good.tip"));

  auto mutated = [&good](size_t offset, uint8_t value) {
    std::vector<uint8_t> bytes = good;
    bytes[offset] = value;
    return bytes;
  };

  EXPECT_FALSE(opens(mutated(3, '0'), "magic.tip")) << "bad magic";
  EXPECT_FALSE(opens(mutated(4, 2), "version.tip")) << "a version-2 file must be refused whole";
  EXPECT_FALSE(opens(mutated(7, 1), "reserved.tip")) << "reserved byte set";
  EXPECT_FALSE(opens(mutated(40, 1), "pad.tip")) << "header pad is not zero";
  EXPECT_FALSE(opens(mutated(12, 0xFF), "bbox.tip")) << "header crc covers the bbox";

  std::vector<uint8_t> shortFile = good;
  shortFile.resize(MapPointReader::kHeaderBytes - 1);
  EXPECT_FALSE(opens(shortFile, "short.tip"));

  // A flipped body byte passes the header check and has to fail verifyBody() --
  // the split is the whole reason there are two checksums.
  std::vector<uint8_t> bodyFlip = good;
  bodyFlip[MapPointReader::kHeaderBytes + 4] ^= 0xFF;
  const std::string path = writeTemp(bodyFlip, "body.tip");
  StdioFileSource file;
  MapPointReader reader;
  ASSERT_TRUE(reader.open(file, path.c_str())) << "the header is still intact";
  EXPECT_FALSE(reader.verifyBody());
}

TEST(MapPointShardGrid, ShardHoldsItsOwnPointsAndTheRadiusFitsThreeAcross) {
  // The fixture's own shard, from point_file.py: 561/353 at z10.
  const MapPointShards::Range one =
      MapPointShards::rangeForMercBbox(kExpected[0].x, kExpected[0].y, kExpected[0].x, kExpected[0].y);
  EXPECT_EQ(one.col0, 561u);
  EXPECT_EQ(one.row0, 353u);
  EXPECT_EQ(one.count(), 1u);

  // Why z10 exists: a 25 km radius search opens 3x3 files worst case. At z11 it
  // would be 5x5, which is the read storm the grid was chosen to avoid.
  const MapPointShards::Range radius = MapPointShards::rangeForRadius(
      kExpected[0].x, kExpected[0].y, MapPointShards::kSearchRadiusM);
  EXPECT_LE(radius.col1 - radius.col0 + 1, 3u);
  EXPECT_LE(radius.row1 - radius.row0 + 1, 3u);

  char path[160] = {};
  ASSERT_TRUE(MapPointShards::buildPath(path, sizeof(path), "/trailink", 561, 353));
  EXPECT_STREQ(path, "/trailink/points/10/561/353.tip");
}

// --- the radius search behind the Nearby menu -------------------------------
//
// The fixture doubles as a shard tree: fixtures/points/10/561/353.tip is the
// same bytes, at the path MapPointShards::buildPath() produces, so a query can
// run against MAP_POINTS_FIXTURES_DIR as its root.

namespace {

// The spring's own coordinate. Distances below are from here, computed with the
// same equirectangular formula PinGeo uses, to +/- a metre.
constexpr int32_t kFixLatE7 = 486000000;
constexpr int32_t kFixLonE7 = 173000000;

MapPointQuery::Config queryConfig() {
  MapPointQuery::Config config;
  config.rootDir = MAP_POINTS_FIXTURES_DIR;
  config.fixLatE7 = kFixLatE7;
  config.fixLonE7 = kFixLonE7;
  return config;
}

}  // namespace

TEST(MapPointQuery, NearestPerCategoryAnswersEveryRowIncludingTheEmptyOnes) {
  StdioFileSource file;
  MapPointQuery query(file);
  query.begin(queryConfig());

  uint32_t nearest[kSafetyCategoryCount] = {};
  ASSERT_TRUE(query.nearestPerCategory(nearest, kSafetyCategoryCount));

  // The rider is standing on the spring, so water is 0 m -- which is why
  // kNoDistance is a sentinel out of range and not 0.
  EXPECT_EQ(nearest[static_cast<uint8_t>(MapSafetyCategory::Water)], 0u);
  EXPECT_NEAR(nearest[static_cast<uint8_t>(MapSafetyCategory::Transport)], 667u, 12u);
  EXPECT_NEAR(nearest[static_cast<uint8_t>(MapSafetyCategory::Hospital)], 1333u, 20u);
  EXPECT_NEAR(nearest[static_cast<uint8_t>(MapSafetyCategory::Pharmacy)], 1708u, 25u);
  EXPECT_NEAR(nearest[static_cast<uint8_t>(MapSafetyCategory::Hut)], 2666u, 40u);

  // A category with nothing in range is kNoDistance, never absent: the menu
  // prints "None within 25 km" as a row, because a missing row reads as zero
  // distance or as a bug (docs/safety-concept.md, "Nearby").
  EXPECT_EQ(nearest[static_cast<uint8_t>(MapSafetyCategory::Fuel)], MapPointQuery::kNoDistance);
  EXPECT_EQ(nearest[static_cast<uint8_t>(MapSafetyCategory::Rescue)], MapPointQuery::kNoDistance);

  // Nine shards is the z10 worst case for a 25 km radius, and this fixture tree
  // holds one of them.
  EXPECT_EQ(query.shardsOpened(), 1u);
  EXPECT_LE(query.shardsOpened() + query.shardsMissing(), 9u);
  EXPECT_EQ(query.shardsCorrupt(), 0u);
}

TEST(MapPointQuery, ListsOneCategoryNearestFirstWithNamesAndSectors) {
  StdioFileSource file;
  MapPointQuery query(file);
  query.begin(queryConfig());

  MapPointQuery::Hit hits[MapPointQuery::kMaxHits];
  const size_t count = query.listCategory(static_cast<uint8_t>(MapSafetyCategory::Water), hits,
                                          MapPointQuery::kMaxHits);
  ASSERT_EQ(count, 2u);

  // Nearest first, and no clever reordering: the unverified spring at 0 m stays
  // above the confirmed tap at 1.3 km (docs/safety-concept.md, "Honesty rules").
  EXPECT_STREQ(hits[0].name, "Spring");
  EXPECT_EQ(hits[0].metres, 0u);
  EXPECT_EQ(hits[0].flags & kPointFlaggedOnMapMask, kPointUnverified);
  EXPECT_STREQ(hits[1].name, "Drinking water");
  EXPECT_NEAR(hits[1].metres, 1333u, 20u);
  EXPECT_EQ(hits[1].flags, 0u);
  // North-east of the fix: the tap is at a higher latitude and a higher
  // longitude.
  EXPECT_STREQ(MapPointQuery::sectorName(hits[1].sector), "NE");

  // A category the fixture has none of fills nothing and says so by count.
  EXPECT_EQ(query.listCategory(static_cast<uint8_t>(MapSafetyCategory::Fuel), hits, MapPointQuery::kMaxHits), 0u);
}

TEST(MapPointQuery, SectorsAreEightAndNeverDegrees) {
  const int32_t lat = 486000000;
  const int32_t lon = 173000000;
  const int32_t step = 100000;  // 0.01 degrees

  EXPECT_STREQ(MapPointQuery::sectorName(MapPointQuery::sector8(lat, lon, lat + step, lon)), "N");
  EXPECT_STREQ(MapPointQuery::sectorName(MapPointQuery::sector8(lat, lon, lat - step, lon)), "S");
  EXPECT_STREQ(MapPointQuery::sectorName(MapPointQuery::sector8(lat, lon, lat, lon + step)), "E");
  EXPECT_STREQ(MapPointQuery::sectorName(MapPointQuery::sector8(lat, lon, lat, lon - step)), "W");
  // The diagonals have to account for cos(latitude): at 48.6 degrees a degree of
  // longitude is two thirds of a degree of latitude on the ground, so equal
  // degree steps are NOT 45 degrees. The one that is scaled to be equal on the
  // ground must come out NE.
  const int32_t lonStepEqualGround = static_cast<int32_t>(step * 1.51);
  EXPECT_STREQ(MapPointQuery::sectorName(MapPointQuery::sector8(lat, lon, lat + step, lon + lonStepEqualGround)),
               "NE");
  EXPECT_STREQ(MapPointQuery::sectorName(MapPointQuery::sector8(lat, lon, lat - step, lon - lonStepEqualGround)),
               "SW");
}
