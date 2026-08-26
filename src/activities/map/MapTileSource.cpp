#include "MapTileSource.h"

#include <cmath>
#include <cstdio>

#include "MapTileGrid.h"

MapTileSource::MapTileSource(IFileSource& file, const MapProjection& proj) : file_(file), proj_(proj) {
  name_[0] = '\0';
  path_[0] = '\0';
}

MapTileSource::~MapTileSource() { closeCurrentTile(); }

void MapTileSource::begin(const Config& config) {
  closeCurrentTile();
  config_ = config;
  reader_.setClock(config_.nowUs);
  rowSpan_ = config_.row1 >= config_.row0 ? (config_.row1 - config_.row0 + 1) : 0;
  const uint32_t colSpan = config_.col1 >= config_.col0 ? (config_.col1 - config_.col0 + 1) : 0;
  tileCount_ = colSpan * rowSpan_;
  nextTileIndex_ = 0;
  tilesOpened_ = 0;
  tilesUnavailable_ = 0;
  unavailableMask_ = 0;
  for (uint32_t i = 0; i < kMaxTrackedTiles; ++i) contentIds_[i] = 0;
  waysEmitted_ = 0;
  waysFiltered_ = 0;
  placesEmitted_ = 0;
  bytesRead_ = 0;
  pointsProjected_ = 0;
  waysOffScreen_ = 0;
  ioUs_ = 0;
  crc32Validated_.clear();
  crc32Skipped_ = 0;
  // Carried in rather than cleared: a caller re-rendering after corruption has to
  // be able to say "not this layer again" (Config::knownBadLayers).
  crc32Failed_ = config_.knownBadLayers;
  cellsSkipped_ = 0;
  bytesSkippedByIndex_ = 0;
  corruptLayers_ = 0;
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
  ioUs_ += reader_.takeIoUs();
  if (tileOpen_) {
    // The streamed sum is only complete once the layer has run dry, which is
    // exactly when the walk closes the tile. Passed marks the pair so later
    // passes skip the check; Failed marks the tile unavailable so the caller's
    // second attempt hatches it instead of drawing it again.
    const uint32_t bit = crcBitFor(currentTileIndex_, layer_);
    switch (reader_.layerCheck()) {
      case MapTileReader::LayerCheck::Passed:
        // Only a whole-layer read proves the layer's own sum. On the cell path
        // layerCheck() speaks for the last cell, and marking the layer validated
        // off that would let a later whole-layer pass skip a check that never
        // happened (MapTileReader::readingCells).
        if (!reader_.readingCells()) crc32Validated_.set(bit);
        break;
      case MapTileReader::LayerCheck::Failed:
        crc32Failed_.set(bit);
        ++corruptLayers_;
        ++tilesUnavailable_;
        if (currentTileIndex_ < 32) unavailableMask_ |= (1u << currentTileIndex_);
        break;
      case MapTileReader::LayerCheck::NotFinished:
      case MapTileReader::LayerCheck::Skipped:
        // Not finished: the walk stopped before the end of the layer, so
        // nothing is known and the pair stays unmarked -- the next pass checks
        // it in full, as before. Skipped: already known good this frame.
        break;
    }
  }
  if (tileOpen_) {
    reader_.close();
    tileOpen_ = false;
  }
  // The box belongs to the tile that just closed. Leaving it set would test the
  // next tile's records against the previous tile's frame.
  screenBoxValid_ = false;
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

    // The header is parsed, so every layer's crc32 is in RAM and the tile's
    // content identity is one crc32 away. Recorded here rather than in a pass of
    // its own: this is the only moment on the device where it is free
    // (docs/tile-freshness.md).
    if (index < kMaxTrackedTiles) contentIds_[index] = reader_.contentId();

    // The origin is known the moment the header is parsed, so the screen box can
    // be brought into this tile's coordinates before any layer is opened -- the
    // cell window is derived from it.
    computeScreenBoxForTile();

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
      ioUs_ += reader_.takeIoUs();
      reader_.close();
      continue;
    }

    // Check this layer's crc32 once per frame, not once per pass. The renderer
    // walks roads twice and landuse twice (MapRenderer::kRoadPasses), and every
    // walk used to re-read the whole layer just to re-check a sum that cannot
    // have changed -- a file does not rot between two passes of one frame.
    // docs/optimization/02-tile-io.md, step 2.
    // The cell index, when this layer has one and there is a screen box to derive
    // a window from. Only buildings are ever indexed in practice, so this is the
    // path the closest rung takes and no other.
    //
    // No crc32Validated_ bookkeeping on this path, deliberately: that bitmap
    // records "this layer's own sum passed", and a cell read never computes that
    // sum -- it checks one per cell instead. Marking the layer validated off a
    // cell read would let a later whole-layer pass skip a check it never had.
    uint32_t cellCol0 = 0, cellCol1 = 0, cellRow0 = 0, cellRow1 = 0;
    if (reader_.layerHasIndex(layer_) && screenCellWindow(cellCol0, cellCol1, cellRow0, cellRow1)) {
      if (!reader_.beginLayerCells(layer_, cellCol0, cellCol1, cellRow0, cellRow1)) {
        ++tilesUnavailable_;
        if (index < 32) unavailableMask_ |= (1u << index);
        bytesRead_ += reader_.takeBytesRead();
        ioUs_ += reader_.takeIoUs();
        reader_.close();
        continue;
      }
      ++tilesOpened_;
      tileOpen_ = true;
      currentTileIndex_ = index;
      cellsSkipped_ += reader_.cellsSkipped();
      bytesSkippedByIndex_ += reader_.bytesSkipped();
      return true;
    }

    const uint32_t crcBit = crcBitFor(index, layer_);
    // Known bad from an earlier pass in this same frame: do not stream it again.
    // The tile is already on the unavailable mask, so it hatches.
    if (crc32Failed_.test(crcBit)) {
      // Known bad, either from an earlier pass of this frame or handed in by a
      // caller re-rendering after the first attempt drew it. Do not stream it
      // again; hatch the tile instead.
      ++tilesUnavailable_;
      if (index < 32) unavailableMask_ |= (1u << index);
      bytesRead_ += reader_.takeBytesRead();
      ioUs_ += reader_.takeIoUs();
      reader_.close();
      continue;
    }
    const bool alreadyValidated = crc32Validated_.test(crcBit);
    if (alreadyValidated) ++crc32Skipped_;
    if (!reader_.beginLayer(layer_, alreadyValidated)) {
      // Present per the directory, but its own crc32 failed. hasLayer()
      // already ruled out "absent" above, so this is corrupt data, not an
      // empty layer -- it must count as unavailable and hatch, the same as
      // a tile that failed to open at all.
      ++tilesUnavailable_;
      if (index < 32) unavailableMask_ |= (1u << index);
      bytesRead_ += reader_.takeBytesRead();
      ioUs_ += reader_.takeIoUs();
      reader_.close();
      continue;
    }

    // No longer marked here: the sum is now folded out of the record stream and
    // the verdict only exists once the layer has been walked to its end, which
    // closeCurrentTile() is what sees (MapTileReader::LayerCheck).
    ++tilesOpened_;
    tileOpen_ = true;
    currentTileIndex_ = index;
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

bool MapTileSource::beginContours() { return startPass(MapTileReader::Layer::Relief); }

// `false`: no mode mask. The mask is a bitmap over the road class enum and a
// contour's class byte belongs to MapContourClass, so testing it would filter
// contours by whether some unrelated road class is drawn.
bool MapTileSource::nextContour(MapWayRef& out) { return nextWayRecord(out, false); }

void MapTileSource::computeScreenBoxForTile() {
  screenBoxValid_ = false;
  if (config_.screenWidth <= 0 || config_.screenHeight <= 0) return;

  // The screen rectangle, inverse-projected into Mercator and then into this
  // tile's local frame. Four corners, because the projection rotates: at any
  // heading other than a multiple of 90 degrees the screen is a tilted rectangle
  // over the tile, and there is no single row or column that separates visible
  // from not.
  //
  // The axis-aligned box around those four corners is a **superset** of the
  // tilted rectangle, so this keeps slightly more than strictly necessary --
  // about 2x the area at 45 degrees. That is the price of the test being four
  // integer compares per record instead of four software `double` projections.
  // Measured 2026-08-06: paying it per record made roads 300-1,200 ms per rung
  // *slower*, because a long road's box reaches the screen almost every time and
  // the projections were spent to learn nothing.
  const int16_t margin = config_.rejectMarginPx > 0 ? config_.rejectMarginPx : 0;
  const int16_t corners[4][2] = {
      {static_cast<int16_t>(-margin), static_cast<int16_t>(-margin)},
      {static_cast<int16_t>(config_.screenWidth + margin), static_cast<int16_t>(-margin)},
      {static_cast<int16_t>(-margin), static_cast<int16_t>(config_.screenHeight + margin)},
      {static_cast<int16_t>(config_.screenWidth + margin), static_cast<int16_t>(config_.screenHeight + margin)},
  };

  for (int i = 0; i < 4; ++i) {
    double mercX = 0.0, mercY = 0.0;
    proj_.screenToMerc(corners[i][0], corners[i][1], mercX, mercY);
    // Inverse of MapProjection::projectTileLocal: localX = mercX - originX,
    // localY = originY - mercY. Same relationship, read backwards.
    const double localX = mercX - static_cast<double>(reader_.originX());
    const double localY = static_cast<double>(reader_.originY()) - mercY;
    // Round outwards, never inwards: a truncation towards zero here could clip a
    // way that touches the very edge of the screen.
    const int32_t loX = static_cast<int32_t>(std::floor(localX));
    const int32_t hiX = static_cast<int32_t>(std::ceil(localX));
    const int32_t loY = static_cast<int32_t>(std::floor(localY));
    const int32_t hiY = static_cast<int32_t>(std::ceil(localY));
    if (i == 0) {
      screenBoxMinX_ = loX;
      screenBoxMaxX_ = hiX;
      screenBoxMinY_ = loY;
      screenBoxMaxY_ = hiY;
      continue;
    }
    if (loX < screenBoxMinX_) screenBoxMinX_ = loX;
    if (hiX > screenBoxMaxX_) screenBoxMaxX_ = hiX;
    if (loY < screenBoxMinY_) screenBoxMinY_ = loY;
    if (hiY > screenBoxMaxY_) screenBoxMaxY_ = hiY;
  }
  screenBoxValid_ = true;
}

bool MapTileSource::screenCellWindow(uint32_t& col0, uint32_t& col1, uint32_t& row0, uint32_t& row1) const {
  if (!screenBoxValid_) return false;

  // The screen box is already in this tile's local coordinates
  // (computeScreenBoxForTile), so the window is a division. Cell size comes from
  // the tile's own span, and the grid has to be the one mapbuilder wrote with
  // (MapTileReader::kCellGrid mirrors tiles.py's CELL_GRID).
  const double span = MapTileGrid::kWorldSizeM / static_cast<double>(1u << config_.z);
  const double cell = span / static_cast<double>(MapTileReader::kCellGrid);
  const int32_t last = static_cast<int32_t>(MapTileReader::kCellGrid) - 1;

  const auto slot = [cell, last](int32_t local) -> uint32_t {
    if (local < 0) return 0;
    const int32_t index = static_cast<int32_t>(static_cast<double>(local) / cell);
    return static_cast<uint32_t>(index > last ? last : index);
  };

  col0 = slot(screenBoxMinX_);
  col1 = slot(screenBoxMaxX_);
  row0 = slot(screenBoxMinY_);
  row1 = slot(screenBoxMaxY_);
  return true;
}

bool MapTileSource::mayReachScreen(const uint16_t pointCount) const {
  // No screen configured, or no box for this tile: keep everything. Both are the
  // safe answer, and the first is what a caller with no viewport wants (Config).
  if (!screenBoxValid_) return true;
  if (pointCount == 0) return false;

  // Integer only, and no projection: the screen has already been brought into
  // these coordinates once for the whole tile (computeScreenBoxForTile).
  int16_t minX = xs_[0], maxX = xs_[0], minY = ys_[0], maxY = ys_[0];
  for (uint16_t i = 1; i < pointCount; ++i) {
    if (xs_[i] < minX) minX = xs_[i];
    if (xs_[i] > maxX) maxX = xs_[i];
    if (ys_[i] < minY) minY = ys_[i];
    if (ys_[i] > maxY) maxY = ys_[i];
  }

  return !(maxX < screenBoxMinX_ || minX > screenBoxMaxX_ || maxY < screenBoxMinY_ || minY > screenBoxMaxY_);
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
