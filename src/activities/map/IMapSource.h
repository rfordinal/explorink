#pragma once

#include <cstdint>

// The pull interface MapRenderer reads geometry through. One record is live
// at a time: the source fills a fixed buffer it owns, the renderer draws
// from it, and the next call overwrites it. Nothing accumulates.
//
// This exists because the renderer must not know how much map there is.
// MapTileReader is already O(1) in tile size, but that buys nothing if the
// layer above materialises every way of every tile into RAM first -- 40.7 KB
// and 1218 heap allocations for one dense z12 tile, ~200 KB for a 3x3
// viewport, on a device with ~380 KB total and 48 KB of it already the
// framebuffer. See docs/map-data-spec.md, "RAM budget".
//
// Pass order is the source's problem, not the renderer's. The renderer asks
// for ways and gets ways, from however many tiles; the source walks
// pass-outer / tile-inner (docs/map-data-spec.md, "A tile is a storage unit,
// not a render unit"). Rendering tile by tile breaks road casings on every
// tile seam, so that order is a correctness requirement.
//
// begin*() is rewindable and may be called more than once. That is how the
// two road passes work: all casings below all white fills means the roads
// layer is streamed twice, once per pass. The second pass is a second seek
// over a ~20 KB layer -- a few milliseconds against the 1800 ms full refresh
// the same viewport reset already pays. Do NOT cache the first pass to skip
// the second; that cache is exactly what this interface removes.

// One way, already projected to screen pixels. xs/ys point into the
// source's own buffer and are valid only until the next nextWay() call.
struct MapWayRef {
  uint8_t classId = 0;
  uint8_t roughness = 0;
  uint16_t flags = 0;
  uint16_t pointCount = 0;
  const int16_t* xs = nullptr;
  const int16_t* ys = nullptr;
};

// One place, already projected to screen pixels. `name` points into the
// source's own buffer, is null-terminated, and may be truncated; it is
// valid only until the next nextPlace() call.
struct MapPlaceRef {
  int16_t x = 0;
  int16_t y = 0;
  uint8_t rank = 0;
  const char* name = "";
};

class IMapSource {
 public:
  virtual ~IMapSource() = default;

  // Rewinds to the first way of the first tile. Returns false if there is
  // nothing to walk at all.
  virtual bool beginWays() = 0;
  // Fills `out` with the next way and returns true; false when the pass is
  // exhausted. A malformed record ends that tile, not the whole pass.
  virtual bool nextWay(MapWayRef& out) = 0;

  // The area layers -- buildings, water, landuse -- are stored as way records
  // like roads (docs/map-data-spec.md, "Layer ids"), so they arrive as
  // MapWayRef too. Three things differ from a road:
  //
  // - `classId` belongs to that layer's own small enum (MapAreaClass.h), not to
  //   the road enum. Water is river/stream/lake, landuse is forest/built-up,
  //   buildings have one class and leave the byte 0.
  // - An **area** is a closed ring: the first point is repeated as the last.
  //   A water record that is not closed is a waterway line. Buildings and
  //   landuse are always areas.
  // - None of them is touched by the mode mask. A lake is not a road class.
  //
  // They are also the expensive layers: buildings alone were 277 KB of the
  // 364 KB a four-tile viewport read (docs/map-data-spec.md, "RAM budget"), so a
  // renderer that does not want one must not call its pass at all rather than
  // read and filter.
  //
  // Landuse carries both forest and built-up areas in one layer, and they are
  // drawn at different depths, so the renderer walks it twice and filters on
  // `classId` -- the same shape as the two road passes.
  virtual bool beginBuildings() = 0;
  virtual bool nextBuilding(MapWayRef& out) = 0;

  virtual bool beginWater() = 0;
  virtual bool nextWater(MapWayRef& out) = 0;

  virtual bool beginLanduse() = 0;
  virtual bool nextLanduse(MapWayRef& out) = 0;

  virtual bool beginPlaces() = 0;
  virtual bool nextPlace(MapPlaceRef& out) = 0;
};

// True when a way record is a closed ring, i.e. an area rather than a line.
// The builder repeats the first point as the last for areas
// (mapbuilder/tiles.py, buildings and water area branches).
inline bool mapWayIsClosedRing(const MapWayRef& way) {
  return way.pointCount >= 4 && way.xs[0] == way.xs[way.pointCount - 1] && way.ys[0] == way.ys[way.pointCount - 1];
}
