#pragma once

#include <cstdint>

#include "MapPointTypes.h"

// One point, projected to screen pixels, as the renderer sees it.
//
// No name in here. A mark is a square with a glyph and, when a condition is
// attached, one corner flag -- it prints no text (docs/map-render-spec.md,
// "Point mark vocabulary"), so carrying a name through the draw walk would be
// bytes nothing reads. The Nearby list, which does print names, goes through
// MapPointQuery instead and fetches a name per printed row.
//
// Coordinates are int32_t screen pixels for the same reason IMapRouteSource
// uses them: a shard is 39 km wide, so at 1 m/px its far corner is 39,000 px
// off screen, and a source that clipped to int16_t would put a mark in the
// middle of the map instead of off it.
struct MapPointRef {
  int32_t x = 0;
  int32_t y = 0;
  MapPointKind kind = MapPointKind::Unknown;
  uint8_t category = 0;
  uint8_t flags = 0;
  // Landmarks only. 0 is most important; on a safety record it means "no rank"
  // and the per-rung cutoff must not read it as a rank at all (MapPointMarks,
  // visible()). A version-1 shard has no rank byte and reads back as 0 here,
  // which is harmless for the same reason: such a file holds no landmarks.
  uint8_t rank = kPointRankNone;

  // The point's name, or nullptr when it has none or the source was not asked
  // for names. **Valid only until the next nextMapPoint() call** -- it points
  // into the source's own buffer, the same contract MapPlaceRef::name has, and
  // for the same reason: a name is a seek and a read per record, so it is
  // fetched into one reusable buffer rather than carried for every point in
  // range (MapPointReader::readName).
  const char* name = nullptr;

  // Metres above the EGM96 geoid, or kPointEleUnknown. Only a few categories
  // carry one and only some of those print it (kLandmarkShowEle) -- a summit
  // is how a walker fixes position on a ridge, a castle's height says nothing.
  // **It is a sentinel, not a low summit**: filter it out before any
  // arithmetic, never feed it in.
  int16_t ele = kPointEleUnknown;
};

// The pull interface MapRenderer draws the point layer through -- the same
// shape as IMapSource and IMapRouteSource, and for the same reason: the
// renderer must not know how many points there are. One point is live at a
// time, the source projects it, nothing accumulates.
//
// Separate from IMapSource rather than a layer inside it, because the points
// are not on the base tile grid: they are z10 shards while the base LOD is
// z11-z13, and they are refreshed on their own (docs/map-data-spec.md, "The
// safety layer is its own file"). Folding them in would mean a tile source
// that sometimes reads a different grid than the one it was configured with.
class IMapPointSource {
 public:
  virtual ~IMapPointSource() = default;

  // Rewinds to the first point of the first shard. False when there is nothing
  // to walk -- no shards for this viewport, or every kind filtered out.
  virtual bool beginMapPoints() = 0;

  // Fills the next point. False at the end of the last shard. A shard that
  // fails its checksum is skipped whole and the walk continues with the next
  // one: a corrupt shard must not be drawn, and one bad file must not hide the
  // eight good ones around it.
  virtual bool nextMapPoint(MapPointRef& out) = 0;
};
