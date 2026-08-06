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
  waysFiltered_ = 0;
  placesEmitted_ = 0;
  bytesRead_ = 0;
  pointsProjected_ = 0;
  waysOffScreen_ = 0;
}

void MapTileSource::buildPath(uint32_t col, uint32_t row) {
  std::snprintf(path_, sizeof(path_), "%s/base/%u/%u/%u.tib", config_.rootDir ? config_.rootDir : "",
                static_cast<unsigned>(config_.z), static_cast<unsigned>(col), static_cast<unsigned>(row));
}

void MapTileSource::closeCurrentTile() {
  // takeBytesRead(), not bytesRead(): this can run more than once between
  // one open() and the next (once when a tile's layer runs dry inside
  // nextWay()/nextPlace(), again from advanceToNextTile()'s own
  // top-of-function call on the very next iteration) and taking zeroes the
  // count, so a repeat call banks zero instead of the same bytes twice.
  // Safe to call unconditionally otherwise: a failed open() still spent
  // real bytes on the header attempt and those must be counted too, and a
  // reader that never opened anything simply reports zero.
  bytesRead_ += reader_.takeBytesRead();
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
      // geometry. open() already closed the reader on failure, so this only
      // needs to bank whatever the failed attempt cost (a short or missing
      // header read).
      closeCurrentTile();
      ++tilesUnavailable_;
      if (index < 32) unavailableMask_ |= (1u << index);
      continue;
    }

    if (!reader_.hasAnyGeometry()) {
      // Valid tile, nothing in any layer -- a data hole wearing a tile's
      // clothes. Counted as unavailable so it hatches and reaches
      // MissingTilesStore, because "no data here" is what it actually is;
      // drawing it white would say "empty countryside" and nothing downstream
      // would ever know to ask for it (MapTileReader::hasAnyGeometry).
      //
      // The trade this accepts: a tile that is genuinely empty -- mid-lake,
      // unmapped forest -- now hatches too. That is the safer of the two wrong
      // answers, and mapbuilder no longer writes empty tiles at all, so the
      // case this fires on is an older card.
      closeCurrentTile();
      ++tilesUnavailable_;
      if (index < 32) unavailableMask_ |= (1u << index);
      continue;
    }

    if (!reader_.hasLayer(layer_)) {
      // Real, header-valid tile, just nothing in this layer. The layer
      // directory made skipping it free, and an empty layer is not a
      // reason to hatch the tile.
      ++tilesOpened_;
      bytesRead_ += reader_.takeBytesRead();
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
      bytesRead_ += reader_.takeBytesRead();
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

bool MapTileSource::nextWay(MapWayRef& out) { return nextWayRecord(out, true); }

bool MapTileSource::beginBuildings() { return startPass(MapTileReader::Layer::Buildings); }

bool MapTileSource::nextBuilding(MapWayRef& out) { return nextWayRecord(out, false); }

bool MapTileSource::beginWater() { return startPass(MapTileReader::Layer::Water); }

bool MapTileSource::nextWater(MapWayRef& out) { return nextWayRecord(out, false); }

bool MapTileSource::beginLanduse() { return startPass(MapTileReader::Layer::Landuse); }

bool MapTileSource::nextLanduse(MapWayRef& out) { return nextWayRecord(out, false); }

bool MapTileSource::mayReachScreen(const uint16_t pointCount) const {
  // No screen configured: nothing to reject against, so keep everything. This
  // is the path a caller with no viewport takes (see Config).
  if (config_.screenWidth <= 0 || config_.screenHeight <= 0) return true;
  if (pointCount == 0) return false;

  int16_t minX = xs_[0], maxX = xs_[0], minY = ys_[0], maxY = ys_[0];
  for (uint16_t i = 1; i < pointCount; ++i) {
    if (xs_[i] < minX) minX = xs_[i];
    if (xs_[i] > maxX) maxX = xs_[i];
    if (ys_[i] < minY) minY = ys_[i];
    if (ys_[i] > maxY) maxY = ys_[i];
  }

  // All four corners, not two: the projection rotates, so the local box's
  // min/max corners are not the screen box's min/max corners at any heading
  // other than north.
  const int16_t localX[4] = {minX, maxX, minX, maxX};
  const int16_t localY[4] = {minY, minY, maxY, maxY};
  int screenMinX = 0, screenMaxX = 0, screenMinY = 0, screenMaxY = 0;
  for (int i = 0; i < 4; ++i) {
    int16_t sx = 0, sy = 0;
    proj_.projectTileLocal(reader_.originX(), reader_.originY(), localX[i], localY[i], sx, sy);
    if (i == 0) {
      screenMinX = screenMaxX = sx;
      screenMinY = screenMaxY = sy;
      continue;
    }
    if (sx < screenMinX) screenMinX = sx;
    if (sx > screenMaxX) screenMaxX = sx;
    if (sy < screenMinY) screenMinY = sy;
    if (sy > screenMaxY) screenMaxY = sy;
  }

  // A stroke is centred on its line, so ink reaches past the geometry by half
  // the width. The margin is the full width (mapStyleMaxStrokePx) -- being one
  // pixel too generous keeps a way that draws nothing, which costs a little
  // time; being one pixel too tight loses a pixel that should be on the panel.
  const int margin = config_.rejectMarginPx > 0 ? config_.rejectMarginPx : 0;
  screenMinX -= margin;
  screenMinY -= margin;
  screenMaxX += margin;
  screenMaxY += margin;

  return !(screenMaxX < 0 || screenMaxY < 0 || screenMinX >= config_.screenWidth || screenMinY >= config_.screenHeight);
}

bool MapTileSource::nextWayRecord(MapWayRef& out, const bool applyClassMask) {
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

    // The mode filter, applied after the points are read and before they are
    // projected. Reading them is not optional -- they are what advances the
    // stream to the next record -- but projecting them is, and that is the
    // per-point cost. docs/map-data-spec.md, "Mode is a render-time filter".
    if (applyClassMask && (config_.classMask & (1u << (header.classId & 31))) == 0) {
      ++waysFiltered_;
      continue;
    }

    // Then the screen test, on the record's own bounding box, still in tile-local
    // coordinates. The tile range is much larger than the panel -- at the closest
    // rung 250x its area -- so most records cannot draw anything, and the cost of
    // finding that out is four corner projections against up to 256 point ones
    // (docs/optimization/01-render-pipeline.md, step 3).
    //
    // Same rule as the class mask above: the bytes were still read, because
    // reading is what advances the stream. Only the work after it is skipped.
    if (!mayReachScreen(header.pointCount)) {
      ++waysOffScreen_;
      continue;
    }

    for (uint16_t i = 0; i < header.pointCount; ++i) {
      proj_.projectTileLocal(reader_.originX(), reader_.originY(), xs_[i], ys_[i], xs_[i], ys_[i]);
    }
    pointsProjected_ += header.pointCount;

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
    ++pointsProjected_;

    out.x = sx;
    out.y = sy;
    out.rank = header.rank;
    out.name = name_;
    ++placesEmitted_;
    return true;
  }
}
