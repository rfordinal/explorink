#pragma once

#include <cstddef>
#include <cstdint>

#include "IFileSource.h"
#include "IMapSource.h"
#include "MapLayerBits.h"
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

    // Render-time mode filter: a way is emitted when
    // `classMask & (1u << class_id)`. All ones means "draw everything",
    // which is what the golden test and every pre-P5 caller want.
    // docs/map-data-spec.md, "Mode is a render-time filter" -- the tile set
    // is the same for every mode and is never filtered at build time.
    uint32_t classMask = 0xFFFFFFFFu;

    // The screen the projection targets, and how far a stroke can reach past
    // the geometry that carries it (mapStyleMaxStrokePx, MapStyle.h). Together
    // they let this source drop a record that cannot put a pixel on the panel
    // *before* projecting its points.
    //
    // Worth having because the tile range is far larger than the screen: at the
    // closest rung a z13 tile is 4,892 m against a 480x800 m screen, so a 2x2
    // range covers 250x the visible area and 99.6 % of the buildings walked
    // never draw anything (docs/optimization/01-render-pipeline.md, measured).
    //
    // Zero width or height disables the test, which is what a caller that has
    // no screen (a format probe, a byte-counting tool) wants. Both real callers
    // -- MapActivity and test/map_preview -- set it, so the golden render is
    // what proves the test drops nothing visible.
    int16_t screenWidth = 0;
    int16_t screenHeight = 0;
    int16_t rejectMarginPx = 0;

    // Microsecond clock for the card-time accounting (MapTileReader::setClock).
    // nullptr means the reads are not timed, which is what the host build and
    // every test want -- see ioUs().
    uint32_t (*nowUs)() = nullptr;

    // (tile, layer) pairs already known corrupt, in crc32Failed_'s bit layout.
    // Such a layer is not opened at all and its tile lands on the unavailable
    // mask, so it hatches. Set by a caller re-rendering after a streamed sum
    // came out wrong -- see failedLayerMask().
    MapLayerBits knownBadLayers;
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
  bool beginBuildings() override;
  bool nextBuilding(MapWayRef& out) override;
  bool beginWater() override;
  bool nextWater(MapWayRef& out) override;
  bool beginLanduse() override;
  bool nextLanduse(MapWayRef& out) override;

  bool beginContours() override;
  bool nextContour(MapWayRef& out) override;
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

  // The content identity of the tile at [index], or 0 if that index never
  // opened this frame -- crc32 over the six per-layer crc32s
  // (MapTileReader::contentId(), docs/tile-freshness.md).
  //
  // Free: every value it is built from is already in RAM after the header
  // parse, so collecting it here costs one array store per tile open and no
  // extra read. Nothing else on the device could produce it that cheaply, which
  // is why the freshness check gathers it from the render rather than from a
  // pass of its own.
  //
  // Cleared in begin(), like unavailableMask_ and for the same reason: it
  // describes the frame, not the last pass.
  uint32_t contentIdAt(uint32_t index) const { return index < kMaxTrackedTiles ? contentIds_[index] : 0; }

  // Tile indices contentIdAt() can answer for. Matches unavailableMask()'s own
  // 32-bit cap; the range is never wider (MapViewport::kMaxTiles is 16).
  static constexpr uint32_t kMaxTrackedTiles = 32;

  // Records handed out since begin(), summed across passes. The renderer walks
  // the road layer MapRenderer::kRoadPasses times, so that many times the way
  // count lands here -- that is the streaming design working, not a bug. A
  // caller after "how much map is in the picture" divides by that constant.
  uint32_t waysEmitted() const { return waysEmitted_; }
  uint32_t placesEmitted() const { return placesEmitted_; }

  // Ways read off the card and dropped by Config::classMask. This is the
  // mode filter's own evidence: the same coordinate in two modes reads the
  // same tiles and the same way count, and differs only here.
  uint32_t waysFiltered() const { return waysFiltered_; }

  // Real bytes read from the card across every tile and every pass since
  // begin(), summed from each tile's MapTileReader::bytesRead() as it
  // closes. This is the number the per-layer crc32 split (docs/PROGRESS.md
  // gate 4) is measured against -- milliseconds alone cannot tell a real
  // I/O reduction from a lucky, mostly-cached SD card.
  uint32_t bytesRead() const { return bytesRead_; }

  // Points handed to MapProjection since begin(), across every pass. This is
  // the render path's own unit of work: the projection is a software `double`
  // on this target (docs/optimization/01-render-pipeline.md), so it is the
  // count that fixed point and off-screen rejection are measured against.
  //
  // Frame-scoped, cleared only in begin() -- unlike tilesOpened() and
  // tilesUnavailable(), which startPass() resets and which therefore describe
  // whichever pass ran last.
  uint32_t pointsProjected() const { return pointsProjected_; }

  // Ways read off the card and dropped because their own bounding box cannot
  // reach the screen (Config::screenWidth). Frame-scoped, cleared in begin().
  //
  // Distinct from waysFiltered(), which is the travel-mode class filter: this
  // one is about where a way is, that one about what it is. A high number here
  // is the tile grid being much larger than the panel, not a bug -- see
  // docs/optimization/01-render-pipeline.md, step 3.
  uint32_t waysOffScreen() const { return waysOffScreen_; }

  // Microseconds spent inside the card since begin(): open, seek, read, summed
  // across every tile and every pass. Zero when Config::nowUs is null.
  //
  // This is the number that decides whether the next move is a smaller tile or a
  // cheaper draw. The per-layer render times cannot answer it: a layer reads from
  // the card as it draws, so its milliseconds are both costs added together.
  uint32_t ioUs() const { return ioUs_; }

  // Cells and bytes the cell index let this frame skip, summed over every layer
  // and pass. Zero when no layer read went through the index -- which is every
  // rung that draws no buildings, since buildings are the only layer big enough
  // for the writer to index (mapbuilder/tiles.py, MIN_INDEXED_LAYER_BYTES).
  uint32_t cellsSkipped() const { return cellsSkipped_; }
  uint32_t bytesSkippedByIndex() const { return bytesSkippedByIndex_; }

  // Layer crc32 checks skipped because that (tile, layer) pair already passed
  // in this frame. Frame-scoped. The evidence that the repeat check is gone:
  // with the renderer's seven passes over six layers, this should be non-zero
  // on any frame that draws roads or landuse, and every skip is one full read
  // of that layer's bytes not paid.
  uint32_t crc32Skipped() const { return crc32Skipped_; }

  // Layers whose streamed crc32 came out wrong this frame. Non-zero means this
  // frame drew records off a corrupt layer before the corruption was known, so
  // the caller must draw the frame again -- the tile is now on the unavailable
  // mask and the second attempt hatches it instead
  // (MapTileReader::LayerCheck, and MapActivity's redraw).
  //
  // Frame-scoped. Expected to be zero always; it exists so a card going bad is
  // visible in `info` rather than only as a strange picture.
  uint32_t corruptLayers() const { return corruptLayers_; }

  // The (tile, layer) pairs that failed, in the same bit layout Config takes
  // back as knownBadLayers. This is how a caller re-renders without the geometry
  // it has just been handed: begin() clears the frame's state, so the mask has to
  // travel back in rather than survive inside.
  MapLayerBits failedLayerMask() const { return crc32Failed_; }

 private:
  bool startPass(MapTileReader::Layer layer);
  // The shared way-record walk. `applyClassMask` is false for buildings and
  // water: their class_id is always 0, so filtering them against a road mode
  // mask would be filtering on a field that carries nothing.
  bool nextWayRecord(MapWayRef& out, bool applyClassMask);
  // Can a way whose tile-local points are already in xs_/ys_ put ink on the
  // screen? Projects the four corners of its local bounding box -- four
  // projections instead of up to kMaxWayPoints of them -- inflates by
  // Config::rejectMarginPx and tests against the screen rect.
  //
  // Conservative by construction: the rotation maps the local bbox to a rotated
  // rectangle, and the axis-aligned bbox of the four projected corners contains
  // that rectangle, so a "no" is always a real no. Every segment of the way lies
  // inside its own bbox, so no drawing can escape it either.
  bool mayReachScreen(uint16_t pointCount) const;
  // Works out `screenBox*_`: the screen rectangle, inflated by the stroke
  // margin, expressed in the current tile's own local coordinates. Called once
  // per tile open, so every record afterwards is tested with integer compares
  // and no projection at all.
  void computeScreenBoxForTile();
  // The cell window the screen box covers, in the 8x8 grid a version-3 index
  // divides the tile into (MapTileReader::kCellGrid). False when there is no
  // screen box to derive it from, in which case the caller reads whole layers.
  bool screenCellWindow(uint32_t& col0, uint32_t& col1, uint32_t& row0, uint32_t& row1) const;
  // Bit index for a (tile index, layer id) pair in crc32Validated_.
  //
  // The stride is MapLayerBits::kSlotsPerTile, and it has to be: it was a
  // hardcoded 7 while layer ids ran 1..6, which left exactly one spare slot per
  // tile. Relief took it. The next layer id -- 8, which is the whole point of
  // raising kMaxLayers to 15 -- would have aliased onto the *next tile's* water
  // layer at stride 7: bit 7t+8 == 7(t+1)+1. That reads as "already validated",
  // so a layer whose crc32 was never checked gets streamed with skipCrc32, and
  // symmetrically one tile's failure hatches a neighbour's water. Silent, and no
  // test could have caught it before layer 8 existed.
  //
  // MapLayerBits already sizes itself off kSlotsPerTile and static_asserts the
  // product against its bit count, so this is now one constant rather than two
  // that have to be remembered together.
  static uint32_t crcBitFor(uint32_t tileIndex, MapTileReader::Layer layer) {
    return tileIndex * MapLayerBits::kSlotsPerTile + static_cast<uint32_t>(layer);
  }
  // The guard that was missing. MapLayerBits already asserts the table is big
  // enough; nothing asserted that the stride is wider than the highest layer id,
  // which is the half that actually aliases.
  static_assert(MapTileReader::kMaxLayers < MapLayerBits::kSlotsPerTile,
                "crcBitFor's stride must exceed the highest layer id, or one tile's top layer shares a bit with "
                "the next tile's first");
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
  uint32_t contentIds_[kMaxTrackedTiles] = {};
  uint32_t waysEmitted_ = 0;
  uint32_t waysFiltered_ = 0;
  uint32_t placesEmitted_ = 0;
  uint32_t bytesRead_ = 0;
  uint32_t pointsProjected_ = 0;
  uint32_t waysOffScreen_ = 0;
  uint32_t ioUs_ = 0;

  // The screen, in this tile's local coordinates. int32 rather than int16: the
  // screen box is a *superset* of the visible area at any heading other than a
  // multiple of 90 degrees, and at the coarse rungs it can run well past the
  // int16 range a tile's own points live in.
  //
  // Valid only while a tile is open, and only when the screen test is on
  // (Config::screenWidth). screenBoxValid_ false means "keep everything", which
  // is the safe answer if the inverse projection ever produced nonsense.
  int32_t screenBoxMinX_ = 0;
  int32_t screenBoxMaxX_ = 0;
  int32_t screenBoxMinY_ = 0;
  int32_t screenBoxMaxY_ = 0;
  bool screenBoxValid_ = false;

  // One bit per (tile index, layer id) pair that has passed its crc32 in this
  // frame. 16 tiles at most (MapViewport::kMaxTiles) and a layer id can be
  // 1..15 (MapTileReader::kMaxLayers), so 16 x 16 = 256 bits -- four words, held
  // by MapLayerBits, which says why it is not one. Cleared in begin(), exactly
  // like unavailableMask_.
  //
  // Why it is safe to trust: a file on the card cannot become corrupt between
  // two passes of one viewport reset, and the whole point of the pass-outer
  // walk is that the same layer is opened more than once per frame
  // (MapRenderer::kRoadPasses, plus landuse's built-up and forest walks).
  MapLayerBits crc32Validated_;
  uint32_t crc32Skipped_ = 0;
  // (tile, layer) pairs whose streamed sum failed this frame. Checked before a
  // layer is opened, so the frame that follows the failure does not stream the
  // same garbage a second time.
  MapLayerBits crc32Failed_;
  uint32_t corruptLayers_ = 0;
  uint32_t cellsSkipped_ = 0;
  uint32_t bytesSkippedByIndex_ = 0;
  // Which tile index the open reader belongs to, so a verdict that arrives when
  // the layer runs dry can be attributed to the right pair.
  uint32_t currentTileIndex_ = 0;

  // The one live record. Overwritten by every nextWay()/nextPlace().
  int16_t xs_[MapTileReader::kMaxWayPoints];
  int16_t ys_[MapTileReader::kMaxWayPoints];
  char name_[kMaxNameLen];
  char path_[kMaxPathLen];
};
