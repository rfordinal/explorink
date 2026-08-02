#include "MapTileSource.h"

#include <cstdio>

MapTileSource::MapTileSource(IFileSource& file, const MapProjection& proj) : file_(file), proj_(proj) {
  name_[0] = '\0';
  path_[0] = '\0';
}

MapTileSource::~MapTileSource() { closeCurrentTile(); }

void MapTileSource::begin(const Config& config) {
  closeCurrentTile();
  config_ = config;
  rowSpan_ = config_.row1 >= config_.row0 ? (config_.row1 - config_.row0 + 1) : 0;
  const uint32_t colSpan = config_.col1 >= config_.col0 ? (config_.col1 - config_.col0 + 1) : 0;
  tileCount_ = colSpan * rowSpan_;
  nextTileIndex_ = 0;
  tilesOpened_ = 0;
  tilesUnavailable_ = 0;
  unavailableMask_ = 0;
  waysEmitted_ = 0;
  placesEmitted_ = 0;
}

void MapTileSource::buildPath(uint32_t col, uint32_t row) {
  std::snprintf(path_, sizeof(path_), "%s/base/%u/%u/%u.tib", config_.rootDir ? config_.rootDir : "",
                static_cast<unsigned>(config_.z), static_cast<unsigned>(col), static_cast<unsigned>(row));
}

void MapTileSource::closeCurrentTile() {
  if (tileOpen_) {
    reader_.close();
    tileOpen_ = false;
  }
}

bool MapTileSource::startPass(MapTileReader::Layer layer) {
  closeCurrentTile();
  layer_ = layer;
  nextTileIndex_ = 0;
  tilesOpened_ = 0;
  tilesUnavailable_ = 0;
  return tileCount_ > 0;
}

bool MapTileSource::advanceToNextTile() {
  closeCurrentTile();
  while (nextTileIndex_ < tileCount_) {
    // Column-major, matching the order the viewport's tile range is walked
    // everywhere else. Draw order across tiles is fixed and reproducible.
    const uint32_t index = nextTileIndex_++;
    const uint32_t col = config_.col0 + index / rowSpan_;
    const uint32_t row = config_.row0 + index % rowSpan_;
    buildPath(col, row);

    if (!reader_.open(file_, path_)) {
      // Absent, truncated or header-crc32-mismatched -- all of them mean
      // "no data here", which is a hatched area, never white or garbage
      // geometry.
      ++tilesUnavailable_;
      if (index < 32) unavailableMask_ |= (1u << index);
      continue;
    }

    if (!reader_.hasLayer(layer_)) {
      // Real, header-valid tile, just nothing in this layer. The layer
      // directory made skipping it free, and an empty layer is not a
      // reason to hatch the tile.
      ++tilesOpened_;
      reader_.close();
      continue;
    }

    if (!reader_.beginLayer(layer_)) {
      // Present per the directory, but its own crc32 failed. hasLayer()
      // already ruled out "absent" above, so this is corrupt data, not an
      // empty layer -- it must count as unavailable and hatch, the same as
      // a tile that failed to open at all.
      ++tilesUnavailable_;
      if (index < 32) unavailableMask_ |= (1u << index);
      reader_.close();
      continue;
    }

    ++tilesOpened_;
    tileOpen_ = true;
    return true;
  }
  return false;
}

bool MapTileSource::beginWays() { return startPass(MapTileReader::Layer::Roads); }

bool MapTileSource::nextWay(MapWayRef& out) {
  for (;;) {
    if (!tileOpen_ && !advanceToNextTile()) return false;

    MapTileReader::WayHeader header;
    if (!reader_.readWayHeader(header)) {
      // End of this tile's layer (or a short read at its tail) -- move on.
      closeCurrentTile();
      continue;
    }
    if (!reader_.readWayPoints(xs_, ys_, header.pointCount)) {
      // readWayPoints rejects a point count past kMaxWayPoints itself, so a
      // corrupt file cannot overrun xs_/ys_. The stream cursor is now
      // untrustworthy, so drop the rest of this tile rather than guess.
      closeCurrentTile();
      continue;
    }

    for (uint16_t i = 0; i < header.pointCount; ++i) {
      proj_.projectTileLocal(reader_.originX(), reader_.originY(), xs_[i], ys_[i], xs_[i], ys_[i]);
    }

    out.classId = header.classId;
    out.roughness = header.roughness;
    out.flags = header.flags;
    out.pointCount = header.pointCount;
    out.xs = xs_;
    out.ys = ys_;
    ++waysEmitted_;
    return true;
  }
}

bool MapTileSource::beginPlaces() { return startPass(MapTileReader::Layer::Places); }

bool MapTileSource::nextPlace(MapPlaceRef& out) {
  for (;;) {
    if (!tileOpen_ && !advanceToNextTile()) return false;

    MapTileReader::PlaceHeader header;
    if (!reader_.readPlaceHeader(header)) {
      closeCurrentTile();
      continue;
    }
    if (!reader_.readPlaceName(header, name_, sizeof(name_))) {
      closeCurrentTile();
      continue;
    }

    int16_t sx = 0;
    int16_t sy = 0;
    proj_.projectTileLocal(reader_.originX(), reader_.originY(), header.x, header.y, sx, sy);

    out.x = sx;
    out.y = sy;
    out.rank = header.rank;
    out.name = name_;
    ++placesEmitted_;
    return true;
  }
}
