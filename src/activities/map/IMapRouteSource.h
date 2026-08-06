#pragma once

#include <cstdint>

// The pull interface MapRenderer draws the route through -- the same shape as
// IMapSource, and for the same reason: the renderer must not know how long the
// route is. One point is live at a time, the source projects it, the renderer
// draws the segment from the previous one, nothing accumulates.
//
// Separate from IMapSource rather than a layer inside it, because a route is
// not tiled. IMapSource is an interface over a rectangular range of .tib files;
// the route is one file that is loaded whole and outlives any single viewport
// (docs/map-data-spec.md, "The trip layer is one file, not tiles"). Folding it
// in would mean a tile source that sometimes has a route in it and a route that
// has to pretend to be a tile.
//
// Coordinates are **int32_t screen pixels, not int16_t**. A route is one object
// that can be 200 km long, so in follow mode at 1 m/px its far end is 2e8
// pixels off screen -- which wraps an int16_t into a line drawn across the
// middle of the map. The canvas clips per segment
// (GfxRendererCanvas::drawLine), so an honest off-screen coordinate is cheap
// and a wrapped one is a wrong picture.
class IMapRouteSource {
 public:
  virtual ~IMapRouteSource() = default;

  // Rewinds to the first point. False when there is no route to walk.
  // Rewindable, like IMapSource::beginWays().
  virtual bool beginRoute() = 0;

  // Fills the next point, already projected to screen pixels. False at the end
  // of the route or on a read error -- which ends the polyline where it is, so
  // a truncated read draws a shorter route rather than a wrong one.
  virtual bool nextRoutePoint(int32_t& outX, int32_t& outY) = 0;
};
