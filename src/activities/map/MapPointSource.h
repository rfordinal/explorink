#pragma once

#include <cstddef>
#include <cstdint>

#include "IFileSource.h"
#include "IMapPointSource.h"
#include "MapPointReader.h"
#include "MapPointShards.h"
#include "MapProjection.h"

// IMapPointSource over the .tip shards on the card -- the point layer for one
// viewport.
//
// Allocates nothing. One MapPointReader with its own 1 KB stream buffer, one
// path buffer, and a cursor over the shard range. So the layer's whole RAM cost
// is sizeof(MapPointSource), a compile-time number that does not move with how
// many points are in range. Same rule as MapTileSource and MapRouteSource.
//
// About 1.3 KB, past CLAUDE.md's 256-byte stack rule, so the owner holds it as a
// member or heap-allocates it with makeUniqueNoThrow. Never a local.
//
// ## One shard open at a time
//
// A viewport touches at most 2x2 shards at the widest rung (a z10 shard is
// 39 km of Mercator against a 21.6x36 km panel at the coarsest rung), and the
// walk opens them one after another rather than holding nine readers. The
// `file` handed in must be this source's own: the tile source is streaming at
// the same time, and one seek cursor cannot serve two readers.
//
// ## Filters are masks, applied before projecting
//
// `kindMask` and `categoryMask` are the render-time filter, exactly as
// MapTileSource's `classMask` is for ways -- the shards are the same for every
// mode and are never filtered at build time. `Nearby -> Show on map` turns one
// category on by setting its bit; nothing on the card changes.
//
// A shard whose header says it holds no kind this mask wants is skipped without
// reading a record (MapPointReader::kindsPresent).
class MapPointSource final : public IMapPointSource {
 public:
  static constexpr size_t kMaxPathLen = 160;

  struct Config {
    // SD root holding points/10/<col>/<row>.tip. Not copied -- it must outlive
    // the source.
    const char* rootDir = nullptr;

    // Which shards to walk. Normally MapPointShards::rangeForMercBbox() of the
    // viewport, padded by the mark's own half-width so a square whose centre is
    // just off screen still draws.
    MapPointShards::Range range;

    // Bit N set = draw MapPointKind N. Default: safety only, because landmarks
    // are T-305 and nothing writes them yet.
    uint8_t kindMask = 1u << static_cast<uint8_t>(MapPointKind::Safety);

    // Bit N set = draw category id N (per kind: the safety categories are
    // MapSafetyCategory). All ones means every category, which is what the
    // golden render and a "show everything" debug view want.
    uint16_t categoryMask = 0xFFFFu;

    // The panel, and how far a mark can reach past its own centre. Together
    // they drop a point that cannot put a pixel on screen before it is
    // projected. Zero width or height disables the test -- what a probe with no
    // screen wants.
    int16_t screenWidth = 0;
    int16_t screenHeight = 0;
    int16_t rejectMarginPx = 0;
  };

  // `proj` must stay valid for this source's whole life and must not change
  // part-way through a walk -- the viewport the points are projected into, read
  // live, exactly as MapTileSource reads it.
  MapPointSource(IFileSource& file, const MapProjection& proj);

  // Points this source at a shard range and a filter. Called once per frame,
  // the same shape as MapTileSource::begin(): the object is allocated once and
  // re-aimed, because the viewport moves and the category filter is a UI
  // decision that changes between frames (`Nearby -> Show on map`).
  void begin(const Config& config);

  bool beginMapPoints() override;
  bool nextMapPoint(MapPointRef& out) override;

  // Points emitted and shards opened since the last beginMapPoints(), plus real
  // bytes read. The layer is re-read once per viewport reset, so these say
  // whether it is worth worrying about next to the tiles' own reads.
  uint32_t pointsEmitted() const { return pointsEmitted_; }
  uint32_t pointsDropped() const { return pointsDropped_; }
  uint32_t shardsOpened() const { return shardsOpened_; }
  uint32_t shardsMissing() const { return shardsMissing_; }
  // Shards that opened and then failed their checksum. Not the same as missing:
  // a missing shard is an area nobody built, a corrupt one is a file that lied.
  uint32_t shardsCorrupt() const { return shardsCorrupt_; }
  uint32_t bytesRead() const { return bytesRead_; }

 private:
  // Opens the next shard in the range whose header passes, leaving it ready to
  // walk. False when the range is exhausted.
  bool openNextShard();
  bool wanted(const MapPointReader::Record& record) const;

  IFileSource& file_;
  const MapProjection& proj_;
  Config config_;
  MapPointReader reader_;

  // Cursor over the shard range, as a flat index so one counter walks a 2-D
  // range without a nested loop across calls.
  uint32_t shardIndex_ = 0;
  uint32_t shardCount_ = 0;
  bool shardOpen_ = false;

  uint32_t pointsEmitted_ = 0;
  uint32_t pointsDropped_ = 0;
  uint32_t shardsOpened_ = 0;
  uint32_t shardsMissing_ = 0;
  uint32_t shardsCorrupt_ = 0;
  uint32_t bytesRead_ = 0;

  char path_[kMaxPathLen] = {};
};
