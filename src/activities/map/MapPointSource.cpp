#include "MapPointSource.h"

MapPointSource::MapPointSource(IFileSource& file, const MapProjection& proj) : file_(file), proj_(proj) {}

void MapPointSource::begin(const Config& config) {
  if (shardOpen_) {
    reader_.close();
    shardOpen_ = false;
  }
  config_ = config;
}

bool MapPointSource::beginMapPoints() {
  if (shardOpen_) {
    reader_.close();
    shardOpen_ = false;
  }
  pointsEmitted_ = 0;
  pointsDropped_ = 0;
  shardsOpened_ = 0;
  shardsMissing_ = 0;
  shardsCorrupt_ = 0;
  bytesRead_ = 0;
  shardIndex_ = 0;

  if (config_.rootDir == nullptr) return false;
  if (config_.kindMask == 0 || config_.categoryMask == 0) return false;
  if (config_.range.col1 < config_.range.col0 || config_.range.row1 < config_.range.row0) return false;
  shardCount_ = config_.range.count();
  if (shardCount_ == 0) return false;

  // Opening the first shard here rather than on the first nextMapPoint() so a
  // caller can treat false as "nothing to draw" and skip the walk entirely --
  // the same contract IMapSource::beginWays() has.
  return openNextShard();
}

bool MapPointSource::openNextShard() {
  const uint32_t cols = config_.range.col1 - config_.range.col0 + 1;
  while (shardIndex_ < shardCount_) {
    const uint32_t col = config_.range.col0 + (shardIndex_ % cols);
    const uint32_t row = config_.range.row0 + (shardIndex_ / cols);
    ++shardIndex_;

    if (!MapPointShards::buildPath(path_, sizeof(path_), config_.rootDir, col, row)) continue;
    if (!reader_.open(file_, path_)) {
      // No file is the normal case: points are sparse, and most shards of a
      // built area carry none. Not an error and not a hatch -- an empty shard
      // is deleted rather than written (../../../docs/point-file-spec.md).
      ++shardsMissing_;
      continue;
    }
    ++shardsOpened_;

    // A shard holding no kind this walk wants costs one header read and no
    // records.
    if ((reader_.kindsPresent() & config_.kindMask) == 0) {
      bytesRead_ += reader_.bytesRead();
      reader_.close();
      continue;
    }

    // Checked before a single record is trusted. A corrupt shard is skipped
    // whole: one bad file must not hide the eight good ones around it, and a
    // half-read shard would put a hospital somewhere there is none.
    if (!reader_.verifyBody() || !reader_.beginRecords()) {
      ++shardsCorrupt_;
      bytesRead_ += reader_.bytesRead();
      reader_.close();
      continue;
    }

    shardOpen_ = true;
    return true;
  }
  return false;
}

bool MapPointSource::wanted(const MapPointReader::Record& record) const {
  const uint8_t kindBit = static_cast<uint8_t>(record.kind);
  if (kindBit >= 8) return false;
  if ((config_.kindMask & (1u << kindBit)) == 0) return false;
  if (record.category >= 16) return false;
  return (config_.categoryMask & (1u << record.category)) != 0;
}

bool MapPointSource::nextMapPoint(MapPointRef& out) {
  while (shardOpen_) {
    MapPointReader::Record record;
    if (!reader_.nextRecord(record)) {
      bytesRead_ += reader_.bytesRead();
      reader_.close();
      shardOpen_ = false;
      if (!openNextShard()) return false;
      continue;
    }

    if (!wanted(record)) {
      ++pointsDropped_;
      continue;
    }

    int32_t screenX = 0;
    int32_t screenY = 0;
    proj_.projectMercWide(static_cast<double>(record.x), static_cast<double>(record.y), screenX, screenY);

    // Off-screen rejection after projecting and before emitting: a mark is
    // drawn centred on its point, so the margin is the mark's own reach. The
    // shard range is far larger than the panel (a 39 km shard against a 480x800
    // px screen), so most points in range never draw anything.
    if (config_.screenWidth > 0 && config_.screenHeight > 0) {
      const int32_t margin = config_.rejectMarginPx;
      if (screenX < -margin || screenY < -margin || screenX > config_.screenWidth + margin ||
          screenY > config_.screenHeight + margin) {
        ++pointsDropped_;
        continue;
      }
    }

    out.x = screenX;
    out.y = screenY;
    out.kind = record.kind;
    out.category = record.category;
    out.flags = record.flags;
    ++pointsEmitted_;
    return true;
  }
  return false;
}
