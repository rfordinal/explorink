#pragma once

#include <cstdint>

#include "IMapCanvas.h"
#include "IMapSource.h"
#include "MapHeading.h"

// Everything the renderer needs that is not geometry. Small and resident by
// definition -- the marker is one point and the heading is one byte. All
// map geometry arrives through IMapSource, one record at a time, and is
// never held (see IMapSource.h for why).
//
// Screen-space (pixels), not geo-coordinates -- MapProjection does that
// conversion inside the source.
struct MapViewState {
  int16_t markerX = 0;
  int16_t markerY = 0;
  MapHeading heading = MapHeading::N;
};

// Draws base map (roads, place dots) then the position marker, in that
// order, onto whatever IMapCanvas it's given. No hardware/HAL dependency --
// this is what both the native preview and MapActivity call.
//
// render() pulls: it holds no geometry of its own, so its RAM cost does not
// move with how much map is on screen.
class MapRenderer {
 public:
  static void render(IMapCanvas& canvas, IMapSource& source, const MapViewState& state);

 private:
  static void drawMarker(IMapCanvas& canvas, int16_t x, int16_t y, MapHeading heading);
};
