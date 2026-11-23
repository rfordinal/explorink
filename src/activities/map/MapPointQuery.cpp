#include "MapPointQuery.h"

#include <cstring>

#include "MapProjection.h"
#include "PinGeo.h"

namespace {

// tan(22.5 deg) = 0.41421, as a 1/1024 fixed-point numerator. The 8-sector
// boundaries are |dx| vs 0.414*|dy| and |dy| vs 0.414*|dx|, so one constant
// covers both and no libm is needed (PinGeo makes the same argument for
// distance).
constexpr int64_t kTan22_5Num = 424;  // 0.41421 * 1024, rounded
constexpr int64_t kTan22_5Den = 1024;

// cos(latitude) as a 1/1024 scale, from the same table shape PinGeo uses for
// distance: a longitude degree shrinks towards the poles, and a bearing taken
// without that scale points too far east or west the further north the rider is.
int64_t cosScale1024(int32_t latE7) {
  int32_t absLat = latE7 < 0 ? -latE7 : latE7;
  if (absLat > 900000000) absLat = 900000000;
  // A coarse table is enough for a sector: 5-degree steps, linear between them.
  static constexpr int16_t kCos[19] = {1024, 1020, 1008, 987, 958, 920, 875, 823, 764, 700,
                                       630,  555,  476,  394, 309, 221, 132, 45,  0};
  const int32_t deg = absLat / 10000000;  // whole degrees
  const int32_t idx = deg / 5;
  const int32_t rem = deg % 5;
  if (idx >= 18) return kCos[18];
  const int32_t a = kCos[idx];
  const int32_t b = kCos[idx + 1];
  return a + (b - a) * rem / 5;
}

}  // namespace

MapPointQuery::MapPointQuery(IFileSource& file) : file_(file) {}

uint8_t MapPointQuery::sector8(int32_t fromLatE7, int32_t fromLonE7, int32_t toLatE7, int32_t toLonE7) {
  const int64_t dLat = static_cast<int64_t>(toLatE7) - fromLatE7;
  int64_t dLon = static_cast<int64_t>(toLonE7) - fromLonE7;
  // The short way round the antimeridian, same rule PinGeo::distanceM applies.
  if (dLon > 1800000000LL) dLon -= 3600000000LL;
  if (dLon < -1800000000LL) dLon += 3600000000LL;

  // East-west is scaled by cos(latitude); north-south is not.
  const int64_t east = dLon * cosScale1024((fromLatE7 + toLatE7) / 2) / 1024;
  const int64_t north = dLat;

  const int64_t absEast = east < 0 ? -east : east;
  const int64_t absNorth = north < 0 ? -north : north;

  // Inside 22.5 degrees of an axis it is that axis; otherwise it is the
  // diagonal between the two signs.
  const bool axisNorthSouth = absEast * kTan22_5Den <= absNorth * kTan22_5Num;
  const bool axisEastWest = absNorth * kTan22_5Den <= absEast * kTan22_5Num;

  if (axisNorthSouth) return north >= 0 ? 0 : 4;  // N or S
  if (axisEastWest) return east >= 0 ? 2 : 6;     // E or W
  if (north > 0) return east > 0 ? 1 : 7;         // NE or NW
  return east > 0 ? 3 : 5;                        // SE or SW
}

const char* MapPointQuery::sectorName(uint8_t sector) {
  static constexpr const char* kNames[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
  return kNames[sector & 0x07];
}

template <typename Visit>
bool MapPointQuery::walk(const Visit& visit, bool verify) {
  shardsOpened_ = 0;
  shardsMissing_ = 0;
  shardsCorrupt_ = 0;
  bytesRead_ = 0;
  if (config_.rootDir == nullptr) return false;

  double fixMercX = 0.0;
  double fixMercY = 0.0;
  MapProjection::lonLatToMerc(config_.fixLatE7 / 1e7, config_.fixLonE7 / 1e7, fixMercX, fixMercY);

  // The circle's bbox in Mercator metres. Mercator metres are longer than
  // ground metres away from the equator, so this over-covers rather than
  // under-covers -- the per-point distance test below is what actually bounds
  // the result, and an extra shard costs one read while a missing one loses a
  // hospital.
  const MapPointShards::Range range =
      MapPointShards::rangeForRadius(fixMercX, fixMercY, static_cast<double>(config_.radiusM));

  for (uint32_t col = range.col0; col <= range.col1; ++col) {
    for (uint32_t row = range.row0; row <= range.row1; ++row) {
      if (!MapPointShards::buildPath(path_, sizeof(path_), config_.rootDir, col, row)) continue;
      if (!reader_.open(file_, path_)) {
        ++shardsMissing_;  // sparse layer: most shards of a built area have none
        continue;
      }
      ++shardsOpened_;
      const bool wantedKind = (reader_.kindsPresent() & config_.kindMask) != 0;
      // verifyBody() is the second read of the file; see the header for why the
      // distance pass does without it.
      const bool trustworthy = wantedKind && (!verify || reader_.verifyBody());
      if (!trustworthy || !reader_.beginRecords()) {
        if (wantedKind && verify) ++shardsCorrupt_;
        bytesRead_ += reader_.bytesRead();
        reader_.close();
        continue;
      }

      MapPointReader::Record record;
      while (reader_.nextRecord(record)) {
        const uint8_t kindBit = static_cast<uint8_t>(record.kind);
        if (kindBit >= 8 || (config_.kindMask & (1u << kindBit)) == 0) continue;

        double lat = 0.0;
        double lon = 0.0;
        MapProjection::mercToLonLat(static_cast<double>(record.x), static_cast<double>(record.y), lat, lon);
        const int32_t latE7 = static_cast<int32_t>(lat * 1e7);
        const int32_t lonE7 = static_cast<int32_t>(lon * 1e7);
        const uint32_t metres = PinGeo::distanceM(config_.fixLatE7, config_.fixLonE7, latE7, lonE7);
        if (metres > config_.radiusM) continue;

        visit(record, latE7, lonE7, metres);
      }
      bytesRead_ += reader_.bytesRead();
      reader_.close();
    }
  }
  return true;
}

bool MapPointQuery::nearestPerCategory(uint32_t* out, size_t outCount) {
  if (out == nullptr || outCount == 0) return false;
  for (size_t i = 0; i < outCount; ++i) out[i] = kNoDistance;

  // No name is read in this pass. Ten numbers off 100 records is 1.6 kB of card
  // reads; fetching a name per record would multiply that by a seek each.
  return walk(
      [&](const MapPointReader::Record& record, int32_t, int32_t, uint32_t metres) {
        if (record.category >= outCount) return;
        if (metres < out[record.category]) out[record.category] = metres;
      },
      /*verify=*/false);
}

size_t MapPointQuery::listCategory(uint8_t category, Hit* out, size_t maxHits) {
  if (out == nullptr || maxHits == 0) return 0;
  const size_t cap = maxHits < kMaxHits ? maxHits : kMaxHits;
  size_t count = 0;

  // Insertion sort into the caller's array, capped: the array is the result and
  // there is no second buffer. A shard holds tens of points and the cap is
  // eight, so this is a handful of moves per hit and no allocation.
  walk(
      [&](const MapPointReader::Record& record, int32_t latE7, int32_t lonE7, uint32_t metres) {
        if (record.category != category) return;
        if (count == cap && metres >= out[count - 1].metres) return;

        size_t at = count < cap ? count : cap - 1;
        while (at > 0 && out[at - 1].metres > metres) {
          out[at] = out[at - 1];
          --at;
        }
        Hit& hit = out[at];
        hit = Hit{};
        hit.latE7 = latE7;
        hit.lonE7 = lonE7;
        hit.metres = metres;
        hit.category = record.category;
        hit.flags = record.flags;
        hit.sector = sector8(config_.fixLatE7, config_.fixLonE7, latE7, lonE7);
        // The one place a name is read, and only for a row that is being kept.
        reader_.readName(record, hit.name, sizeof(hit.name));
        if (count < cap) ++count;
      },
      /*verify=*/true);

  return count;
}
