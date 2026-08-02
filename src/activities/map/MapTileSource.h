#pragma once

#include <cstddef>
#include <cstdint>

#include "IFileSource.h"
#include "IMapSource.h"
#include "MapProjection.h"
#include "MapTileReader.h"

// IMapSource over a rectangular range of .tib tiles on the card. This is
// where the tile set lives, so the renderer never learns that tiles exist:
// it asks for ways, and gets ways from however many tiles the viewport
// touches, walked pass-outer / tile-inner (docs/map-data-spec.md, "A tile is
// a storage unit, not a render unit").
//
// Allocates nothing, ever. Everything it needs is a member: one
// MapTileReader (its own fixed 4 KB stream buffer), one way's worth of
// point scratch, one place name buffer, one path buffer. That makes the
// whole streaming path's RAM cost sizeof(MapTileSource), a compile-time
// number that does not move with tile count or tile density.
//
// The object is ~5 KB, far past the 256-byte stack rule in CLAUDE.md, so the
// caller must own it as a member or heap-allocate it with makeUniqueNoThrow.
// It is not a local.
class MapTileSource : public IMapSource {
 public:
  static constexpr size_t kMaxPathLen = 160;
  static constexpr size_t kMaxNameLen = 64;

  struct Config {
    // SD root holding base/<z>/<col>/<row>.tib. Not copied -- it must
    // outlive the source.
    const char* rootDir = nullptr;
    uint8_t z = 0;
    uint32_t col0 = 0;
    uint32_t row0 = 0;
    uint32_t col1 = 0;
    uint32_t row1 = 0;
  };

  // `file` is reused for every tile: opened, streamed, closed, reopened for
  // the next one. `proj` must stay valid and unchanged for the source's
  // whole life -- it is the viewport this geometry is being projected into.
  MapTileSource(IFileSource& file, const MapProjection& proj);
  ~MapTileSource() override;

  // No I/O. Just fixes which tiles this source covers.
  void begin(const Config& config);

  bool beginWays() override;
  bool nextWay(MapWayRef& out) override;
  bool beginPlaces() override;
  bool nextPlace(MapPlaceRef& out) override;

  // Counted over the pass that is running or most recently ran, so they
  // describe one walk of the tile set rather than a growing total. A tile
  // that fails crc32 counts as unavailable: the renderer must hatch it, not
  // draw its bytes (docs/map-data-spec.md, "Refreshing map data").
  uint32_t tilesOpened() const { return tilesOpened_; }
  uint32_t tilesUnavailable() const { return tilesUnavailable_; }

  // Bit per tile index in the configured range, column-major, same order
  // advanceToNextTile() walks it and same order MapViewport::TileRange
  // indexes it. Set means the tile was absent, truncated or crc32-mismatched
  // on at least one pass, which is what the caller hatches.
  //
  // Unlike the counters above this accumulates across passes and is cleared
  // only by begin(): the caller learns which tiles are missing from the
  // render itself, so nothing pays a third read of every tile just to ask.
  uint32_t unavailableMask() const { return unavailableMask_; }

  // Records handed out since begin(), summed across passes. Two road passes
  // will report twice the way count -- that is the streaming design working,
  // not a bug.
  uint32_t waysEmitted() const { return waysEmitted_; }
  uint32_t placesEmitted() const { return placesEmitted_; }

 private:
  bool startPass(MapTileReader::Layer layer);
  // Opens the next tile in the range that actually has the current layer.
  // Returns false when the range is exhausted.
  bool advanceToNextTile();
  void closeCurrentTile();
  void buildPath(uint32_t col, uint32_t row);

  IFileSource& file_;
  const MapProjection& proj_;
  Config config_;

  MapTileReader reader_;
  MapTileReader::Layer layer_ = MapTileReader::Layer::Roads;
  uint32_t tileCount_ = 0;
  uint32_t rowSpan_ = 0;
  uint32_t nextTileIndex_ = 0;
  bool tileOpen_ = false;

  uint32_t tilesOpened_ = 0;
  uint32_t tilesUnavailable_ = 0;
  uint32_t unavailableMask_ = 0;
  uint32_t waysEmitted_ = 0;
  uint32_t placesEmitted_ = 0;

  // The one live record. Overwritten by every nextWay()/nextPlace().
  int16_t xs_[MapTileReader::kMaxWayPoints];
  int16_t ys_[MapTileReader::kMaxWayPoints];
  char name_[kMaxNameLen];
  char path_[kMaxPathLen];
};
