#pragma once

#include <cstdint>

#include "IFileSource.h"
#include "IMapRouteSource.h"
#include "MapProjection.h"
#include "MapRouteFit.h"
#include "MapRouteReader.h"

// IMapRouteSource over one .tir file on the card -- the loaded route, for as
// long as the map screen is up.
//
// Allocates nothing. Everything it needs is a member: one MapRouteReader with
// its own fixed 1 KB stream buffer, one path buffer. So the route path's whole
// RAM cost is sizeof(MapRouteSource), a compile-time number that does not move
// with route length. Same rule and same reasoning as MapTileSource.
//
// About 1.2 KB, past CLAUDE.md's 256-byte stack rule, so the owner holds it as a
// member or heap-allocates it with makeUniqueNoThrow. Never a local.
//
// ## The file stays open
//
// load() opens the file, checks both checksums, and leaves it open. Every
// viewport reset then seeks back to the first point rather than reopening --
// a route is read once per reset, and the card does not change underneath a
// session. This is the one difference from MapTileSource, which reopens because
// each tile is a different file.
//
// The `file` handed in must therefore be this source's own, not shared with the
// tile source: both are streaming during a render, and one seek cursor cannot
// serve two readers.
class MapRouteSource final : public IMapRouteSource {
 public:
  static constexpr size_t kMaxPathLen = 160;

  // `proj` must stay valid for this source's whole life and must not change
  // part-way through a render -- it is the viewport the route is projected into,
  // read live, exactly as MapTileSource reads it.
  MapRouteSource(IFileSource& file, const MapProjection& proj);

  // Opens `path` (absolute, on the card), validates the header and then the
  // point array. False means nothing is loaded and nothing will be drawn: a
  // route that fails its crc must not be drawn at all, because half a route
  // ends somewhere it does not (../../../docs/route-file-spec.md).
  bool load(const char* path);
  void unload();

  bool isLoaded() const { return loaded_; }
  // Empty string when nothing is loaded, so a caller can print it unguarded.
  const char* name() const { return loaded_ ? reader_.name() : ""; }
  const char* path() const { return loaded_ ? path_ : ""; }
  uint32_t pointCount() const { return loaded_ ? reader_.pointCount() : 0; }
  uint32_t buildEpoch() const { return loaded_ ? reader_.buildEpoch() : 0; }

  // One streaming pass over the whole route, working out the rung, heading and
  // anchor that show it all (MapRouteFit.h). False when nothing is loaded or the
  // read failed part-way -- a fit computed from half a route would frame the
  // wrong thing, which is worse than not framing at all.
  bool computeFit(int screenWidth, int screenHeight, MapRouteFit::Result& out);

  bool beginRoute() override;
  bool nextRoutePoint(int32_t& outX, int32_t& outY) override;

  // Points handed out and bytes read since load(), for the debug readout. The
  // route is re-read once per viewport reset, so these say whether that read is
  // worth worrying about next to the tiles'.
  uint32_t pointsEmitted() const { return pointsEmitted_; }
  uint32_t bytesRead() const { return reader_.bytesRead(); }

 private:
  IFileSource& file_;
  const MapProjection& proj_;
  MapRouteReader reader_;
  // A member, not a local in computeFit(): MapRouteFit carries 16 headings'
  // worth of accumulators and is over 400 bytes, past CLAUDE.md's 256-byte
  // stack cap. This object is already heap-allocated by whoever owns it, so
  // holding it here costs no allocation and no stack.
  MapRouteFit fit_;
  bool loaded_ = false;
  uint32_t pointsEmitted_ = 0;
  char path_[kMaxPathLen] = {};
};
